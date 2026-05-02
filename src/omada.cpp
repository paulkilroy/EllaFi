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
 *   site (name)       Auth                 ✓                                      Sites API
 *   time              Auth                                                        Calculated
 *   authType          Auth                          ✓       ✓        4
 *   id (client) *     Extend, Disconnect                    ✓
 *   period            Extend                                                      Calculated
 *
 *   * = stored in HOTSPOT_SESSION_CACHE (SessionParams)
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
String OPERATOR_USERNAME;
String OPERATOR_PASSWORD;
String SITE_ID;
String SITE_NAME;
String ADMIN_USERNAME;
String ADMIN_PASSWORD;

static const unsigned long CONTROLLER_SESSION_MAX_AGE_MILLIS = 3600000;

// ---- Omada API implementations ----

// POST /api/v2/hotspot/login — gets CSRF token and session cookie for subsequent API calls.
// Reuses existing session if less than CONTROLLER_SESSION_MAX_AGE_MILLIS old.
bool loginToController(OmadaSession& op, String& errorDetail) {
  if (op.sessionStart > 0 && (millis() - op.sessionStart) < CONTROLLER_SESSION_MAX_AGE_MILLIS) {
    ESP_LOGI(TAG, "loginToController: Session age: %lu ms, reusing session", millis() - op.sessionStart);
    return true;
  }

  ESP_LOGI(TAG, "loginToController: No valid session, performing login");

  op.csrfToken = "";
  op.sessionStart = 0;

  WiFiClientSecure wifiClient;
  HTTPClient http;
  wifiClient.setInsecure();
  http.setCookieJar(&op.cookieJar);

  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/hotspot/login";
  String postData = "{\"name\":\"" + OPERATOR_USERNAME + "\",\"password\":\"" + OPERATOR_PASSWORD + "\"}";

  ESP_LOGI(TAG, "POST %s", url.c_str());
  ESP_LOGD(TAG, "POST data: %s", postData.c_str());

  http.begin(wifiClient, url);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(postData);

  if (httpCode <= 0) {
    errorDetail = "Login: " + http.errorToString(httpCode);
    ESP_LOGE(TAG, "HTTP POST failed: %s", errorDetail.c_str());
    return false;
  }

  String responseBody = http.getString();
  http.end();  // close connection — don't reuse across login boundary
  ESP_LOGD(TAG, "Response body: %s", responseBody.c_str());

  JsonDocument doc;
  if (deserializeJson(doc, responseBody)) {
    ESP_LOGE(TAG, "Login: JSON parse error: %.300s", responseBody.c_str());
    errorDetail = "Login: JSON parse error";
    return false;
  }

  if (doc["errorCode"].as<int>() == 0) {
    if (doc["result"]["token"].is<const char*>()) {
      op.csrfToken = doc["result"]["token"].as<String>();
      // Workaround: ESP32 HTTPClient sets cookie.date=0 because the Date: response header
      // arrives after Set-Cookie: in the Omada response. generateCookieString() then evaluates
      // 0 + max_age < now → true and erases the cookie before sending subsequent requests.
      // Patch the date to now so the expiry check passes.
      {
        time_t now_local = time(NULL);
        time_t now_gmt = mktime(gmtime(&now_local));
        for (auto& cookie : op.cookieJar) {
          if (cookie.name == "TPOMADA_SESSIONID") {
            cookie.date = now_gmt;
            ESP_LOGD(TAG, "Patched TPOMADA_SESSIONID cookie.date to %ld", (long)now_gmt);
            break;
          }
        }
      }
      op.sessionStart = millis();
      ESP_LOGI(TAG, "Login successful! Token: %s", op.csrfToken.c_str());
      return true;
    }
    errorDetail = "Login: response missing token";
  } else {
    String msg = doc["msg"].is<const char*>() ? doc["msg"].as<String>() : "errorCode " + String(doc["errorCode"].as<int>());
    errorDetail = "Login: " + msg;
    ESP_LOGE(TAG, "Login failed: %s", errorDetail.c_str());
  }

  return false;
}

