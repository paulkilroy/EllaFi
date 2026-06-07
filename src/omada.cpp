/**
 * Omada API Parameter Sources:
 *
 *                                          Omada    WiFi    Hotspot
 *   Field             Used By              Redir    Client  Client   Hardcoded  Other
 *   ----------------  -------------------  -------  ------  -------  ---------  ----------
 *   clientMac *       Auth                 ✓        ✓       ✓
 *   apMac *           Auth                 ✓        ✓
 *   ssidName *        Auth                 ✓        ✓       ✓
 *   radioId *         Auth                 ✓        ✓
 *   ip *              Cache lookup                  ✓†      ✓
 *   start *           UI timer                              ✓
 *   end *             Session expiry                        ✓
 *   site (name)       Auth                 ✓                                      Sites API
 *   time              Auth                                                        Calculated
 *   authType          Auth                          ✓       ✓        OMADA_AUTH_TYPE_EXTERNAL_PORTAL (4)
 *   id (client) *     Extend, Disconnect                    ✓
 *   period            Extend                                                      Calculated
 *   pausedRemaining * Pause/resume                                                Flash (LittleFS)
 *
 *   *  = stored in HOTSPOT_SESSION_CACHE (SessionParams)
 *   †  = only present in WiFi Client response when client is currently connected
 */

#include "omada.h"
#include "globals.h"
#include "files.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_log.h>
#include "logger.h"

static const char* TAG = "EllaFi";

// ---- Omada config globals ----
// Populated by loadConfig() in main.cpp at startup.

String CONTROLLER_BASE_URL;
String CONTROLLER_ID;
String SITE_ID;
String SITE_NAME;
String ADMIN_USERNAME;
String ADMIN_PASSWORD;

static const unsigned long CONTROLLER_SESSION_MAX_AGE_MILLIS = 3600000;

volatile int VOUCHER_ACTIVE_COUNT = 0;
static unsigned long g_lastCacheRefreshMs = 0;

// ---- Shared credential store ----
// Login result is stored here. API functions copy what they need under a brief
// lock, then make their HTTP calls independently — enabling true concurrency.
// The mutex is held only during: (a) credential copy, or (b) re-login.

struct OmadaCredentials {
  String csrfToken;
  String sessionId;  // raw TPOMADA_SESSIONID cookie value
  unsigned long loginMs = 0;
};

static OmadaCredentials g_creds;
static SemaphoreHandle_t g_credsMutex;

// Returns a credential snapshot, logging in first if the session is stale.
// Mutex is held for the copy (microseconds) or for a re-login (≤ 2s, once/hour).
// Callers use the snapshot to build their own local HTTPClient — no shared state
// during the actual HTTP request, so concurrent calls are safe.
static OmadaCredentials getCredentials(String& errorDetail) {
  xSemaphoreTake(g_credsMutex, portMAX_DELAY);

  if (g_creds.loginMs > 0 && (millis() - g_creds.loginMs) < CONTROLLER_SESSION_MAX_AGE_MILLIS) {
    OmadaCredentials snap = g_creds;
    xSemaphoreGive(g_credsMutex);
    ESP_LOGI(TAG, "loginToController: reusing session (age %lu ms)", millis() - g_creds.loginMs);
    return snap;
  }

  // Session stale — re-login. Still inside the lock to prevent double-login races.
  g_creds = {};

  CookieJar jar;
  WiFiClientSecure wc;
  HTTPClient h;
  wc.setInsecure();
  wc.setTimeout(15000);
  h.setCookieJar(&jar);
  h.setTimeout(15000);

  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/login";
  String body = "{\"username\":\"" + ADMIN_USERNAME + "\",\"password\":\"" + ADMIN_PASSWORD + "\"}";

  ESP_LOGI(TAG, "POST %s", url.c_str());
  h.begin(wc, url);
  h.addHeader("Content-Type", "application/json");
  int code = h.POST(body);
  if (code <= 0) {
    errorDetail = "Login: " + h.errorToString(code);
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    h.end();
    xSemaphoreGive(g_credsMutex);
    return {};
  }

  String resp = h.getString();
  h.end();

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    errorDetail = "Login: JSON parse error";
    ESP_LOGE(TAG, "Login: JSON parse error: %.300s", resp.c_str());
    xSemaphoreGive(g_credsMutex);
    return {};
  }

  if (doc["errorCode"].as<int>() != 0) {
    String msg = doc["msg"].is<const char*>() ? doc["msg"].as<String>() : "errorCode " + String(doc["errorCode"].as<int>());
    errorDetail = "Login: " + msg;
    ESP_LOGE(TAG, "Login failed: %s", errorDetail.c_str());
    xSemaphoreGive(g_credsMutex);
    return {};
  }

  if (!doc["result"]["token"].is<const char*>()) {
    errorDetail = "Login: response missing token";
    xSemaphoreGive(g_credsMutex);
    return {};
  }

  g_creds.csrfToken = doc["result"]["token"].as<String>();

  // Extract session ID from the cookie jar. Also patch cookie.date so the
  // ESP32 HTTPClient expiry check doesn't erase it before it's used.
  time_t now_local = time(NULL);
  time_t now_gmt = mktime(gmtime(&now_local));
  for (auto& c : jar) {
    if (c.name == "TPOMADA_SESSIONID") {
      g_creds.sessionId = c.value;
      c.date = now_gmt;
      break;
    }
  }

  if (g_creds.sessionId.isEmpty()) {
    errorDetail = "Login: no TPOMADA_SESSIONID cookie";
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    xSemaphoreGive(g_credsMutex);
    return {};
  }

  g_creds.loginMs = millis();
  ESP_LOGI(TAG, "Login successful");

  OmadaCredentials snap = g_creds;
  xSemaphoreGive(g_credsMutex);
  return snap;
}

