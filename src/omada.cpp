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
#include <freertos/semphr.h>
#include <esp_log.h>
#include <stdlib.h>   // setenv
#include <time.h>     // tzset
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

ControllerStatus CONTROLLER_STATUS;

static const unsigned long CONTROLLER_SESSION_MAX_AGE_MILLIS = 3600000;

volatile int VOUCHER_ACTIVE_COUNT = 0;
static unsigned long LAST_CACHE_REFRESH_MILLIS = 0;

// ---- Shared credential store ----
// Login result is stored here. API functions copy what they need under a brief
// lock, then make their HTTP calls independently — enabling true concurrency.
// The mutex is held only during: (a) credential copy, or (b) re-login.

struct OmadaCredentials {
  String csrfToken;
  String sessionId;  // raw TPOMADA_SESSIONID cookie value
  unsigned long loginMs = 0;
};

static OmadaCredentials CREDS;
static SemaphoreHandle_t CREDS_MUTEX;

// Returns a credential snapshot, logging in first if the session is stale.
// Mutex is held for the copy (microseconds) or for a re-login (≤ 2s, once/hour).
// Callers use the snapshot to build their own local HTTPClient — no shared state
// during the actual HTTP request, so concurrent calls are safe.
static OmadaCredentials getCredentials(String& errorDetail) {
  xSemaphoreTake(CREDS_MUTEX, portMAX_DELAY);

  if (CREDS.loginMs > 0 && (millis() - CREDS.loginMs) < CONTROLLER_SESSION_MAX_AGE_MILLIS) {
    OmadaCredentials snap = CREDS;
    xSemaphoreGive(CREDS_MUTEX);
    ESP_LOGV(TAG, "loginToController: reusing session (age %lu ms)", millis() - CREDS.loginMs);
    return snap;
  }

  // Session stale — re-login. Still inside the lock to prevent double-login races.
  CREDS = {};

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
    xSemaphoreGive(CREDS_MUTEX);
    return {};
  }

  String resp = h.getString();
  h.end();

  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    errorDetail = "Login: JSON parse error";
    ESP_LOGE(TAG, "Login: JSON parse error: %.300s", resp.c_str());
    xSemaphoreGive(CREDS_MUTEX);
    return {};
  }

  if (doc["errorCode"].as<int>() != 0) {
    String msg = doc["msg"].is<const char*>() ? doc["msg"].as<String>() : "errorCode " + String(doc["errorCode"].as<int>());
    errorDetail = "Login: " + msg;
    ESP_LOGE(TAG, "Login failed: %s", errorDetail.c_str());
    xSemaphoreGive(CREDS_MUTEX);
    return {};
  }

  if (!doc["result"]["token"].is<const char*>()) {
    errorDetail = "Login: response missing token";
    xSemaphoreGive(CREDS_MUTEX);
    return {};
  }

  CREDS.csrfToken = doc["result"]["token"].as<String>();

  // Extract session ID from the cookie jar. Also patch cookie.date so the
  // ESP32 HTTPClient expiry check doesn't erase it before it's used.
  time_t now_local = time(NULL);
  time_t now_gmt = mktime(gmtime(&now_local));
  for (auto& c : jar) {
    if (c.name == "TPOMADA_SESSIONID") {
      CREDS.sessionId = c.value;
      c.date = now_gmt;
      break;
    }
  }

  if (CREDS.sessionId.isEmpty()) {
    errorDetail = "Login: no TPOMADA_SESSIONID cookie";
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    xSemaphoreGive(CREDS_MUTEX);
    return {};
  }

  CREDS.loginMs = millis();
  ESP_LOGI(TAG, "Login successful");

  OmadaCredentials snap = CREDS;
  xSemaphoreGive(CREDS_MUTEX);
  return snap;
}