// POST /api/v2/login — admin login for session reconstruction.
// Uses separate cookie jar and CSRF token from the operator session.
bool loginToAdminController(OmadaSession& admin, String& errorDetail) {
  if (admin.sessionStart > 0 && (millis() - admin.sessionStart) < CONTROLLER_SESSION_MAX_AGE_MILLIS) {
    return true;
  }
  if (ADMIN_USERNAME.isEmpty()) {
    errorDetail = "Admin credentials not configured";
    return false;
  }

  admin.csrfToken = "";
  admin.sessionStart = 0;

  WiFiClientSecure wifiClient;
  HTTPClient http;
  wifiClient.setInsecure();
  http.setCookieJar(&admin.cookieJar);

  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/login";
  String postData = "{\"username\":\"" + ADMIN_USERNAME + "\",\"password\":\"" + ADMIN_PASSWORD + "\"}";

  ESP_LOGI(TAG, "Admin login POST %s", url.c_str());
  http.begin(wifiClient, url);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(postData);
  if (httpCode <= 0) {
    errorDetail = "Admin login: " + http.errorToString(httpCode);
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    return false;
  }

  String responseBody = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, responseBody)) {
    ESP_LOGE(TAG, "Admin login: JSON parse error: %.300s", responseBody.c_str());
    errorDetail = "Admin login: JSON parse error";
    return false;
  }

  if (doc["errorCode"].as<int>() == 0) {
    if (doc["result"]["token"].is<const char*>()) {
      admin.csrfToken = doc["result"]["token"].as<String>();
      // Same cookie.date=0 workaround as operator login
      {
        time_t now_local = time(NULL);
        time_t now_gmt = mktime(gmtime(&now_local));
        for (auto& cookie : admin.cookieJar) {
          if (cookie.name == "TPOMADA_SESSIONID") {
            cookie.date = now_gmt;
            break;
          }
        }
      }
      admin.sessionStart = millis();
      ESP_LOGI(TAG, "Admin login successful");
      return true;
    }
    errorDetail = "Admin login: response missing token";
  } else {
    String msg = doc["msg"].is<const char*>() ? doc["msg"].as<String>() : "errorCode " + String(doc["errorCode"].as<int>());
    errorDetail = "Admin login: " + msg;
    ESP_LOGE(TAG, "Admin login failed: %s", errorDetail.c_str());
  }
  return false;
}