// Call when an API response indicates the session is no longer valid server-side.
// Forces a re-login on the next getCredentials() call.
static void invalidateCredentials() {
  xSemaphoreTake(g_credsMutex, portMAX_DELAY);
  g_creds.loginMs = 0;
  xSemaphoreGive(g_credsMutex);
}

// Applies the credential snapshot to a local HTTPClient as Cookie + Csrf-Token headers.
static void applyCredentials(HTTPClient& h, const OmadaCredentials& creds) {
  h.addHeader("Cookie", "TPOMADA_SESSIONID=" + creds.sessionId);
  h.addHeader("Csrf-Token", creds.csrfToken);
}

// ---- Omada API implementations ----

bool loadOmadaSites(String& errorDetail) {
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) return false;

  WiFiClientSecure wc;
  HTTPClient h;
  wc.setInsecure();
  h.setTimeout(15000);

  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID +
               "/api/v2/user/sites?currentPage=1&currentPageSize=100";
  ESP_LOGI(TAG, "GET %s", url.c_str());

  h.begin(wc, url);
  applyCredentials(h, creds);

  int code = h.GET();
  if (code <= 0) {
    errorDetail = "Sites: " + h.errorToString(code);
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    h.end();
    return false;
  }

  String body = h.getString();
  h.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    errorDetail = "Sites: JSON parse error";
    ESP_LOGE(TAG, "Sites: JSON parse error: %.300s", body.c_str());
    return false;
  }

  if (doc["errorCode"].as<int>() != 0) {
    errorDetail = "Sites: " + doc["msg"].as<String>();
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    return false;
  }

  JsonArray data = doc["result"]["data"].as<JsonArray>();
  if (data.size() == 0) {
    errorDetail = "Sites: no sites returned";
    return false;
  }

  SITE_ID   = data[0]["id"].as<String>();
  SITE_NAME = data[0]["name"].as<String>();
  if (SITE_ID.isEmpty()) {
    errorDetail = "Sites: id field empty";
    return false;
  }

  ESP_LOGI(TAG, "Site loaded: id=%s name=%s", SITE_ID.c_str(), SITE_NAME.c_str());
  return true;
}

bool authenticateOmadaClient(SessionParams& session, unsigned long long durationMillis, String& errorDetail) {
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) return false;

  WiFiClientSecure wc;
  HTTPClient h;
  wc.setInsecure();

  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/hotspot/extPortal/auth";

  String postData = "{";
  postData += "\"clientMac\":\"" + session.clientMac + "\",";