// Call when an API response indicates the session is no longer valid server-side.
// Forces a re-login on the next getCredentials() call.
static void invalidateCredentials() {
  xSemaphoreTake(CREDS_MUTEX, portMAX_DELAY);
  CREDS.loginMs = 0;
  xSemaphoreGive(CREDS_MUTEX);
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
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    return false;
  }

  SITE_ID   = data[0]["id"].as<String>();
  SITE_NAME = data[0]["name"].as<String>();
  if (SITE_ID.isEmpty()) {
    errorDetail = "Sites: id field empty";
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
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
// Captive-page AP coverage map: snapshot EVERY Omada device as {mac:status} (2=online 1=issue
// 0=offline). The page (which has /map.svg with data-mac dots) colours the ones it has dots for.
// Gated on /map.svg existing — no map, no poll. The snapshot string is mutex-guarded: written here
// (refresh path), read by buildStatusJson (web/ws tasks).
// statusCategory enum (standard Omada): 1=CONNECTED, 0=DISCONNECTED, others=transitional. Confirm
// with the live controller (build_site_map.py / a probe) and adjust the mapping if it differs.
static String            DEVICE_STATUS_JSON = "{}";
static SemaphoreHandle_t DEVICE_STATUS_MUTEX = nullptr;

String deviceStatusJson() {
  if (!DEVICE_STATUS_MUTEX) return "{}";
  xSemaphoreTake(DEVICE_STATUS_MUTEX, portMAX_DELAY);
  String copy = DEVICE_STATUS_JSON;
  xSemaphoreGive(DEVICE_STATUS_MUTEX);
  return copy;
}

void refreshDeviceStatus() {
  if (!fsExists("/map.svg")) return;   // feature off — no coverage map on this node
  if (!DEVICE_STATUS_MUTEX) DEVICE_STATUS_MUTEX = xSemaphoreCreateMutex();

  String errorDetail;
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) { ESP_LOGW(TAG, "refreshDeviceStatus: login failed: %s", errorDetail.c_str()); return; }

  WiFiClientSecure wc; wc.setInsecure();
  HTTPClient h;
  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID +
               "/api/v2/sites/" + SITE_ID + "/grid/devices";
  h.begin(wc, url);
  applyCredentials(h, creds);
  int code = h.GET();
  String body = (code > 0) ? h.getString() : "";
  h.end();
  if (code != 200 || body.isEmpty()) {
    ESP_LOGW(TAG, "refreshDeviceStatus: HTTP %d (%d bytes)", code, body.length());
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) || doc["errorCode"].as<int>() != 0) {
    ESP_LOGW(TAG, "refreshDeviceStatus: parse/API error: %.200s", body.c_str());
    return;
  }

  String out = "{";
  int n = 0;
  for (JsonObject dev : doc["result"]["data"].as<JsonArray>()) {
    String mac = dev["mac"] | "";
    if (mac.isEmpty()) continue;
    mac.replace(":", "-"); mac.toUpperCase();          // normalize to match data-mac in map.svg
    int cat = dev["statusCategory"] | -1;
    uint8_t st = (cat == 1) ? 2 : (cat == 0) ? 0 : 1;  // connected / disconnected / transitional
    if (n++) out += ",";
    out += "\"" + mac + "\":" + String(st);
  }
  out += "}";

  xSemaphoreTake(DEVICE_STATUS_MUTEX, portMAX_DELAY);
  DEVICE_STATUS_JSON = out;
  xSemaphoreGive(DEVICE_STATUS_MUTEX);
  ESP_LOGI(TAG, "refreshDeviceStatus: %d device(s)", n);
}

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

// Set a client's Omada name/alias (PATCH). EllaFi stows a node's "nickname|commission" here so it
// survives a device wipe and syncs across nodes; it's read back for free from getAllClientsJson()["name"].
bool setClientName(const String& mac, const String& name, String& errorDetail) {
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) return false;

  String macDash = mac; macDash.replace(":", "-"); macDash.toUpperCase();   // path wants dash-separated
  String esc = name; esc.replace("\\", "\\\\"); esc.replace("\"", "\\\"");

  WiFiClientSecure wc;
  HTTPClient h;
  wc.setInsecure();
  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/sites/" + SITE_ID + "/clients/" + macDash;
  String body = "{\"name\":\"" + esc + "\"}";
  ESP_LOGI(TAG, "PATCH %s name=%s", url.c_str(), name.c_str());
  h.begin(wc, url);
  applyCredentials(h, creds);
  h.addHeader("Content-Type", "application/json;charset=UTF-8");
  int code = h.sendRequest("PATCH", body);
  if (code <= 0) {
    errorDetail = "setClientName: " + h.errorToString(code);
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    h.end();
    return false;
  }
  String resp = h.getString();
  h.end();
  JsonDocument doc;
  if (deserializeJson(doc, resp) || doc["errorCode"].as<int>() != 0) {
    errorDetail = "setClientName: " + resp.substring(0, 160);
    ESP_LOGE(TAG, "setClientName: API error: %.200s", resp.c_str());
    return false;
  }
  return true;
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
    entry["ip"]    = hs["ip"];  // baseline: auth-time IP (covers a client not yet in the WiFi list)

    auto it = allMap.find(hs["mac"].as<String>());
    if (it != allMap.end()) {
      if (!it->second["ip"].isNull()) entry["ip"] = it->second["ip"];  // live WiFi table = current IP, fresh across re-IP
      entry["apMac"]   = it->second["apMac"];
      entry["ssid"]    = it->second["ssid"];
      entry["radioId"] = it->second["radioId"];
    }
  }
  return merged;
}