// GET /api/v2/user/sites — fetch the site ID from the controller at startup.
// Note - only takes the first one in the list
// Eliminates the need to hard-code omada_site_id in config.json.
bool loadOmadaSites(OmadaSession& admin, String& errorDetail) {
  if (!loginToAdminController(admin, errorDetail)) return false;

  WiFiClientSecure wifiClient;
  HTTPClient http;
  wifiClient.setInsecure();
  http.setCookieJar(&admin.cookieJar);

  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID +
               "/api/v2/user/sites?currentPage=1&currentPageSize=100";
  ESP_LOGI(TAG, "GET %s", url.c_str());

  http.begin(wifiClient, url);
  http.addHeader("Csrf-Token", admin.csrfToken);

  int httpCode = http.GET();
  if (httpCode <= 0) {
    errorDetail = "Sites: " + http.errorToString(httpCode);
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    return false;
  }

  String body = http.getString();
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    ESP_LOGE(TAG, "Sites: JSON parse error: %.300s", body.c_str());
    errorDetail = "Sites: JSON parse error";
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

bool authenticateOmadaClient(OmadaSession& op, SessionParams& session, unsigned long long durationMillis, String& errorDetail) {
  if (!loginToController(op, errorDetail)) return false;

  WiFiClientSecure wifiClient;
  HTTPClient http;
  wifiClient.setInsecure();
  http.setCookieJar(&op.cookieJar);

  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/hotspot/extPortal/auth";

  String postData = "{";
  postData += "\"clientMac\":\"" + session.clientMac + "\",";
#ifdef TEST_MODE
  postData += "\"clientIp\":\"" + session.clientIp + "\",";
#endif
  postData += "\"site\":\"" + SITE_NAME + "\",";
  postData += "\"time\":\"" + String(durationMillis) + "\",";
  postData += "\"authType\":4,";
  postData += "\"apMac\":\"" + session.apMac + "\",";
  postData += "\"ssidName\":\"" + session.ssidName + "\",";
  postData += "\"radioId\":\"" + session.radioId + "\"";
  postData += "}";

  ESP_LOGI(TAG, "Authenticating client: %s for %llu ms", session.clientMac.c_str(), durationMillis);
  ESP_LOGI(TAG, "POST %s", url.c_str());
  ESP_LOGD(TAG, "Payload: %s", postData.c_str());

  http.begin(wifiClient, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Csrf-Token", op.csrfToken);

  int httpCode = http.POST(postData);
  if (httpCode <= 0) {
    errorDetail = "Auth: " + http.errorToString(httpCode);
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    return false;
  }

  String body = http.getString();
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    ESP_LOGE(TAG, "Auth: JSON parse error: %.300s", body.c_str());
    errorDetail = "Auth: JSON parse error";
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
bool extendOmadaClient(OmadaSession& op, SessionParams& session, unsigned long long durationMillis, String& errorDetail) {
  if (!loginToController(op, errorDetail)) return false;

  WiFiClientSecure wifiClient;
  HTTPClient http;
  wifiClient.setInsecure();
  http.setCookieJar(&op.cookieJar);

  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID +
               "/api/v2/hotspot/sites/" + SITE_ID +
               "/cmd/clients/" + session.clientId + "/extend";
  String postData = "{\"period\":" + String((unsigned long long)durationMillis) + "}";

  ESP_LOGI(TAG, "POST %s body=%s", url.c_str(), postData.c_str());
  http.begin(wifiClient, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Csrf-Token", op.csrfToken);

  int httpCode = http.POST(postData);
  if (httpCode <= 0) {
    errorDetail = "Extend: " + http.errorToString(httpCode);
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    return false;
  }

  String body = http.getString();
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    ESP_LOGE(TAG, "Extend: JSON parse error: %.300s", body.c_str());
    errorDetail = "Extend: JSON parse error";
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
bool disconnectOmadaClient(OmadaSession& op, SessionParams& session, String& errorDetail) {
  if (!loginToController(op, errorDetail)) return false;

  WiFiClientSecure wifiClient;
  HTTPClient http;
  wifiClient.setInsecure();
  http.setCookieJar(&op.cookieJar);

  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID +
               "/api/v2/hotspot/sites/" + SITE_ID +
               "/cmd/clients/" + session.clientId + "/disconnect";

  ESP_LOGI(TAG, "POST %s (no body)", url.c_str());
  http.begin(wifiClient, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Csrf-Token", op.csrfToken);

  int httpCode = http.POST("");
  if (httpCode <= 0) {
    errorDetail = "Disconnect: " + http.errorToString(httpCode);
    ESP_LOGE(TAG, "%s", errorDetail.c_str());
    return false;
  }

  String body = http.getString();
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    ESP_LOGE(TAG, "Disconnect: JSON parse error: %.300s", body.c_str());
    errorDetail = "Disconnect: JSON parse error";
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

// Fetches and parses the hotspot client list (operator session).
// Returns a JsonDocument on success; empty document on failure.
JsonDocument getHotspotClientsJson(OmadaSession& op) {
  String errorDetail;
  if (!loginToController(op, errorDetail)) {
    ESP_LOGE(TAG, "getHotspotClientsJson: operator login failed: %s", errorDetail.c_str());
    return JsonDocument();
  }
  WiFiClientSecure wc;
  HTTPClient h;
  wc.setInsecure();
  h.setCookieJar(&op.cookieJar);
  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID +
               "/api/v2/hotspot/sites/" + SITE_ID +
               "/clients?sorts.end=desc&page=1&pageSize=100";
  ESP_LOGI(TAG, "GET %s", url.c_str());
  h.begin(wc, url);
  h.addHeader("Csrf-Token", op.csrfToken);
  int code = h.GET();
  if (code <= 0) {
    ESP_LOGE(TAG, "getHotspotClientsJson: HTTP error: %s", h.errorToString(code).c_str());
    return JsonDocument();
  }
  String body = h.getString();
  h.end();
  ESP_LOGI(TAG, "getHotspotClientsJson: HTTP %d, %d bytes", code, body.length());
  if (code != 200 || body.isEmpty()) {
    ESP_LOGE(TAG, "getHotspotClientsJson: bad response: %.200s - invalidating operator session", body.c_str());
    op.sessionStart = 0;
    return JsonDocument();
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) || doc["errorCode"].as<int>() != 0)
    ESP_LOGE(TAG, "getHotspotClientsJson: parse/API error: %.200s", body.c_str());
  return doc;
}

// Fetches and parses the all-clients list (admin session).
// Returns a JsonDocument on success; empty document on failure (non-fatal — merge handles gracefully).
JsonDocument getAllClientsJson(OmadaSession& admin) {
  String errorDetail;
  if (!loginToAdminController(admin, errorDetail)) {
    ESP_LOGW(TAG, "getAllClientsJson: admin login failed: %s", errorDetail.c_str());
    return JsonDocument();
  }
  WiFiClientSecure wc;
  HTTPClient h;
  wc.setInsecure();
  h.setCookieJar(&admin.cookieJar);
  // Omada UI equivalent: POST body {"filters":{"active":true},"sorts":{},"page":1,"pageSize":100,"scope":1}
  String url = CONTROLLER_BASE_URL + "/" + CONTROLLER_ID +
               "/api/v2/sites/" + SITE_ID + "/clients?page=1&pageSize=100&filters.active=true";
  ESP_LOGI(TAG, "GET %s", url.c_str());
  h.begin(wc, url);
  h.addHeader("Csrf-Token", admin.csrfToken);
  int code = h.GET();
  if (code <= 0) {
    ESP_LOGW(TAG, "getAllClientsJson: HTTP error: %s", h.errorToString(code).c_str());
    return JsonDocument();
  }
  String body = h.getString();
  h.end();
  ESP_LOGI(TAG, "getAllClientsJson: HTTP %d, %d bytes", code, body.length());
  if (code != 200 || body.isEmpty()) {
    ESP_LOGW(TAG, "getAllClientsJson: bad response: %.200s — invalidating admin session", body.c_str());
    admin.sessionStart = 0;
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
//   ip, apMac, ssid, radioId  — from all-clients (ip is "" if MAC not found there)
JsonDocument mergeClientLists(JsonArray hotspotClients, JsonArray allClients, uint64_t nowMs) {
  std::map<String, JsonObject> allMap;
  for (JsonObject c : allClients) allMap[c["mac"].as<String>()] = c;

  JsonDocument merged;
  JsonArray arr = merged.to<JsonArray>();
  for (JsonObject hs : hotspotClients) {
    uint64_t end = hs["end"].as<uint64_t>();
    if (end <= nowMs) continue;

    JsonObject entry = arr.add<JsonObject>();
    entry["mac"]   = hs["mac"];
    entry["id"]    = hs["id"];
    entry["start"] = hs["start"].as<uint64_t>();
    entry["end"]   = end;

    auto it = allMap.find(hs["mac"].as<String>());
    if (it != allMap.end()) {
      entry["ip"]      = it->second["ip"];
      entry["apMac"]   = it->second["apMac"];
      entry["ssid"]    = it->second["ssid"];
      entry["radioId"] = it->second["radioId"];
    } else {
      entry["ip"] = "";  // not yet visible in all-clients
    }
  }
  return merged;
}

// Merges the two client lists and updates the in-memory session cache.
// Called at startup, after any auth/extend/disconnect, and every 60s from loop().
void refreshHotspotSessionCache(OmadaSession& op, OmadaSession& admin) {
  JsonDocument hotspotDoc = getHotspotClientsJson(op);
  JsonDocument allDoc = getAllClientsJson(admin);

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
      // MAC known but IP changed (DHCP re-IP or all-clients lag resolved to new IP)
      session = macSession;
    } else if (!macSession && ipSession) {
      // IP known but MAC not indexed — vestigial entry from a previous client; remove and insert fresh
      ESP_LOGW(TAG, "Replacing stale cache entry at IP=%s with MAC=%s", ip.c_str(), mac.c_str());
      HOTSPOT_SESSION_CACHE.removeByIp(ip);
      session = &HOTSPOT_SESSION_CACHE.upsert(ip, mac);
      isNew = true;
    } else if (macSession && ipSession && macSession != ipSession) {
      // MAC moved to a different IP — remove old entry, upsert at new IP
      ESP_LOGI(TAG, "MAC %s moved IP: %s -> %s", mac.c_str(), macSession->clientIp.c_str(), ip.c_str());
      HOTSPOT_SESSION_CACHE.removeByIp(macSession->clientIp);
      session = &HOTSPOT_SESSION_CACHE.upsert(ip, mac);
    } else {
      // Normal: new client (both null) or existing at same IP (both point to same entry)
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
}

void omadaSetup() {
  OmadaSession admin;
  OmadaSession op;
  String error;
  if (!loadOmadaSites(admin, error)) {
    ESP_LOGE(TAG, "Failed to load Omada sites: %s - halting", error.c_str());
    halt();
  }
  refreshHotspotSessionCache(op, admin);
}