#ifdef TEST_MODE
  postData += "\"clientIp\":\"" + session.clientIp + "\",";
#endif
  postData += "\"site\":\"" + SITE_NAME + "\",";
  postData += "\"time\":\"" + String(durationMillis) + "\",";
  postData += "\"authType\":" + String(OMADA_AUTH_TYPE_EXTERNAL_PORTAL) + ",";
  postData += "\"apMac\":\"" + session.apMac + "\",";
  postData += "\"ssidName\":\"" + session.ssidName + "\",";
  postData += "\"radioId\":\"" + session.radioId + "\"";
  postData += "}";

  ESP_LOGI(TAG, "Authenticating client: %s for %llu ms", session.clientMac.c_str(), durationMillis);
  ESP_LOGI(TAG, "POST %s", url.c_str());

  h.begin(wc, url);
  h.addHeader("Content-Type", "application/json");
  applyCredentials(h, creds);

  int httpCode = h.POST(postData);
  if (httpCode <= 0) {
    errorDetail = "Auth: " + h.errorToString(httpCode);
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    h.end();
    return false;
  }

  String body = h.getString();
  h.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    errorDetail = "Auth: JSON parse error";
    ESP_LOGE(TAG, "Auth: JSON parse error: %.300s", body.c_str());
    return false;
  }
  if (doc["errorCode"].as<int>() == 0) {
    ESP_LOGI(TAG, "Authentication successful");
    session.sessionStartMillis = nowEpochMillis();
    session.sessionEndMillis   = session.sessionStartMillis + durationMillis;
    return true;
  }
  String msg = doc["msg"].is<const char*>() ? doc["msg"].as<String>() : "errorCode " + String(doc["errorCode"].as<int>());
  errorDetail = "Auth: " + msg;
  ESP_LOGE(TAG, "Authentication failed: %s", errorDetail.c_str());
  return false;
}

// POST /api/v2/hotspot/sites/{siteId}/cmd/clients/{clientId}/extend
// Adds periodMillis to the session's existing end timestamp (not to now).
bool extendOmadaClient(SessionParams& session, unsigned long long durationMillis, String& errorDetail) {
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) return false;

  WiFiClientSecure wc;
  HTTPClient h;
  wc.setInsecure();

  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID +
               "/api/v2/hotspot/sites/" + SITE_ID +
               "/cmd/clients/" + session.clientId + "/extend";
  String postData = "{\"period\":" + String((unsigned long long)durationMillis) + "}";

  ESP_LOGI(TAG, "POST %s body=%s", url.c_str(), postData.c_str());
  h.begin(wc, url);
  h.addHeader("Content-Type", "application/json");
  applyCredentials(h, creds);

  int httpCode = h.POST(postData);
  if (httpCode <= 0) {
    errorDetail = "Extend: " + h.errorToString(httpCode);
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    h.end();
    return false;
  }

  String body = h.getString();
  h.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    errorDetail = "Extend: JSON parse error";
    ESP_LOGE(TAG, "Extend: JSON parse error: %.300s", body.c_str());
    return false;
  }
  if (doc["errorCode"].as<int>() == 0) {
    ESP_LOGI(TAG, "Extend successful for %s (+%llu ms)", session.clientMac.c_str(), (unsigned long long)durationMillis);
    session.sessionEndMillis += durationMillis;
    return true;
  }
  String msg = doc["msg"].is<const char*>() ? doc["msg"].as<String>() : "errorCode " + String(doc["errorCode"].as<int>());
  errorDetail = "Extend: " + msg;
  ESP_LOGE(TAG, "%s", errorDetail.c_str());
  return false;
}