// refreshControllerStatus() is declared in omada.h (also called periodically by the master monitor).

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
    if (mac.isEmpty()) continue;  // the cache is keyed by MAC

    SessionParams* prior = HOTSPOT_SESSION_CACHE.findByMac(mac);
    bool isNew = (prior == nullptr);
    if (ip.isEmpty() && isNew) continue;  // not yet visible in all-clients and never cached

    if (prior && !ip.isEmpty() && prior->clientIp != ip)
      ESP_LOGI(TAG, "MAC %s moved IP: %s -> %s", mac.c_str(), prior->clientIp.c_str(), ip.c_str());

    // Keyed by MAC: an IP change re-points the index, never destroys the entry — this is what
    // fixes the finalize use-after-free and preserves paused/socket state across a DHCP re-IP.
    SessionParams* session = &HOTSPOT_SESSION_CACHE.upsert(ip, mac);

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

  // Management-SSID tripwire: a hotspot/portal client should never appear on the SSID our
  // nodes live on. One that does = a customer breached isolation. Our own nodes aren't
  // portal clients, so they never show in this list — no whitelist needed. Binary flag for
  // the dashboard; the offending MAC goes to the log for detail.
  bool mgmtBreach = false;
  if (!MANAGEMENT_SSID.isEmpty()) {
    for (JsonObject client : merged.as<JsonArray>()) {
      if (client["ssid"].as<String>() != MANAGEMENT_SSID) continue;
      mgmtBreach = true;
      ESP_LOGW(TAG, "SECURITY: hotspot client %s is on the management SSID '%s' — isolation breach",
               client["mac"].as<String>().c_str(), MANAGEMENT_SSID.c_str());
    }
  }
  CONTROLLER_STATUS.mgmtSsidBreach = mgmtBreach;

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
  LAST_CACHE_REFRESH_MILLIS = millis();

  refreshControllerStatus();  // refresh cached controller status (reachable, AP count, TZ)
  refreshDeviceStatus();      // refresh AP up/down for the captive-page coverage map
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
  String escapedName = name;
  escapedName.replace("\"", "\\\"");
  String escapedDesc = description;
  escapedDesc.replace("\"", "\\\"");
  String body =
    "{\"codeLength\":8,\"codeForm\":[0],\"amount\":" + String(totalCount) +
    ",\"name\":\"" + escapedName + "\",\"type\":0,\"logout\":true" +
    ",\"downLimitUnit\":0,\"upLimitUnit\":0,\"trafficLimitEnable\":false,\"trafficLimit\":null,\"trafficLimitUnit\":0" +
    // upTimeLimitEnable: true = "By Usage" (counts only online time); false = "By Time" (from first use)
    ",\"voucherValidityEnable\":false,\"upTimeLimitEnable\":true"
    + ",\"durationType\":1,\"duration\":" + String(durationMin) +
    ",\"description\":\"" + escapedDesc + "\"" +
    ",\"maxUsers\":1,\"trafficLimitFrequency\":1,\"portalIds\":[],\"applyToAllPortals\":true" +
    ",\"validityType\":0,\"startTime\":\"00:00\",\"endTime\":\"23:59\",\"scheduleTime\":0" +
    ",\"weeklyEnableDays\":{\"1\":true,\"2\":true,\"3\":true,\"4\":true,\"5\":true,\"6\":true,\"7\":true}" +
    ",\"pattern\":{\"patternType\":0,\"position\":0,\"ssidNetworkEnable\":false,\"durationEnable\":false" +
    ",\"limitEnable\":false,\"logoPictureId\":null,\"logoSize\":62}}";
  int code = h.POST(body);
  if (code <= 0) {
    errorDetail = h.errorToString(code); h.end();
    ESP_LOGE(TAG, "createVoucherGroup: POST failed: %s", errorDetail.c_str());
    return false;
  }
  String resp = h.getString(); h.end();
  ESP_LOGI(TAG, "createVoucherGroup: HTTP %d body=%.200s", code, resp.c_str());
  JsonDocument doc;
  if (code != 200 || deserializeJson(doc, resp) || doc["errorCode"].as<int>() != 0) {
    errorDetail = doc["msg"].isNull() ? ("HTTP " + String(code)) : doc["msg"].as<String>();
    ESP_LOGE(TAG, "createVoucherGroup failed: %s", errorDetail.c_str());  // W/E → errors.log + forward + Logs tab
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

bool deleteVoucherGroup(const String& groupId, String& errorDetail) {
  OmadaCredentials creds = getCredentials(errorDetail);
  if (creds.loginMs == 0) return false;
  WiFiClientSecure wc; HTTPClient h; wc.setInsecure();
  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/hotspot/sites/" + SITE_ID +
               "/voucherGroups/" + groupId;
  h.begin(wc, url); applyCredentials(h, creds);
  int code = h.sendRequest("DELETE");
  if (code <= 0) { errorDetail = h.errorToString(code); h.end(); return false; }
  String body = h.getString(); h.end();
  JsonDocument doc;
  if (code != 200 || deserializeJson(doc, body) || doc["errorCode"].as<int>() != 0) {
    errorDetail = doc["msg"].isNull() ? ("HTTP " + String(code)) : doc["msg"].as<String>();
    ESP_LOGE(TAG, "deleteVoucherGroup: %s", errorDetail.c_str());
    return false;
  }
  ESP_LOGI(TAG, "deleteVoucherGroup: deleted %s", groupId.c_str());
  return true;
}

unsigned long getOmadaSessionAgeMs() {
  xSemaphoreTake(CREDS_MUTEX, portMAX_DELAY);
  unsigned long age = CREDS.loginMs > 0 ? millis() - CREDS.loginMs : 0;
  xSemaphoreGive(CREDS_MUTEX);
  return age;
}

unsigned long getLastCacheRefreshMs() {
  return LAST_CACHE_REFRESH_MILLIS > 0 ? millis() - LAST_CACHE_REFRESH_MILLIS : 0;
}

// IANA timezone name (from the controller) → POSIX TZ string. The POSIX string
// encodes the base offset AND the daylight-saving rules; the ESP32 has no IANA
// database but newlib's tzset() applies POSIX DST rules, so localtime()/mktime()
// become offset- and season-correct. POSIX sign is inverted (UTC+8 → "-8").
// DST rules reflect 2024 tzdata; a zone whose government changes policy needs its
// one string updated. Covers Omada's timezone list; unknown zones fall back to
// UTC+8 (logged). Used only for daily report buckets — device NTP stays UTC.
struct IanaPosix { const char* iana; const char* posix; };
static const IanaPosix TZ_TABLE[] = {
  {"Pacific/Midway",                 "SST11"},
  {"Pacific/Honolulu",               "HST10"},
  {"America/Anchorage",              "AKST9AKDT,M3.2.0,M11.1.0"},
  {"America/Los_Angeles",            "PST8PDT,M3.2.0,M11.1.0"},
  {"America/Phoenix",                "MST7"},
  {"America/Mazatlan",               "MST7"},
  {"America/Denver",                 "MST7MDT,M3.2.0,M11.1.0"},
  {"America/Guatemala",              "CST6"},
  {"America/Chicago",                "CST6CDT,M3.2.0,M11.1.0"},
  {"America/Mexico_City",            "CST6"},
  {"America/Regina",                 "CST6"},
  {"America/Bogota",                 "<-05>5"},
  {"America/New_York",               "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Indiana/Indianapolis",   "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Caracas",                "<-04>4"},
  {"America/Asuncion",               "<-04>4"},
  {"America/Halifax",                "AST4ADT,M3.2.0,M11.1.0"},
  {"America/Cuiaba",                 "<-04>4"},
  {"America/Manaus",                 "<-04>4"},
  {"America/Santiago",               "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
  {"America/St_Johns",               "NST3:30NDT,M3.2.0,M11.1.0"},
  {"America/Sao_Paulo",              "<-03>3"},
  {"America/Argentina/Buenos_Aires", "<-03>3"},
  {"America/Cayenne",                "<-03>3"},
  {"America/Godthab",                "<-03>3"},
  {"America/Montevideo",             "<-03>3"},
  {"Atlantic/South_Georgia",         "<-02>2"},
  {"Atlantic/Azores",                "<-01>1<+00>,M3.5.0/0,M10.5.0/1"},
  {"Atlantic/Cape_Verde",            "<-01>1"},
  {"Africa/Casablanca",              "<+01>-1"},
  {"UTC",                            "UTC0"},
  {"Etc/UTC",                        "UTC0"},
  {"Europe/London",                  "GMT0BST,M3.5.0/1,M10.5.0"},
  {"Atlantic/Reykjavik",             "GMT0"},
  {"Europe/Berlin",                  "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Belgrade",                "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Paris",                   "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Warsaw",                  "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Africa/Lagos",                   "WAT-1"},
  {"Europe/Athens",                  "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Asia/Beirut",                    "EET-2EEST,M3.5.0/0,M10.5.0/0"},
  {"Africa/Cairo",                   "EET-2EEST,M4.5.5/0,M10.5.4/24"},
  {"Asia/Damascus",                  "<+03>-3"},
  {"Africa/Johannesburg",            "SAST-2"},
  {"Europe/Helsinki",                "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Asia/Jerusalem",                 "IST-2IDT,M3.4.4/26,M10.5.0"},
  {"Asia/Amman",                     "<+03>-3"},
  {"Asia/Baghdad",                   "<+03>-3"},
  {"Europe/Minsk",                   "<+03>-3"},
  {"Asia/Riyadh",                    "<+03>-3"},
  {"Africa/Nairobi",                 "EAT-3"},
  {"Europe/Istanbul",                "<+03>-3"},
  {"Europe/Moscow",                  "MSK-3"},
  {"Asia/Tehran",                    "<+0330>-3:30"},
  {"Asia/Dubai",                     "<+04>-4"},
  {"Asia/Baku",                      "<+04>-4"},
  {"Asia/Tbilisi",                   "<+04>-4"},
  {"Asia/Yerevan",                   "<+04>-4"},
  {"Asia/Kabul",                     "<+0430>-4:30"},
  {"Asia/Karachi",                   "PKT-5"},
  {"Asia/Yekaterinburg",             "<+05>-5"},
  {"Asia/Tashkent",                  "<+05>-5"},
  {"Asia/Kolkata",                   "IST-5:30"},
  {"Asia/Colombo",                   "<+0530>-5:30"},
  {"Asia/Kathmandu",                 "<+0545>-5:45"},
  {"Asia/Dhaka",                     "<+06>-6"},
  {"Asia/Yangon",                    "<+0630>-6:30"},
  {"Asia/Bangkok",                   "<+07>-7"},
  {"Asia/Krasnoyarsk",               "<+07>-7"},
  {"Asia/Novosibirsk",               "<+07>-7"},
  {"Asia/Hong_Kong",                 "HKT-8"},          // confirmed: controller returns this for UTC+8 Beijing/HK
  {"Asia/Shanghai",                  "CST-8"},
  {"Asia/Manila",                    "<+08>-8"},
  {"Asia/Singapore",                 "<+08>-8"},
  {"Asia/Kuala_Lumpur",              "<+08>-8"},
  {"Asia/Taipei",                    "CST-8"},
  {"Australia/Perth",                "AWST-8"},
  {"Asia/Ulaanbaatar",               "<+08>-8"},
  {"Asia/Irkutsk",                   "<+08>-8"},
  {"Asia/Tokyo",                     "JST-9"},
  {"Asia/Seoul",                     "KST-9"},
  {"Asia/Yakutsk",                   "<+09>-9"},
  {"Australia/Adelaide",             "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
  {"Australia/Darwin",               "ACST-9:30"},
  {"Australia/Brisbane",             "AEST-10"},
  {"Australia/Sydney",               "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"Australia/Hobart",               "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"Asia/Vladivostok",               "<+10>-10"},
  {"Pacific/Port_Moresby",           "<+10>-10"},
  {"Asia/Magadan",                   "<+11>-11"},
  {"Pacific/Guadalcanal",            "<+11>-11"},
  {"Pacific/Auckland",               "NZST-12NZDT,M9.5.0,M4.1.0/3"},
  {"Pacific/Fiji",                   "<+12>-12"},
  {"Asia/Kamchatka",                 "<+12>-12"},
  {"Pacific/Tongatapu",              "<+13>-13"},
};

// GET settings/system/status → refresh the cached ControllerStatus (reachable + counters every
// call; identity strings + timezone resolved once) and apply the controller's TZ via setenv/tzset
// so the report-bucket helpers (localDayNum/localDayStartUtc in web.cpp) use controller-local days.
// Best-effort: an unreachable controller leaves the last-known identity and flips reachable=false.
void refreshControllerStatus() {
  String err;
  OmadaCredentials creds = getCredentials(err);
  JsonDocument doc;
  bool ok = false;
  if (creds.loginMs != 0) {
    WiFiClientSecure wc; HTTPClient h; wc.setInsecure();
    wc.setTimeout(4000);          // bounded — this runs in the master loop's 30s health poll
    h.setConnectTimeout(4000);    // a dead controller must fail fast, not stall coin/UDP timing
    h.setTimeout(4000);
    String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/settings/system/status";
    h.begin(wc, url); applyCredentials(h, creds);
    int code = h.GET();
    String body = (code > 0) ? h.getString() : String();
    h.end();
    ok = (code == 200 && !deserializeJson(doc, body) && doc["errorCode"].as<int>() == 0);
    if (!ok) ESP_LOGW(TAG, "refreshControllerStatus: status fetch failed (HTTP %d)", code);
  }

  CONTROLLER_STATUS.reachable = ok;
  if (!ok) {
    // Force a fresh login next poll. The session is cached for an hour, so without this a controller
    // *restart* (which invalidates the session) would keep every poll failing until the 1h expiry.
    invalidateCredentials();
    return;  // keep last-known identity/counters; reachable is now false
  }

  JsonObject r = doc["result"];
  CONTROLLER_STATUS.uptimeMillis  = r["upTime"].as<unsigned long>();
  CONTROLLER_STATUS.fetchedMillis = millis();
  CONTROLLER_STATUS.clockSkewSec  =
    (int)(((long long)r["systemTime"].as<long long>() - (long long)nowEpochMillis()) / 1000);

  // Resolve + apply the timezone on every successful fetch (idempotent; no shared String written).
  String zone = r["timeZone"].as<String>();
  const char* posix = nullptr;
  for (auto& e : TZ_TABLE) if (zone == e.iana) { posix = e.posix; break; }
  if (!posix) posix = "HKT-8";  // UTC+8 default
  setenv("TZ", posix, 1);
  tzset();

  // Identity strings: set ONCE on first success (never rewritten → concurrent-read-safe).
  if (CONTROLLER_STATUS.model.isEmpty()) {
    CONTROLLER_STATUS.model    = r["model"].as<String>();
    CONTROLLER_STATUS.version  = r["controllerVersion"].as<String>();
    CONTROLLER_STATUS.timeZone = zone;
    ESP_LOGI(TAG, "Controller: %s v%s tz=%s -> TZ=%s",
             CONTROLLER_STATUS.model.c_str(), CONTROLLER_STATUS.version.c_str(), zone.c_str(), posix);
  }
}

bool isServiceReady() {
  return (time(NULL) > NTP_EPOCH_MIN) && CONTROLLER_STATUS.reachable && !SITE_ID.isEmpty();
}

bool setupOmada() {
  // Create the credentials lock EXACTLY ONCE. setupOmada() is also the controller-down retry
  // (main.loop, every 30s): re-creating CREDS_MUTEX while another task holds it swaps the global to a
  // fresh semaphore and lets a second task enter the critical section — a heap-clean "double owner"
  // that trips `assert failed: xQueueGenericSend queue.c:832`.
  if (CREDS_MUTEX == nullptr) CREDS_MUTEX = xSemaphoreCreateMutex();
  String error;
  if (!loadOmadaSites(error)) {
    ESP_LOGE(TAG, "setupOmada failed: %s", error.c_str());
    return false;
  }
  setenv("TZ", "HKT-8", 1); tzset();  // default UTC+8 until refreshControllerStatus resolves the real zone
  refreshHotspotSessionCache();        // also refreshes controller status (sets real TZ)
  return true;
}