// POST /api/v2/hotspot/sites/{siteId}/cmd/clients/{clientId}/disconnect
// No body. Works on both active and expired sessions (Omada does not auto-kick on expiry).
bool disconnectOmadaClient(SessionParams& session, String& errorDetail) {
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) return false;

  WiFiClientSecure wc;
  HTTPClient h;
  wc.setInsecure();

  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID +
               "/api/v2/hotspot/sites/" + SITE_ID +
               "/cmd/clients/" + session.clientId + "/disconnect";

  ESP_LOGI(TAG, "POST %s (no body)", url.c_str());
  h.begin(wc, url);
  h.addHeader("Content-Type", "application/json");
  applyCredentials(h, creds);

  int httpCode = h.POST("");
  if (httpCode <= 0) {
    errorDetail = "Disconnect: " + h.errorToString(httpCode);
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    h.end();
    return false;
  }

  String body = h.getString();
  h.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    errorDetail = "Disconnect: JSON parse error";
    ESP_LOGE(TAG, "Disconnect: JSON parse error: %.300s", body.c_str());
    return false;
  }
  if (doc["errorCode"].as<int>() == 0) {
    ESP_LOGI(TAG, "Disconnect successful for %s", session.clientMac.c_str());
    session.sessionEndMillis = 0;
    session.clientId = "";
    return true;
  }
  String msg = doc["msg"].is<const char*>() ? doc["msg"].as<String>() : "errorCode " + String(doc["errorCode"].as<int>());
  errorDetail = "Disconnect: " + msg;
  ESP_LOGE(TAG, "%s", errorDetail.c_str());
  return false;
}

// Fetches and parses the hotspot client list.
// Returns a JsonDocument on success; empty document on failure.
JsonDocument getHotspotClientsJson() {
  String errorDetail;
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) {
    ESP_LOGE(TAG, "getHotspotClientsJson: login failed: %s", errorDetail.c_str());
    return JsonDocument();
  }

  WiFiClientSecure wc;
  HTTPClient h;
  wc.setInsecure();
  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID +
               "/api/v2/hotspot/sites/" + SITE_ID +
               "/clients?sorts.end=desc&currentPage=1&currentPageSize=200";
  ESP_LOGI(TAG, "GET %s", url.c_str());
  h.begin(wc, url);
  applyCredentials(h, creds);

  int code = h.GET();
  if (code <= 0) {
    ESP_LOGE(TAG, "getHotspotClientsJson: HTTP error: %s", h.errorToString(code).c_str());
    h.end();
    return JsonDocument();
  }
  String body = h.getString();
  h.end();
  ESP_LOGI(TAG, "getHotspotClientsJson: HTTP %d, %d bytes", code, body.length());
  if (code != 200 || body.isEmpty()) {
    ESP_LOGE(TAG, "getHotspotClientsJson: bad response: %.200s — invalidating session", body.c_str());
    invalidateCredentials();
    return JsonDocument();
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) || doc["errorCode"].as<int>() != 0)
    ESP_LOGE(TAG, "getHotspotClientsJson: parse/API error: %.200s", body.c_str());
  return doc;
}

// Fetches and parses the all-clients (WiFi) list.
// Returns a JsonDocument on success; empty document on failure (non-fatal).
JsonDocument getAllClientsJson() {
  String errorDetail;
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) {
    ESP_LOGW(TAG, "getAllClientsJson: login failed: %s", errorDetail.c_str());
    return JsonDocument();
  }

  WiFiClientSecure wc;
  HTTPClient h;
  wc.setInsecure();
  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID +
               "/api/v2/sites/" + SITE_ID +
               "/clients?currentPage=1&currentPageSize=200&filters.active=true";
  ESP_LOGI(TAG, "GET %s", url.c_str());
  h.begin(wc, url);
  applyCredentials(h, creds);

  int code = h.GET();
  if (code <= 0) {
    ESP_LOGW(TAG, "getAllClientsJson: HTTP error: %s", h.errorToString(code).c_str());
    h.end();
    return JsonDocument();
  }
  String body = h.getString();
  h.end();
  ESP_LOGI(TAG, "getAllClientsJson: HTTP %d, %d bytes", code, body.length());
  if (code != 200 || body.isEmpty()) {
    ESP_LOGW(TAG, "getAllClientsJson: bad response: %.200s — invalidating session", body.c_str());
    invalidateCredentials();
    return JsonDocument();
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) || doc["errorCode"].as<int>() != 0) {
    ESP_LOGW(TAG, "getAllClientsJson: parse/API error: %.200s", body.c_str());
    return JsonDocument();
  }
  return doc;
}

// Merges active hotspot sessions with all-clients data, keyed on MAC.
// Only includes sessions where end > nowMs (expired records are filtered out).
// Each entry in the returned array has:
//   mac, id, end              — from hotspot clients
//   ip               — from hotspot clients (authoritative; wifi clients list lags after auth)
//   apMac, ssid, radioId  — from all-clients (empty if client not yet visible there)
JsonDocument mergeClientLists(JsonArray hotspotClients, JsonArray allClients, uint64_t nowMs) {
  std::map<String, JsonObject> allMap;
  for (JsonObject c : allClients) allMap[c["mac"].as<String>()] = c;

  JsonDocument merged;
  JsonArray arr = merged.to<JsonArray>();
  std::set<String> seen;  // deduplicate: list sorted end desc, keep first (latest) session per MAC
  for (JsonObject hs : hotspotClients) {
    uint64_t end = hs["end"].as<uint64_t>();
    if (end <= nowMs) continue;
    if (hs["authType"].as<int>() != OMADA_AUTH_TYPE_EXTERNAL_PORTAL) continue;
    String mac = hs["mac"].as<String>();
    if (!seen.insert(mac).second) continue;  // already have a later session for this MAC

    JsonObject entry = arr.add<JsonObject>();
    entry["mac"]   = hs["mac"];
    entry["id"]    = hs["id"];
    entry["start"] = hs["start"].as<uint64_t>();
    entry["end"]   = end;
    entry["ip"]    = hs["ip"];  // hotspot API is authoritative for ip; wifi clients list lags

    auto it = allMap.find(hs["mac"].as<String>());
    if (it != allMap.end()) {
      entry["apMac"]   = it->second["apMac"];
      entry["ssid"]    = it->second["ssid"];
      entry["radioId"] = it->second["radioId"];
    }
  }
  return merged;
}

// Merges the two client lists and updates the in-memory session cache.
// Called at startup, after any auth/extend/disconnect, and every 60s from loop().
// getHotspotClientsJson and getAllClientsJson each get their own credential
// snapshot and run their HTTP calls with fully independent local state —
// they may execute concurrently across tasks without risk.
void refreshHotspotSessionCache() {
  JsonDocument hotspotDoc = getHotspotClientsJson();
  JsonDocument allDoc = getAllClientsJson();

  {
    int count = 0;
    uint64_t now = nowEpochMillis();
    for (JsonObject hs : hotspotDoc["result"]["data"].as<JsonArray>()) {
      if (hs["authType"].as<int>() == OMADA_AUTH_TYPE_VOUCHER && hs["end"].as<uint64_t>() > now)
        count++;
    }
    VOUCHER_ACTIVE_COUNT = count;
  }

  JsonDocument merged = mergeClientLists(
    hotspotDoc["result"]["data"].as<JsonArray>(),
    allDoc["result"]["data"].as<JsonArray>(),
    nowEpochMillis()
  );

  int updated = 0;
  for (JsonObject client : merged.as<JsonArray>()) {
    String mac = client["mac"].as<String>();
    String ip  = client["ip"].as<String>();

    SessionParams* macSession = HOTSPOT_SESSION_CACHE.findByMac(mac);
    SessionParams* ipSession  = HOTSPOT_SESSION_CACHE.findByIp(ip);
    SessionParams* session;
    bool isNew = false;

    if (ip.isEmpty() && !macSession) continue;  // not yet visible in all-clients and not previously cached

    if (!ipSession && macSession) {
      session = macSession;
    } else if (!macSession && ipSession) {
      ESP_LOGW(TAG, "Replacing stale cache entry at IP=%s with MAC=%s", ip.c_str(), mac.c_str());
      HOTSPOT_SESSION_CACHE.removeByIp(ip);
      session = &HOTSPOT_SESSION_CACHE.upsert(ip, mac);
      isNew = true;
    } else if (macSession && ipSession && macSession != ipSession) {
      ESP_LOGI(TAG, "MAC %s moved IP: %s -> %s", mac.c_str(), macSession->clientIp.c_str(), ip.c_str());
      HOTSPOT_SESSION_CACHE.removeByIp(macSession->clientIp);
      session = &HOTSPOT_SESSION_CACHE.upsert(ip, mac);
    } else {
      isNew = (macSession == nullptr && ipSession == nullptr);
      session = &HOTSPOT_SESSION_CACHE.upsert(ip, mac);
    }

    if (isNew && hasPausedSessionFile(mac)) session->pausedRemainingMillis = loadPausedSessionFile(mac);

    session->sessionStartMillis = client["start"].as<uint64_t>();
    session->sessionEndMillis   = client["end"].as<uint64_t>();
    session->clientId           = client["id"].as<String>();
    if (!client["apMac"].isNull())   session->apMac    = client["apMac"].as<String>();
    if (!client["ssid"].isNull())    session->ssidName = client["ssid"].as<String>();
    if (!client["radioId"].isNull()) session->radioId  = String(client["radioId"].as<int>());
    updated++;
    ESP_LOGI(TAG, "Session updated: IP=%s MAC=%s sessionEnd=%llu", session->clientIp.c_str(), mac.c_str(), (unsigned long long)session->sessionEndMillis);
  }

  ESP_LOGI(TAG, "refreshHotspotSessionCache: %d active session(s)", updated);

  uint64_t now = nowEpochMillis();
  HOTSPOT_SESSION_CACHE.lock();
  for (auto& kv : HOTSPOT_SESSION_CACHE) {
    const SessionParams& s = kv.second;
    int64_t remainingMs = (int64_t)s.sessionEndMillis - (int64_t)now;
    int remainingMin    = (int)(remainingMs / 60000);
    time_t expSec = (time_t)(s.sessionEndMillis / 1000);
    struct tm* t = localtime(&expSec);
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", t);
    ESP_LOGI(TAG, "  MAC=%-17s IP=%-15s remaining=%dm expires=%s.%03llu",
      s.clientMac.c_str(), s.clientIp.c_str(), remainingMin,
      timeBuf, (unsigned long long)(s.sessionEndMillis % 1000));
  }
  HOTSPOT_SESSION_CACHE.unlock();
  g_lastCacheRefreshMs = millis();
}

// ---- Voucher API ----

JsonDocument getVoucherGroupsJson() {
  String errorDetail;
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) { ESP_LOGE(TAG, "getVoucherGroupsJson: login failed: %s", errorDetail.c_str()); return JsonDocument(); }
  WiFiClientSecure wc; HTTPClient h; wc.setInsecure();
  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/hotspot/sites/" + SITE_ID + "/voucherGroups?currentPage=1&currentPageSize=100";
  h.begin(wc, url); applyCredentials(h, creds);
  int code = h.GET();
  if (code <= 0) { ESP_LOGE(TAG, "getVoucherGroupsJson: %s", h.errorToString(code).c_str()); h.end(); return JsonDocument(); }
  String body = h.getString(); h.end();
  if (code != 200 || body.isEmpty()) { ESP_LOGE(TAG, "getVoucherGroupsJson: HTTP %d: %.200s", code, body.c_str()); invalidateCredentials(); return JsonDocument(); }
  JsonDocument doc;
  if (deserializeJson(doc, body) || doc["errorCode"].as<int>() != 0) ESP_LOGE(TAG, "getVoucherGroupsJson: API error: %.200s", body.c_str());
  return doc;
}

JsonDocument getVoucherHistoryJson(time_t startSec, time_t endSec) {
  String errorDetail;
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) { ESP_LOGW(TAG, "getVoucherHistoryJson: login failed: %s", errorDetail.c_str()); return JsonDocument(); }
  WiFiClientSecure wc; HTTPClient h; wc.setInsecure();
  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/hotspot/sites/" + SITE_ID +
               "/vouchers/statistics/history?filters.timeStart=" + String((long)startSec) +
               "&filters.timeEnd=" + String((long)endSec);
  h.begin(wc, url); applyCredentials(h, creds);
  int code = h.GET();
  if (code <= 0) { ESP_LOGW(TAG, "getVoucherHistoryJson: %s", h.errorToString(code).c_str()); h.end(); return JsonDocument(); }
  String body = h.getString(); h.end();
  if (code != 200 || body.isEmpty()) { ESP_LOGW(TAG, "getVoucherHistoryJson: HTTP %d: %.200s", code, body.c_str()); return JsonDocument(); }
  JsonDocument doc;
  if (deserializeJson(doc, body) || doc["errorCode"].as<int>() != 0) {
    ESP_LOGW(TAG, "getVoucherHistoryJson: API error: %.200s", body.c_str());
    return JsonDocument();
  }
  ESP_LOGI(TAG, "getVoucherHistoryJson: HTTP %d, %d bytes", code, body.length());
  return doc;
}


bool createVoucherGroup(const String& name, int durationMin, int totalCount,
                        const String& description,
                        String& groupId, String& errorDetail) {
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) return false;
  WiFiClientSecure wc; HTTPClient h; wc.setInsecure();
  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/hotspot/sites/" + SITE_ID + "/voucherGroups";
  h.begin(wc, url); applyCredentials(h, creds); h.addHeader("Content-Type", "application/json");
  String escapedDesc = description;
  escapedDesc.replace("\"", "\\\"");
  String body =
    "{\"codeLength\":8,\"codeForm\":[0],\"amount\":" + String(totalCount) +
    ",\"name\":\"" + name + "\",\"type\":0,\"logout\":true" +
    ",\"downLimitUnit\":0,\"upLimitUnit\":0,\"trafficLimitEnable\":false,\"trafficLimit\":null,\"trafficLimitUnit\":0" +
    ",\"voucherValidityEnable\":false,\"upTimeLimitEnable\":false" +
    ",\"durationType\":1,\"duration\":" + String(durationMin) +
    ",\"description\":\"" + escapedDesc + "\"" +
    ",\"maxUsers\":1,\"trafficLimitFrequency\":1,\"portalIds\":[],\"applyToAllPortals\":true" +
    ",\"validityType\":0,\"startTime\":\"00:00\",\"endTime\":\"23:59\",\"scheduleTime\":0" +
    ",\"weeklyEnableDays\":{\"1\":true,\"2\":true,\"3\":true,\"4\":true,\"5\":true,\"6\":true,\"7\":true}" +
    ",\"pattern\":{\"patternType\":0,\"position\":0,\"ssidNetworkEnable\":false,\"durationEnable\":false" +
    ",\"limitEnable\":false,\"logoPictureId\":null,\"logoSize\":62}}";
  int code = h.POST(body);
  if (code <= 0) { errorDetail = h.errorToString(code); h.end(); return false; }
  String resp = h.getString(); h.end();
  ESP_LOGI(TAG, "createVoucherGroup: HTTP %d body=%.200s", code, resp.c_str());
  JsonDocument doc;
  if (code != 200 || deserializeJson(doc, resp) || doc["errorCode"].as<int>() != 0) {
    errorDetail = doc["msg"].isNull() ? ("HTTP " + String(code)) : doc["msg"].as<String>();
    return false;
  }
  groupId = doc["result"]["id"].as<String>();
  return true;
}

JsonDocument getVoucherCodesJson(const String& groupId, int page) {
  String errorDetail;
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) return JsonDocument();
  WiFiClientSecure wc; HTTPClient h; wc.setInsecure();
  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/hotspot/sites/" + SITE_ID +
               "/voucherGroups/" + groupId + "?currentPage=" + String(page) + "&currentPageSize=130";
  h.begin(wc, url); applyCredentials(h, creds);
  int code = h.GET();
  if (code <= 0) { h.end(); return JsonDocument(); }
  String body = h.getString(); h.end();
  JsonDocument doc;
  if (code != 200 || body.isEmpty() || deserializeJson(doc, body) || doc["errorCode"].as<int>() != 0) return JsonDocument();
  return doc;
}

unsigned long getOmadaSessionAgeMs() {
  xSemaphoreTake(g_credsMutex, portMAX_DELAY);
  unsigned long age = g_creds.loginMs > 0 ? millis() - g_creds.loginMs : 0;
  xSemaphoreGive(g_credsMutex);
  return age;
}

unsigned long getLastCacheRefreshMs() {
  return g_lastCacheRefreshMs > 0 ? millis() - g_lastCacheRefreshMs : 0;
}

bool setupOmada() {
  g_credsMutex = xSemaphoreCreateMutex();
  String error;
  if (!loadOmadaSites(error)) {
    ESP_LOGE(TAG, "setupOmada failed: %s", error.c_str());
    return false;
  }
  refreshHotspotSessionCache();
  return true;
}
