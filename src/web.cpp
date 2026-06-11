#include "web.h"
#include "network.h"
#include "log_buffer.h"

#include <LittleFS.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <map>
#include <set>
#include <vector>
#include <Update.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <PsychicUploadHandler.h>
#include <mbedtls/base64.h>
#include <esp_log.h>
#include "logger.h"

static const char* TAG = "EllaFi";

// ── Admin auth ────────────────────────────────────────────────────────────────

static bool isAuthorized(PsychicRequest* request) {
  size_t len = httpd_req_get_hdr_value_len(request->request(), "Authorization");
  if (len == 0) return false;
  char buf[256] = {};
  if (len >= sizeof(buf)) return false;
  httpd_req_get_hdr_value_str(request->request(), "Authorization", buf, sizeof(buf));
  String hdr(buf);
  if (!hdr.startsWith("Basic ")) return false;
  unsigned char decoded[128] = {};
  size_t decodedLen = 0;
  String b64 = hdr.substring(6);
  mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decodedLen,
                        (const unsigned char*)b64.c_str(), b64.length());
  return String((char*)decoded) == ADMIN_USERNAME + ":" + ADMIN_PASSWORD;
}

static bool checkAdminAuth(PsychicRequest* request, PsychicResponse* response) {
  if (isAuthorized(request)) return true;
  response->addHeader("WWW-Authenticate", "Basic realm=\"EllaFi Admin\"");
  response->send(401, "text/plain", "Unauthorized");
  return false;
}

esp_err_t handleAdminPage(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  response->addHeader("Cache-Control", "no-store");
  return response->send(200, "text/html",
    (const uint8_t*)web_admin_html_start,
    web_admin_html_end - web_admin_html_start);
}

// ── HTTP / WS handlers ────────────────────────────────────────────────────────

// Serve captive portal page. Omada redirects here with query params:
// ?clientMac=...&apMac=...&ssidName=...&radioId=...&site=...&redirectUrl=...
esp_err_t handleRoot(PsychicRequest* request, PsychicResponse* response) {
#ifdef TEST_MODE
  String clientIp = request->hasParam("clientIp") ? request->getParam("clientIp")->value() : request->client()->remoteIP().toString();
#else
  String clientIp = request->client()->remoteIP().toString();
#endif
  String clientMac  = request->hasParam("clientMac")  ? request->getParam("clientMac")->value()  : "";
  String apMac      = request->hasParam("apMac")      ? request->getParam("apMac")->value()      : "";
  String ssidName   = request->hasParam("ssidName")   ? request->getParam("ssidName")->value()   : "";
  String radioId    = request->hasParam("radioId")    ? request->getParam("radioId")->value()     : "";

  SessionParams* existing = HOTSPOT_SESSION_CACHE.findByIp(clientIp);
  bool isNew = (existing == nullptr || existing->clientMac.isEmpty());

  ESP_LOGI(TAG, "======handleRoot [%s] ip=%s mac=%s apMac=%s ssid=%s radioId=%s paused=%lums openConn=%d",
    isNew ? "NEW" : "RETURNING",
    clientIp.c_str(), clientMac.c_str(), apMac.c_str(),
    ssidName.c_str(), radioId.c_str(),
    existing ? existing->pausedRemainingMillis : 0UL,
    (int)HTTP_SERVER.getClientList().size());

  // Guard: if clientMac is missing, look up by IP — do NOT upsert yet (would create a phantom entry)
  if (clientMac.isEmpty()) {
    if (!existing || existing->clientMac.isEmpty()) {
      ESP_LOGW(TAG, "No MAC for IP %s — rejecting portal request", clientIp.c_str());
      return response->send(200, "text/html",
        (const uint8_t*)web_unidentified_html_start,
        web_unidentified_html_end - web_unidentified_html_start);
    }
    clientMac = existing->clientMac;
    ESP_LOGI(TAG, "Restored MAC %s from cache for IP %s", clientMac.c_str(), clientIp.c_str());
  }

  SessionParams& session = HOTSPOT_SESSION_CACHE.upsert(clientIp, clientMac);
  if (!apMac.isEmpty())    session.apMac    = apMac;
  if (!ssidName.isEmpty()) session.ssidName = ssidName;
  if (!radioId.isEmpty())  session.radioId  = radioId;

  // Load paused session from disk if needed (outside session mutex — only LittleFS I/O)
  if (hasPausedSessionFile(clientMac) && session.pausedRemainingMillis == 0)
    session.pausedRemainingMillis = loadPausedSessionFile(clientMac);

  response->addHeader("Connection", "close");
  response->addHeader("Cache-Control", "no-store");
  return response->send(200, "text/html",
    (const uint8_t*)web_index_html_start,
    web_index_html_end - web_index_html_start);
}

esp_err_t handleGetStatus(PsychicRequest* request, PsychicResponse* response) {
#ifdef TEST_MODE
  String clientIp = request->hasParam("clientIp") ? request->getParam("clientIp")->value() : request->client()->remoteIP().toString();
#else
  String clientIp = request->client()->remoteIP().toString();
#endif
  SessionParams* session = HOTSPOT_SESSION_CACHE.findByIp(clientIp);
  String json = session ? buildStatusJson(*session)
    : "{\"type\":\"status\",\"sessionEndMillis\":0,\"pausedRemainingMillis\":0,\"coinInsertTimeLeft\":0}";
  response->addHeader("Cache-Control", "no-store");
  return response->send(200, "application/json", json.c_str());
}

esp_err_t handleNotFound(PsychicRequest* request, PsychicResponse* response) {
  String host = request->host();
  String url  = request->url();  // full path + query string
  if (host != "ellafi.local" && host != WiFi.localIP().toString())
    ESP_LOGW(TAG, "404 unexpected host: %s %s", host.c_str(), url.c_str());
  else
    ESP_LOGI(TAG, "404: %s", url.c_str());
  return response->send(404, "text/plain", "Not found");
}

esp_err_t handleProgram(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  COINSLOT_PROGRAM_MODE = true;
  digitalWrite(COINSLOT_POWER_PIN, HIGH);
  ESP_LOGI(TAG, "Coin slot program mode enabled — relay held HIGH until restart");
  return response->send(200, "text/plain", "Coin slot program mode ON — power held until restart.\nRestart ESP32 to exit.");
}

static String formatTimestamp(long ts) {
  char buf[20];
  time_t t = (time_t)ts;
  struct tm* tm = gmtime(&t);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
  return String(buf);
}

esp_err_t handleRefunds(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  if (!fsExists("/refunds.log")) return response->send(200, "text/plain", "No refunds logged.");
  File f = LittleFS.open("/refunds.log", FILE_READ);
  if (!f) return response->send(500, "text/plain", "Could not open refunds.log");
  String out = "REFUNDS\n=======\n";
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
    out += formatTimestamp(doc["ts"] | 0L);
    out += "  " + String(doc["mac"] | "?");
    out += "  " + String(doc["coins"] | 0) + " coin(s)";
    out += " / " + String(doc["minutes"] | 0) + " min";
    out += "  Code: " + String(doc["code"] | "?");
    out += "\n";
  }
  f.close();
  return response->send(200, "text/plain", out.c_str());
}

esp_err_t handleErrors(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  if (!fsExists("/errors.log")) return response->send(200, "text/plain", "No errors logged.");
  File f = LittleFS.open("/errors.log", FILE_READ);
  if (!f) return response->send(500, "text/plain", "Could not open errors.log");
  String out = "ERRORS\n======\n";
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
    out += formatTimestamp(doc["ts"] | 0L);
    out += "  [" + String(doc["tag"] | "?") + "]";
    out += "  " + String(doc["msg"] | "?");
    out += "\n";
  }
  f.close();
  return response->send(200, "text/plain", out.c_str());
}

void handleWsOpen(PsychicWebSocketClient* client) {
  int socketFd = client->socket();
  String clientIp = client->remoteIP().toString();
  ESP_LOGI(TAG, "WS client connected: socket #%d from %s", socketFd, clientIp.c_str());

  SessionParams* session = HOTSPOT_SESSION_CACHE.findByIp(clientIp);
  if (session) {
    session->socketFd = socketFd;
    ESP_LOGI(TAG, "WS client has session: socket #%d -> MAC %s", socketFd, session->clientMac.c_str());
  } else {
    ESP_LOGW(TAG, "WS socket #%d from %s: no cached session for this IP", socketFd, clientIp.c_str());
  }

  if (coinSessionUpdateSocket(clientIp, socketFd))
    ESP_LOGI(TAG, "Updated coin-session socket to %d (client reconnected)", socketFd);
}

esp_err_t handleWsRequest(PsychicWebSocketRequest* request, httpd_ws_frame* frame) {
  int socketFd = request->client()->socket();
  String clientIp = request->client()->remoteIP().toString();
  String payload((char*)frame->payload, frame->len);

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    ESP_LOGE(TAG, "WS JSON parse error (%s): %.200s", error.c_str(), payload.c_str());
    request->reply("{\"type\":\"error\",\"message\":\"Invalid WS JSON Request\"}");
    return ESP_OK;
  }

#ifdef TEST_MODE
  String payloadIp = doc["clientIp"] | "";
  if (!payloadIp.isEmpty()) clientIp = payloadIp;
#endif

  ESP_LOGI(TAG, "WS frame from socket #%d: clientIp=%s action=%s", socketFd, clientIp.c_str(), (doc["action"] | ""));

  SessionParams* session = HOTSPOT_SESSION_CACHE.findByIp(clientIp);
  if (session) {
    session->socketFd = socketFd;
    session->clientIp = clientIp;
  }

  if (!session) {
    request->reply("{\"type\":\"error\",\"subtype\":\"no_session\",\"message\":\"No portal session found for this device\"}");
    return ESP_OK;
  }

  String action = doc["action"] | "";

  if (action == "getStatus") {
    if (coinSessionUpdateSocket(clientIp, socketFd))
      ESP_LOGI(TAG, "Updated coin-session socket to %d (client reconnected)", socketFd);
    request->client()->sendMessage(buildStatusJson(*session).c_str());

  } else if (action == "startCoinInsert") {
    if (coinSessionTryClaim(socketFd, clientIp)) {
      ESP_LOGI(TAG, "Coin insertion started for MAC: %s via WS", session->clientMac.c_str());
      request->client()->sendMessage(buildStatusJson(*session).c_str());
    } else {
      request->reply("{\"type\":\"error\",\"message\":\"Another user already inserting coins\"}");
    }

  } else if (action == "pause") {
    WsJob job(JOB_PAUSE, session->clientMac);
    xQueueSend(WEBSOCKET_JOB_QUEUE, &job, 0);

  } else if (action == "resume") {
    WsJob job(JOB_RESUME, session->clientMac);
    xQueueSend(WEBSOCKET_JOB_QUEUE, &job, 0);

#ifdef TEST_MODE
  } else if (action == "testInsertCoin") {
    // Simulate a coin pulse (assumes startCoinInsert was called first)
    coinButtonPulse();
    ESP_LOGI(TAG, "Test coin inserted for MAC: %s (count: %d)", session->clientMac.c_str(), COIN_COUNT.load());
#endif
  } else {
    request->reply("{\"type\":\"error\",\"message\":\"Unknown action\"}");
  }

  return ESP_OK;
}

void handleWsClose(PsychicWebSocketClient* client) {
  ESP_LOGI(TAG, "WS client disconnected: socket #%d", client->socket());
}

String buildStatusJson(const SessionParams& session, const char* type) {
  CoinSessionStatus cs = coinSessionSnapshot();
  bool myCoinInsertActive = cs.accepting && coinIpToU32(session.clientIp) == cs.clientIpV4;
  int coinInsertTimeLeft = 0;
  if (myCoinInsertActive) {
    unsigned long now = millis();
    if (cs.deadlineMs > now) coinInsertTimeLeft = (cs.deadlineMs - now) / 1000;
  }

  String json = "{";
  json += "\"type\":\"" + String(type) + "\",";
  json += "\"coinCount\":" + String(myCoinInsertActive ? cs.count : 0) + ",";
  json += "\"coinInsertTimeLeft\":" + String(coinInsertTimeLeft) + ",";
  json += "\"sessionStartMillis\":" + String((unsigned long long)session.sessionStartMillis) + ",";
  json += "\"sessionEndMillis\":" + String((unsigned long long)session.sessionEndMillis) + ",";
  json += "\"pausedRemainingMillis\":" + String(session.pausedRemainingMillis) + ",";
  json += "\"clientMac\":\"" + session.clientMac + "\",";
  json += "\"clientIp\":\"" + session.clientIp + "\",";
  json += "\"minutesPerCoin\":" + String(MINUTES_PER_COIN);
  json += "}";

  return json;
}

void pushCoinUpdateToClient() {
  CoinSessionStatus cs = coinSessionSnapshot();
  if (cs.socketFd < 0 || cs.clientIpV4 == 0) return;

  SessionParams* session = HOTSPOT_SESSION_CACHE.findByIp(coinU32ToIp(cs.clientIpV4));
  if (!session) return;

  PsychicWebSocketClient* ws = WEBSOCKET_HANDLER.getClient(cs.socketFd);
  if (ws != nullptr) ws->sendMessage(buildStatusJson(*session).c_str());
}

// ── OTA update ───────────────────────────────────────────────────────────────

void setupOtaRoute() {
  PsychicUploadHandler* otaHandler = new PsychicUploadHandler();

  otaHandler->onUpload([](PsychicRequest* request, const String& /*filename*/,
                          uint64_t index, uint8_t* data, size_t len, bool final) -> esp_err_t {
    if (index == 0) {
      if (!isAuthorized(request)) {
        ESP_LOGW(TAG, "OTA upload rejected: unauthorized");
        return ESP_FAIL;
      }
      size_t contentLen = request->contentLength();
      ESP_LOGI(TAG, "OTA fw upload start: %zu bytes", contentLen);
      if (!Update.begin(contentLen)) {
        ESP_LOGE(TAG, "OTA Update.begin failed: %s", Update.errorString());
        return ESP_FAIL;
      }
      ESP_LOGI("ota", "fw Update.begin OK");
    }

    if (Update.write(data, len) != len) {
      ESP_LOGE(TAG, "OTA write failed at offset %llu len %zu", index, len);
      return ESP_FAIL;
    }

    if (final) {
      ESP_LOGI("ota", "fw final chunk received: %llu bytes total — calling Update.end", index + len);
      if (!Update.end(true)) {
        ESP_LOGE(TAG, "OTA Update.end failed: %s", Update.errorString());
        return ESP_FAIL;
      }
      ESP_LOGI("ota", "fw Update.end OK — partition committed");
      Preferences prefs;
      prefs.begin("ellafi", false);
      prefs.putUInt("fw_size", (uint32_t)(index + len));
      prefs.end();
      ESP_LOGI("ota", "fw fw_size=%llu saved to NVS", index + len);
    }
    return ESP_OK;
  });

  otaHandler->onRequest([](PsychicRequest* /*request*/, PsychicResponse* response) -> esp_err_t {
    ESP_LOGI("ota", "fw onRequest fired — sending 200 OK, reboot in 500ms");
    response->send(200, "application/json", "{\"ok\":true}");
    // Reboot master — slaves detect version mismatch via heartbeat and self-update
    TimerHandle_t t = xTimerCreate("OtaReboot", pdMS_TO_TICKS(500), pdFALSE, NULL,
                                   [](TimerHandle_t) { esp_restart(); });
    if (t) xTimerStart(t, 0);
    else   esp_restart();
    return ESP_OK;
  });

  HTTP_SERVER.maxUploadSize = 16 * 1024 * 1024;
  HTTP_SERVER.on("/update", HTTP_POST, otaHandler);
}

// ── Firmware serve (for slave OTA) ───────────────────────────────────────────

esp_err_t handleFwBin(PsychicRequest* request, PsychicResponse* response) {
  Preferences prefs;
  prefs.begin("ellafi", false);
  uint32_t fw_size = prefs.getUInt("fw_size", 0);
  prefs.end();

  if (fw_size == 0) {
    ESP_LOGW(TAG, "/fw.bin requested by %s but fw_size=0", request->client()->remoteIP().toString().c_str());
    return response->send(404, "text/plain", "Not found");
  }

  ESP_LOGI(TAG, "/fw.bin requested by %s — serving %u bytes from OTA partition",
           request->client()->remoteIP().toString().c_str(), (unsigned)fw_size);

  const esp_partition_t* partition = esp_ota_get_running_partition();
  response->addHeader("Content-Disposition", "attachment; filename=\"firmware.bin\"");

  // Stream directly from flash via mmap — no PSRAM copy needed.
  // response->send() with a raw pointer is synchronous, so munmap after is safe.
  const void* mapped = nullptr;
  spi_flash_mmap_handle_t mmap_handle;
  if (esp_partition_mmap(partition, 0, fw_size, SPI_FLASH_MMAP_DATA,
                         &mapped, &mmap_handle) == ESP_OK) {
    esp_err_t err = response->send(200, "application/octet-stream",
                                   (const uint8_t*)mapped, fw_size);
    spi_flash_munmap(mmap_handle);
    ESP_LOGI(TAG, "/fw.bin sent to %s (err=%d)", request->client()->remoteIP().toString().c_str(), err);
    return err;
  }

  // mmap failed — fall back to PSRAM copy
  ESP_LOGW(TAG, "/fw.bin: mmap failed, falling back to PSRAM copy");
  uint8_t* buf = (uint8_t*)heap_caps_malloc(fw_size, MALLOC_CAP_SPIRAM);
  if (!buf) {
    ESP_LOGE(TAG, "/fw.bin: PSRAM alloc failed for %u bytes", (unsigned)fw_size);
    return response->send(500, "text/plain", "Out of memory");
  }
  esp_partition_read(partition, 0, buf, fw_size);
  esp_err_t err = response->send(200, "application/octet-stream", buf, fw_size);
  heap_caps_free(buf);
  ESP_LOGI(TAG, "/fw.bin sent to %s (err=%d)", request->client()->remoteIP().toString().c_str(), err);
  return err;
}

// Serve the jsPDF library (embedded in firmware, so it ships with every flash — no separate
// FS step). Used by the admin page to generate voucher PDFs. Cached hard in the browser.
esp_err_t handleJsPdf(PsychicRequest* request, PsychicResponse* response) {
  response->addHeader("Cache-Control", "max-age=31536000, immutable");
  return response->send(200, "application/javascript",
    (const uint8_t*)web_jspdf_umd_min_js_start,
    web_jspdf_umd_min_js_end - web_jspdf_umd_min_js_start);
}


// ── Filesystem OTA ───────────────────────────────────────────────────────────

// PSRAM buffer for master's own FS self-flash, allocated during upload and freed after flashing.
static uint8_t* FS_PSRAM_BUFFER    = nullptr;
static size_t   FS_PSRAM_BUFFER_LEN = 0;
static size_t   FS_PSRAM_BUFFER_POS = 0;

void setupFsOtaRoute() {
  PsychicUploadHandler* fsHandler = new PsychicUploadHandler();

  fsHandler->onUpload([](PsychicRequest* request, const String& /*filename*/,
                         uint64_t index, uint8_t* data, size_t len, bool final) -> esp_err_t {
    if (index == 0) {
      if (!isAuthorized(request)) {
        ESP_LOGW(TAG, "FS OTA upload rejected: unauthorized");
        return ESP_FAIL;
      }
      size_t contentLen = request->contentLength();
      ESP_LOGI(TAG, "FS OTA upload start: %zu bytes", contentLen);
      if (FS_PSRAM_BUFFER) { heap_caps_free(FS_PSRAM_BUFFER); FS_PSRAM_BUFFER = nullptr; }
      FS_PSRAM_BUFFER    = (uint8_t*)heap_caps_malloc(contentLen, MALLOC_CAP_SPIRAM);
      FS_PSRAM_BUFFER_LEN = contentLen;
      FS_PSRAM_BUFFER_POS = 0;
      if (!FS_PSRAM_BUFFER) {
        ESP_LOGE("ota", "fs PSRAM alloc failed for %zu bytes", contentLen);
      } else {
        ESP_LOGI("ota", "fs PSRAM alloc OK: %zu bytes", contentLen);
      }
    }
    if (FS_PSRAM_BUFFER && FS_PSRAM_BUFFER_POS + len <= FS_PSRAM_BUFFER_LEN) {
      memcpy(FS_PSRAM_BUFFER + FS_PSRAM_BUFFER_POS, data, len);
      FS_PSRAM_BUFFER_POS += len;
    }
    if (final) {
      ESP_LOGI("ota", "fs upload final chunk done: %llu bytes total", index + len);
    }
    return ESP_OK;
  });

  fsHandler->onRequest([](PsychicRequest* /*request*/, PsychicResponse* response) -> esp_err_t {
    ESP_LOGI("ota", "fs onRequest fired — sending 200 OK");
    response->send(200, "application/json", "{\"ok\":true}");

    if (!FS_PSRAM_BUFFER || FS_PSRAM_BUFFER_LEN == 0) {
      ESP_LOGW("ota", "fs no PSRAM buf — skipping self-flash");
      return ESP_OK;
    }

    struct SavedFile { String path; uint8_t* buf; size_t len; };
    struct FsBuf {
      uint8_t* buf; size_t len;
      SavedFile preserved[4]; int nPreserved;
    };
    FsBuf* fb = new FsBuf{};
    fb->buf = FS_PSRAM_BUFFER;
    fb->len = FS_PSRAM_BUFFER_LEN;
    FS_PSRAM_BUFFER = nullptr;

    // Back up runtime data files before wiping the filesystem
    static const char* PRESERVE[] = {"/config.json", "/sellers.json", "/errors.log", "/refunds.log"};
    fb->nPreserved = 0;
    for (auto path : PRESERVE) {
      if (!LittleFS.exists(path)) continue;
      File f = LittleFS.open(path, "r");
      if (!f) continue;
      size_t sz = f.size();
      uint8_t* b = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
      if (b && f.read(b, sz) == sz) {
        fb->preserved[fb->nPreserved++] = {path, b, sz};
        ESP_LOGI(TAG, "FS OTA: preserved %s (%zu bytes)", path, sz);
      } else { heap_caps_free(b); }
      f.close();
    }

    TimerHandle_t t = xTimerCreate("FsFlash", pdMS_TO_TICKS(500), pdFALSE, fb,
      [](TimerHandle_t timer) {
        FsBuf* a = (FsBuf*)pvTimerGetTimerID(timer);
        ESP_LOGI("ota", "fs self-flash start: %zu bytes", a->len);
        LittleFS.end();
        bool beginOk = Update.begin(a->len, U_SPIFFS);
        if (!beginOk) {
          ESP_LOGE("ota", "fs Update.begin failed: %s", Update.errorString());
          for (int i = 0; i < a->nPreserved; i++) heap_caps_free(a->preserved[i].buf);
          heap_caps_free(a->buf); delete a; esp_restart(); return;
        }
        ESP_LOGI("ota", "fs Update.begin OK");
        size_t written = Update.write(a->buf, a->len);
        if (written != a->len) {
          ESP_LOGE("ota", "fs Update.write failed: wrote %zu of %zu", written, a->len);
          for (int i = 0; i < a->nPreserved; i++) heap_caps_free(a->preserved[i].buf);
          heap_caps_free(a->buf); delete a; esp_restart(); return;
        }
        ESP_LOGI("ota", "fs Update.write OK: %zu bytes", written);
        if (!Update.end(true)) {
          ESP_LOGE("ota", "fs Update.end failed: %s", Update.errorString());
          for (int i = 0; i < a->nPreserved; i++) heap_caps_free(a->preserved[i].buf);
          heap_caps_free(a->buf); delete a; esp_restart(); return;
        }
        ESP_LOGI("ota", "fs Update.end OK — partition committed");
        LittleFS.begin(false);
        heap_caps_free(a->buf);
        for (int i = 0; i < a->nPreserved; i++) {
          File rf = LittleFS.open(a->preserved[i].path, "w", true);
          if (rf) {
            rf.write(a->preserved[i].buf, a->preserved[i].len); rf.close();
            ESP_LOGI("ota", "fs restored %s", a->preserved[i].path.c_str());
          } else {
            ESP_LOGE("ota", "fs FAILED to restore %s", a->preserved[i].path.c_str());
          }
          heap_caps_free(a->preserved[i].buf);
        }
        delete a;
        ESP_LOGI("ota", "fs complete — rebooting");
        esp_restart();
      });
    if (t) xTimerStart(t, 0);
    else   esp_restart();
    return ESP_OK;
  });

  HTTP_SERVER.on("/update-fs", HTTP_POST, fsHandler);
}

// ── Admin nodes ───────────────────────────────────────────────────────────────

esp_err_t handleAdminNodes(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  JsonDocument doc;

  // Load node nicknames and commissions from sellers.json
  std::map<String, String> nicknames;
  std::map<String, int>    commissions;
  {
    File f = LittleFS.open("/sellers.json", "r");
    if (f) {
      JsonDocument sellers;
      if (deserializeJson(sellers, f) == DeserializationError::Ok) {
        for (JsonObject node : sellers["nodes"].as<JsonArray>()) {
          String mac = node["mac"].as<String>();
          mac.toUpperCase();
          nicknames[mac]   = node["nickname"].as<String>();
          commissions[mac] = node["commission"].as<int>();
        }
      }
      f.close();
    }
  }

  JsonArray nodes = doc["nodes"].to<JsonArray>();
  String ownMac = WiFi.macAddress();
  ownMac.toUpperCase();

  auto nickFor = [&](const String& mac) -> String {
    String upper = mac; upper.toUpperCase();
    auto it = nicknames.find(upper);
    return it != nicknames.end() ? it->second : "";
  };
  auto commFor = [&](const String& mac) -> int {
    String upper = mac; upper.toUpperCase();
    auto it = commissions.find(upper);
    return it != commissions.end() ? it->second : 0;
  };

  // Master node (self) — always online, info from own globals
  {
    JsonObject n = nodes.add<JsonObject>();
    n["mac"]        = WiFi.macAddress();
    n["ip"]         = WiFi.localIP().toString();
    n["role"]       = "master";
    n["nickname"]   = nickFor(ownMac);
    n["commission"] = commFor(ownMac);
    n["version"]    = FIRMWARE_VERSION;
    n["build"]      = (uint32_t)FIRMWARE_BUILD;
    n["uptimeSec"] = millis() / 1000UL;
    n["freeHeap"]  = ESP.getFreeHeap();
    n["rssi"]      = (int)WiFi.RSSI();
    n["online"]    = true;
  }

  // Slave nodes seen via heartbeat
  unsigned long now = millis();
  for (const NodeInfo& ni : getKnownNodes()) {
    String mac = ni.mac;  mac.toUpperCase();
    if (mac == ownMac) continue;  // skip own heartbeat echo
    JsonObject n = nodes.add<JsonObject>();
    n["mac"]        = ni.mac;
    n["ip"]         = ni.ip;
    n["role"]       = "slave";
    n["nickname"]   = nickFor(ni.mac);
    n["commission"] = commFor(ni.mac);
    n["version"]    = ni.version;
    n["build"]      = ni.buildNumber;
    n["uptimeSec"] = ni.uptimeSec;
    n["freeHeap"]  = ni.freeHeap;
    n["rssi"]      = (int)ni.rssi;
    n["online"]       = (now - ni.lastSeenMs) < 60000;
    n["otaSentCount"] = ni.otaSentCount;
    n["lastOtaSentMs"] = ni.lastOtaSentMs;
  }

  // Vendo active: sessions with sessionEndMillis > now
  int vendoActive = 0;
  {
    uint64_t nowMs = nowEpochMillis();
    HOTSPOT_SESSION_CACHE.lock();
    for (auto& kv : HOTSPOT_SESSION_CACHE) {
      if (kv.second.sessionEndMillis > nowMs) vendoActive++;
    }
    HOTSPOT_SESSION_CACHE.unlock();
  }
  doc["vendoActive"]   = vendoActive;
  doc["voucherActive"] = VOUCHER_ACTIVE_COUNT;

  // Omada controller status (cached from GET settings/system/status)
  {
    JsonObject ctrl = doc["controller"].to<JsonObject>();
    ctrl["reachable"]    = CONTROLLER_STATUS.reachable;
    ctrl["model"]        = CONTROLLER_STATUS.model;
    ctrl["version"]      = CONTROLLER_STATUS.version;
    ctrl["timeZone"]     = CONTROLLER_STATUS.timeZone;
    ctrl["uptimeMs"]     = CONTROLLER_STATUS.uptimeMillis;
    ctrl["clockSkewSec"] = CONTROLLER_STATUS.clockSkewSec;
  }

  // System health — master-only live metrics
  {
    JsonObject sys = doc["sys"].to<JsonObject>();
    sys["freeHeap"]           = ESP.getFreeHeap();
    sys["minFreeHeap"]        = ESP.getMinFreeHeap();
    sys["activeSockets"]      = (int)HTTP_SERVER.getClientList().size();
    sys["maxSockets"]         = HTTP_SERVER.config.max_open_sockets;
    sys["omadaSessionAgeMs"]  = getOmadaSessionAgeMs();
    sys["lastCacheRefreshMs"] = getLastCacheRefreshMs();
    sys["littleFsUsed"]       = LittleFS.usedBytes();
    sys["littleFsTotal"]      = LittleFS.totalBytes();
    sys["ntpSynced"]          = (time(NULL) > NTP_EPOCH_MIN);
    Preferences prefs;
    prefs.begin("ellafi", false);
    sys["fwSize"] = prefs.getUInt("fw_size", 0);
    prefs.end();
    sys["firmwareBuild"] = (uint32_t)FIRMWARE_BUILD;
  }

  String json;
  serializeJson(doc, json);
  response->addHeader("Cache-Control", "no-store");
  return response->send(200, "application/json", json.c_str());
}

// ── Admin log (ring buffer) ───────────────────────────────────────────────────

esp_err_t handleAdminLog(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  uint32_t sinceSeq = request->hasParam("since")
    ? (uint32_t)atoi(request->getParam("since")->value().c_str()) : 0;

  static const int BATCH = 50;
  LogEntry* entries = (LogEntry*)malloc(BATCH * sizeof(LogEntry));
  if (!entries) return response->send(500, "text/plain", "oom");

  int count = logBufferSnapshot(entries, BATCH, sinceSeq);

  JsonDocument doc;
  doc["seq"] = logBufferLastSeq();
  JsonArray arr = doc["entries"].to<JsonArray>();
  for (int i = 0; i < count; i++) {
    JsonObject e = arr.add<JsonObject>();
    e["seq"]   = entries[i].seq;
    e["ms"]    = entries[i].uptimeMs;
    e["ts"]    = (long)entries[i].ts;
    e["level"] = String(entries[i].level);
    e["tag"]   = entries[i].tag;
    e["msg"]   = entries[i].msg;
  }
  free(entries);

  String json;
  serializeJson(doc, json);
  response->addHeader("Cache-Control", "no-store");
  return response->send(200, "application/json", json.c_str());
}

// ── Admin sellers ─────────────────────────────────────────────────────────────

esp_err_t handleAdminSellers(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  response->addHeader("Cache-Control", "no-store");
  if (request->method() == HTTP_GET) {
    File f = LittleFS.open("/sellers.json", "r");
    if (!f) return response->send(200, "application/json", "{\"sellers\":[],\"nodes\":[]}");
    String content = f.readString(); f.close();
    return response->send(200, "application/json", content.c_str());
  }
  // POST — write sellers.json
  JsonDocument doc;
  if (deserializeJson(doc, request->body()) || !doc["sellers"].is<JsonArray>())
    return response->send(400, "text/plain", "Bad request");
  File f = LittleFS.open("/sellers.json", "w");
  if (!f) return response->send(500, "text/plain", "Failed to write file");
  serializeJson(doc, f); f.close();
  return response->send(200, "application/json", "{\"ok\":true}");
}

// ── Admin vouchers ────────────────────────────────────────────────────────────

esp_err_t handleAdminVouchers(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  response->addHeader("Cache-Control", "no-store");

  if (request->method() == HTTP_GET) {
    String groupId = request->hasParam("id") ? request->getParam("id")->value() : "";

    if (!groupId.isEmpty()) {
      // Return codes for a specific group page
      int page = request->hasParam("page") ? request->getParam("page")->value().toInt() : 1;
      JsonDocument raw = getVoucherCodesJson(groupId, page);
      if (raw.isNull()) return response->send(502, "text/plain", "Failed to fetch from controller");
      JsonDocument out;
      JsonArray codes = out["codes"].to<JsonArray>();
      for (JsonObject v : raw["result"]["data"].as<JsonArray>()) codes.add(v["code"].as<String>());
      int totalRows = raw["result"]["totalRows"].as<int>();
      out["totalPages"] = (totalRows + 129) / 130;
      String json; serializeJson(out, json);
      return response->send(200, "application/json", json.c_str());
    }

    // List all voucher groups filtered by AUTOGEN: prefix
    JsonDocument raw = getVoucherGroupsJson();
    if (raw.isNull()) return response->send(502, "text/plain", "Failed to fetch from controller");
    JsonDocument out;
    JsonArray arr = out.to<JsonArray>();
    for (JsonObject g : raw["result"]["data"].as<JsonArray>()) {
      String name = g["name"].as<String>();
      if (!name.startsWith("AUTOGEN: ")) continue;
      String rest   = name.substring(9);           // "5P2H KANO"
      int spaceIdx  = rest.indexOf(' ');
      String seller = spaceIdx >= 0 ? rest.substring(spaceIdx + 1) : "";
      int price = 0;
      {
        JsonDocument descDoc;
        if (deserializeJson(descDoc, g["description"].as<String>()) == DeserializationError::Ok)
          price = descDoc["price"].as<int>();
      }
      // totalCount: prefer top-level field, fall back to statisticsCount (structure varies between list and detail endpoints)
      int totalCount = g["totalCount"].as<int>();
      if (totalCount == 0) totalCount = g["statisticsCount"]["totalStatisticsCount"].as<int>();
      JsonObject entry = arr.add<JsonObject>();
      entry["id"]          = g["id"];
      entry["name"]        = name;
      entry["seller"]      = seller;
      entry["pages"]       = max(1, (totalCount + 129) / 130);
      entry["duration"]    = g["duration"].as<int>();
      entry["price"]       = price;
      entry["createdTime"] = g["createdTime"].as<uint64_t>();
      entry["unusedCount"] = g["unusedCount"].as<int>();
      entry["usedCount"]   = g["usedCount"].as<int>();
      entry["totalCount"]  = totalCount;
    }
    String json; serializeJson(out, json);
    return response->send(200, "application/json", json.c_str());
  }

  // DELETE — remove a voucher group
  if (request->method() == HTTP_DELETE) {
    String groupId = request->hasParam("id") ? request->getParam("id")->value() : "";
    if (groupId.isEmpty()) return response->send(400, "text/plain", "Missing id");
    String errorDetail;
    if (!deleteVoucherGroup(groupId, errorDetail))
      return response->send(502, "text/plain", ("Controller error: " + errorDetail).c_str());
    return response->send(200, "application/json", "{\"ok\":true}");
  }

  // POST — create new voucher group
  JsonDocument req;
  if (deserializeJson(req, request->body())) return response->send(400, "text/plain", "Bad JSON");
  int pages       = req["pages"].as<int>();
  int durationMin = req["duration_min"].as<int>();
  int price       = req["price"].as<int>();
  String seller   = req["seller"].as<String>();
  if (pages < 1 || durationMin < 1 || price < 1 || seller.isEmpty())
    return response->send(400, "text/plain", "Missing required fields");

  int qty          = pages * 130;
  int durationHrs  = durationMin / 60;
  String name      = "AUTOGEN: " + String(price) + "P" + String(durationHrs) + "H " + seller;
  String desc      = "{\"price\":" + String(price) + "}";

  String groupId, errorDetail;
  if (!createVoucherGroup(name, durationMin, qty, desc, groupId, errorDetail))
    return response->send(502, "text/plain", ("Controller error: " + errorDetail).c_str());

  // Fetch first page of codes immediately so the browser can print without a second round-trip
  JsonDocument codesDoc = getVoucherCodesJson(groupId, 1);
  JsonDocument out;
  out["id"]    = groupId;
  out["pages"] = pages;
  JsonArray codes = out["codes"].to<JsonArray>();
  if (!codesDoc.isNull())
    for (JsonObject v : codesDoc["result"]["data"].as<JsonArray>()) codes.add(v["code"].as<String>());
  String json; serializeJson(out, json);
  return response->send(200, "application/json", json.c_str());
}

// ── Voucher cache helpers ─────────────────────────────────────────────────────

static JsonArray voucherBuckets(JsonDocument& doc) {
  JsonArray arr = doc["result"]["usage"].as<JsonArray>();
  if (arr.isNull()) arr = doc["result"]["data"].as<JsonArray>();
  return arr;
}

static int voucherBucketRevenue(JsonObject b, int fallbackPrice) {
  float amt = atof(b["amount"].as<const char*>() ?: "0");
  if (amt == 0) amt = b["count"].as<int>() * fallbackPrice;
  return (int)amt;
}

// ── Admin sales ───────────────────────────────────────────────────────────────

// Day bucketing in the controller's local time. TZ is set at startup from the
// controller's zone (applyControllerTimezone), so localtime()/mktime() apply the
// correct offset AND daylight-saving. localDayNum increments by 1 at each local
// midnight; localDayStartUtc is its inverse (UTC epoch of a local day's midnight),
// used for Omada range queries and as the voucher-cache key. DST-correct because
// the per-instant offset comes from localtime, not a fixed constant.

// Days since 1970-01-01 for a civil (proleptic Gregorian) date. Hinnant's algorithm.
static long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}
static long localDayNum(time_t utc) {
  struct tm lt;
  localtime_r(&utc, &lt);
  return daysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}
static time_t localDayStartUtc(long dayNum) {
  time_t midnightAsUtc = (time_t)dayNum * 86400;
  struct tm lt;
  gmtime_r(&midnightAsUtc, &lt);   // recover Y/M/D 00:00:00 of that local day
  lt.tm_isdst = -1;
  return mktime(&lt);              // interpret as local → real UTC epoch (DST-aware)
}

esp_err_t handleAdminSales(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  response->addHeader("Cache-Control", "no-store");

  time_t now      = time(NULL);
  long   todayDay = localDayNum(now);
  static const int DAYS = 90;

  // ── Vendo history ──────────────────────────────────────────────────────────
  int dayCoinTotals[DAYS] = {};
  int dayMinTotals[DAYS]  = {};
  std::map<String, std::vector<int>> nodeCoins;

  if (fsExists("/vendo_history.json")) {
    File f = LittleFS.open("/vendo_history.json", FILE_READ);
    if (f) {
      while (f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        if (line.isEmpty()) continue;
        JsonDocument doc;
        if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
        long day = localDayNum((time_t)(doc["ts"] | 0L));
        int  idx = (int)(todayDay - day);
        if (idx < 0 || idx >= DAYS) continue;
        int coins = doc["coins"] | 0;
        dayCoinTotals[DAYS - 1 - idx] += coins;
        dayMinTotals [DAYS - 1 - idx] += doc["min"] | 0;
        String node = doc["node"] | "";
        if (!node.isEmpty()) {
          if (nodeCoins.find(node) == nodeCoins.end()) nodeCoins[node].assign(DAYS, 0);
          nodeCoins[node][DAYS - 1 - idx] += coins;
        }
      }
      f.close();
    }
  }

  // ── Voucher history cache ──────────────────────────────────────────────────
  int dayVoucherTotals[DAYS] = {};
  int dayVchrRevTotals[DAYS] = {};

  std::map<long, std::pair<int,int>> vchrCache;  // day → {count, rev}
  if (fsExists("/voucher_history.json")) {
    File f = LittleFS.open("/voucher_history.json", FILE_READ);
    if (f) {
      while (f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        if (line.isEmpty()) continue;
        JsonDocument doc;
        if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
        long day = localDayNum((time_t)(doc["ts"] | 0L));
        if (day > 0) vchrCache[day] = {doc["count"] | 0, doc["revenue"] | 0};
      }
      f.close();
    }
  }

  // Find all missing past days and fetch in one call
  std::set<long> missingDays;
  for (int i = 0; i < DAYS - 1; i++) {
    long day = todayDay - (DAYS - 1) + i;
    if (!vchrCache.count(day)) missingDays.insert(day);
  }
  if (!missingDays.empty()) {
    // Omada end-stamps each daily bucket at the NEXT local midnight, so the bucket
    // for day L sits at localDayStartUtc(L+1). Widen the window by an extra day so
    // the last missing day's bucket is comfortably inside it (extra buckets are
    // filtered by missingDays below).
    time_t startSec = localDayStartUtc(*missingDays.begin());
    time_t endSec   = localDayStartUtc(*missingDays.rbegin() + 2);
    JsonDocument histDoc = getVoucherHistoryJson(startSec, endSec);
    if (!histDoc.isNull()) {
      // Accumulate all sub-day buckets by day before caching
      for (JsonObject b : voucherBuckets(histDoc)) {
        // Omada stamps the bucket at the day's END (next local midnight); -1s lands
        // it in the day it actually represents.
        long day = localDayNum((time_t)(b["time"].as<long long>() / 1000) - 1);
        if (!missingDays.count(day)) continue;
        vchrCache[day].first  += b["count"].as<int>();
        vchrCache[day].second += voucherBucketRevenue(b, PRICE_PER_COIN);
      }
      // Write one entry per missing day (days with no API response get cached as 0)
      File cacheFile = LittleFS.open("/voucher_history.json", FILE_APPEND);
      if (cacheFile) {
        for (long day : missingDays) {
          auto& v = vchrCache[day];
          JsonDocument e; e["ts"] = (long)localDayStartUtc(day); e["count"] = v.first; e["revenue"] = v.second;
          serializeJson(e, cacheFile); cacheFile.print('\n');
        }
        cacheFile.close();
      }
    }
  }

  // Apply cached past days
  for (int i = 0; i < DAYS - 1; i++) {
    long day = todayDay - (DAYS - 1) + i;
    auto it = vchrCache.find(day);
    if (it != vchrCache.end()) {
      dayVoucherTotals[i] += it->second.first;
      dayVchrRevTotals[i] += it->second.second;
    }
  }

  // Always fetch today live — incomplete day, never cache
  {
    // Fetch the whole current local day; Omada returns the partial bucket end-stamped
    // at tomorrow's local midnight, which -1s attributes back to today.
    JsonDocument todayDoc = getVoucherHistoryJson(localDayStartUtc(todayDay), localDayStartUtc(todayDay + 1));
    if (!todayDoc.isNull()) {
      for (JsonObject b : voucherBuckets(todayDoc)) {
        long day = localDayNum((time_t)(b["time"].as<long long>() / 1000) - 1);
        if (day != todayDay) continue;
        dayVoucherTotals[DAYS - 1] += b["count"].as<int>();
        dayVchrRevTotals[DAYS - 1] += voucherBucketRevenue(b, PRICE_PER_COIN);
      }
    }
  }

  // ── Build response ─────────────────────────────────────────────────────────
  JsonDocument out;
  out["pricePerCoin"] = PRICE_PER_COIN;
  JsonArray days = out["days"].to<JsonArray>();
  for (int i = 0; i < DAYS; i++) {
    time_t   dayTs = (time_t)((todayDay - (DAYS - 1 - i)) * 86400 + 43200);
    struct tm* tm  = gmtime(&dayTs);
    char dateBuf[24];
    snprintf(dateBuf, sizeof(dateBuf), "%d/%d", tm->tm_mon + 1, tm->tm_mday);
    JsonObject d = days.add<JsonObject>();
    d["date"]       = dateBuf;
    d["coins"]      = dayCoinTotals[i];
    d["minutes"]    = dayMinTotals[i];
    d["vouchers"]   = dayVoucherTotals[i];
    d["voucherRev"] = dayVchrRevTotals[i];
    if (!nodeCoins.empty()) {
      JsonObject nodes = d["nodes"].to<JsonObject>();
      for (auto& kv : nodeCoins) nodes[kv.first.c_str()] = kv.second[i];
    }
  }
  String json; serializeJson(out, json);
  return response->send(200, "application/json", json.c_str());
}

// ── Admin leaderboard ─────────────────────────────────────────────────────────

esp_err_t handleAdminLeaderboard(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  response->addHeader("Cache-Control", "no-store");

  time_t now    = time(NULL);
  int    nDays  = request->hasParam("days")
                  ? (int)max(1L, min(request->getParam("days")->value().toInt(), 365L)) : 30;
  time_t cutoff = now - (time_t)nDays * 86400;

  // Load seller commission rates
  std::map<String, int> commissions;
  if (fsExists("/sellers.json")) {
    File f = LittleFS.open("/sellers.json", "r");
    if (f) {
      JsonDocument doc;
      if (deserializeJson(doc, f) == DeserializationError::Ok)
        for (JsonObject s : doc["sellers"].as<JsonArray>()) {
          String name = s["name"].as<String>();
          if (!name.isEmpty()) commissions[name] = s["commission"].as<int>();
        }
      f.close();
    }
  }

  // Aggregate usedCount per seller from AUTOGEN groups created within the period
  std::map<String, std::pair<int,int>> totals;  // seller → {vouchers, revenue}
  JsonDocument groupsDoc = getVoucherGroupsJson();
  if (!groupsDoc.isNull()) {
    for (JsonObject g : groupsDoc["result"]["data"].as<JsonArray>()) {
      String name = g["name"].as<String>();
      if (!name.startsWith("AUTOGEN: ")) continue;
      long long createdMs = g["createdTime"].as<long long>();
      if (createdMs > 0 && (time_t)(createdMs / 1000) < cutoff) continue;
      String rest   = name.substring(9);
      int    space  = rest.indexOf(' ');
      String seller = space >= 0 ? rest.substring(space + 1) : rest;
      int    price  = PRICE_PER_COIN;
      JsonDocument descDoc;
      if (deserializeJson(descDoc, g["description"].as<String>()) == DeserializationError::Ok)
        price = descDoc["price"] | PRICE_PER_COIN;
      int used = g["usedCount"].as<int>();
      totals[seller].first  += used;
      totals[seller].second += used * price;
    }
  }

  // Sort by revenue descending
  using SelEntry = std::pair<String, std::pair<int,int>>;
  std::vector<SelEntry> sorted(totals.begin(), totals.end());
  std::sort(sorted.begin(), sorted.end(),
    [](const SelEntry& a, const SelEntry& b){ return a.second.second > b.second.second; });

  JsonDocument out;
  JsonArray arr = out.to<JsonArray>();
  for (auto& kv : sorted) {
    int  comm = commissions.count(kv.first) ? commissions[kv.first] : 20;
    bool reg  = commissions.count(kv.first) > 0;
    JsonObject s = arr.add<JsonObject>();
    s["seller"]     = kv.first;
    s["vouchers"]   = kv.second.first;
    s["revenue"]    = kv.second.second;
    s["commission"] = comm;
    s["net"]        = kv.second.second - (kv.second.second * comm / 100);
    s["registered"] = reg;
  }
  String json; serializeJson(out, json);
  return response->send(200, "application/json", json.c_str());
}

// ── Admin config ──────────────────────────────────────────────────────────────

esp_err_t handleAdminConfig(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  response->addHeader("Cache-Control", "no-store");
  if (request->method() == HTTP_GET) {
    File f = LittleFS.open("/config.json", "r");
    if (!f) return response->send(404, "text/plain", "Not found");
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return response->send(500, "text/plain", "Parse error"); }
    f.close();
    // Mask passwords
    for (const char* k : {"wifi_password", "omada_password", "network_key"})
      if (!doc[k].isNull()) doc[k] = "••••••••";
    String json; serializeJson(doc, json);
    return response->send(200, "application/json", json.c_str());
  }
  // POST — merge onto existing config, validate, then write and restart
  JsonDocument doc;
  if (deserializeJson(doc, request->body())) return response->send(400, "text/plain", "Bad JSON");

  // Start from the current config so omitted/blank fields (e.g. unchanged passwords) survive.
  JsonDocument merged;
  { File rf = LittleFS.open("/config.json", "r"); if (rf) { deserializeJson(merged, rf); rf.close(); } }
  for (JsonPair kv : doc.as<JsonObject>()) {
    // Ignore masked placeholders posted back unchanged — never overwrite a secret with "••••••••".
    if (kv.value().is<const char*>() && String(kv.value().as<const char*>()) == "••••••••") continue;
    merged[kv.key()] = kv.value();
  }

  // Refuse to write a config that would brick the device (can't join WiFi / reach Omada).
  for (const char* k : {"wifi_ssid", "wifi_password", "omada_url", "omada_controller_id",
                        "omada_username", "omada_password", "network_key"}) {
    bool emptyStr = merged[k].is<const char*>() && String(merged[k].as<const char*>()).isEmpty();
    if (merged[k].isNull() || emptyStr)
      return response->send(400, "text/plain", (String("Missing required field: ") + k).c_str());
  }

  File f = LittleFS.open("/config.json", "w");
  if (!f) return response->send(500, "text/plain", "Failed to write");
  serializeJson(merged, f); f.close();
  response->send(200, "text/plain", "Saved — restarting...");
  delay(500);
  esp_restart();
  return ESP_OK;
}

// ── Admin reboot ──────────────────────────────────────────────────────────────

esp_err_t handleAdminReboot(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  response->send(200, "text/plain", "Rebooting...");
  delay(200);
  esp_restart();
  return ESP_OK;
}

esp_err_t handleAdminClearVoucherCache(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  if (fsExists("/voucher_history.json")) LittleFS.remove("/voucher_history.json");
  ESP_LOGI(TAG, "Voucher cache cleared by admin");
  return response->send(200, "text/plain", "OK");
}

// ── GitHub OTA (pull the latest published build over HTTPS) ───────────────────
// jsPDF and the web UI are embedded in firmware.bin, so an update only needs the firmware
// (the filesystem holds device-local data we never ship). CI publishes firmware.bin +
// version.json to GitHub Pages; the master pulls firmware.bin and flashes itself, then
// slaves auto-follow via the existing heartbeat/OTA-available mechanism.
// NOTE: fetched with setInsecure() (no cert check) — MITM risk, audit S2; real fix is a
// signed firmware image. Acceptable for a hardcoded github.io URL as a first cut.
static const char* GITHUB_BASE = "https://paulkilroy.github.io/EllaFi";

// Streams firmware.bin from a URL into the OTA partition. Returns true on success.
static bool flashFirmwareFromUrl(const String& url, size_t& written, String& err) {
  WiFiClientSecure wc;
  wc.setInsecure();
  wc.setTimeout(20000);
  HTTPClient http;
  http.setTimeout(60000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(wc, url)) { err = "connect failed"; return false; }
  int code = http.GET();
  if (code != 200) { err = "HTTP " + String(code); http.end(); return false; }
  int total = http.getSize();
  if (total <= 0) { err = "unknown content length"; http.end(); return false; }
  if (!Update.begin(total, U_FLASH)) { err = Update.errorString(); http.end(); return false; }
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  int remaining = total;
  unsigned long lastData = millis();
  while (http.connected() && remaining > 0) {
    int avail = stream->available();
    if (avail > 0) {
      int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
      if (n > 0) { Update.write(buf, n); remaining -= n; lastData = millis(); }
    } else if (millis() - lastData > 10000) {
      err = "download stalled"; http.end(); Update.abort(); return false;
    } else {
      delay(1);
    }
  }
  http.end();
  if (remaining > 0) { err = "incomplete (" + String(total - remaining) + "/" + String(total) + ")"; Update.abort(); return false; }
  if (!Update.end(true)) { err = Update.errorString(); return false; }
  written = total;
  return true;
}

static void githubOtaTask(void*) {
  ESP_LOGI(TAG, "GitHub OTA: downloading firmware...");
  size_t written = 0;
  String err;
  if (flashFirmwareFromUrl(String(GITHUB_BASE) + "/firmware.bin", written, err)) {
    Preferences prefs; prefs.begin("ellafi", false);
    prefs.putUInt("fw_size", (uint32_t)written); prefs.end();  // so slaves can pull /fw.bin from us
    ESP_LOGI(TAG, "GitHub OTA: flashed %u bytes — rebooting (slaves follow via heartbeat)", (unsigned)written);
    delay(1500);
    esp_restart();
  } else {
    ESP_LOGE(TAG, "GitHub OTA failed: %s", err.c_str());
  }
  vTaskDelete(NULL);
}

// GET /admin/github-check → compare our build to GitHub's version.json (device fetches it,
// avoiding the browser CORS block on github.io).
esp_err_t handleGithubCheck(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  WiFiClientSecure wc; wc.setInsecure(); wc.setTimeout(10000);
  HTTPClient http; http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int latest = 0; String version, err;
  if (http.begin(wc, String(GITHUB_BASE) + "/version.json")) {
    int code = http.GET();
    if (code == 200) {
      JsonDocument d;
      if (deserializeJson(d, http.getString())) err = "parse error";
      else { latest = d["build"] | 0; version = d["version"] | ""; }
    } else err = "HTTP " + String(code);
    http.end();
  } else err = "connect failed";

  JsonDocument out;
  out["current"]         = (uint32_t)FIRMWARE_BUILD;
  out["latest"]          = latest;
  out["version"]         = version;
  out["updateAvailable"] = latest > (int)FIRMWARE_BUILD;
  if (err.length()) out["error"] = err;
  String s; serializeJson(out, s);
  response->addHeader("Cache-Control", "no-store");
  return response->send(200, "application/json", s.c_str());
}

// POST /admin/update-github → kick off the pull-and-flash in a background task.
esp_err_t handleGithubUpdate(PsychicRequest* request, PsychicResponse* response) {
  if (!checkAdminAuth(request, response)) return ESP_OK;
  if (xTaskCreate(githubOtaTask, "GithubOTA", 16384, NULL, 5, NULL) != pdPASS)
    return response->send(500, "text/plain", "Could not start update");
  return response->send(200, "application/json", "{\"ok\":true}");
}

// ── Server setup ─────────────────────────────────────────────────────────────

void setupWeb() {
  if (MDNS.begin("ellafi")) {
    ESP_LOGI(TAG, "mDNS started: ellafi.local");
  } else {
    ESP_LOGW(TAG, "mDNS failed to start");
  }

  HTTP_SERVER.config.stack_size       = 16384;
  HTTP_SERVER.config.max_open_sockets = 12;
  HTTP_SERVER.config.lru_purge_enable = true;

  HTTP_SERVER.on("/", HTTP_GET, handleRoot);

  WEBSOCKET_HANDLER.onOpen(handleWsOpen);
  WEBSOCKET_HANDLER.onFrame(handleWsRequest);
  WEBSOCKET_HANDLER.onClose(handleWsClose);
  HTTP_SERVER.on("/ws", &WEBSOCKET_HANDLER);

  HTTP_SERVER.on("/refunds",           HTTP_GET,  handleRefunds);
  HTTP_SERVER.on("/errors",            HTTP_GET,  handleErrors);
  HTTP_SERVER.on("/program",           HTTP_GET,  handleProgram);
  HTTP_SERVER.on("/admin/nodes",       HTTP_GET,  handleAdminNodes);
  HTTP_SERVER.on("/admin/log",         HTTP_GET,  handleAdminLog);
  HTTP_SERVER.on("/admin/sales",       HTTP_GET,  handleAdminSales);
  HTTP_SERVER.on("/admin/leaderboard", HTTP_GET,  handleAdminLeaderboard);
  HTTP_SERVER.on("/admin/sellers",     HTTP_ANY,  handleAdminSellers);
  HTTP_SERVER.on("/admin/vouchers",    HTTP_ANY,   handleAdminVouchers);
  HTTP_SERVER.on("/admin/config",      HTTP_ANY,  handleAdminConfig);
  HTTP_SERVER.on("/admin/reboot",             HTTP_POST, handleAdminReboot);
  HTTP_SERVER.on("/admin/clear-voucher-cache", HTTP_POST, handleAdminClearVoucherCache);
  HTTP_SERVER.on("/admin/github-check",        HTTP_GET,  handleGithubCheck);
  HTTP_SERVER.on("/admin/update-github",       HTTP_POST, handleGithubUpdate);
  HTTP_SERVER.on("/admin",             HTTP_GET,  handleAdminPage);
  HTTP_SERVER.on("/config.json", HTTP_GET, [](PsychicRequest* req, PsychicResponse* res) {
    return res->send(403, "text/plain", "Forbidden");
  });
  HTTP_SERVER.on("/status",     HTTP_GET, handleGetStatus);
  HTTP_SERVER.on("/index.html", HTTP_GET, handleRoot);
  setupOtaRoute();
  setupFsOtaRoute();
  HTTP_SERVER.on("/fw.bin", HTTP_GET, handleFwBin);
  HTTP_SERVER.on("/jspdf.umd.min.js", HTTP_GET, handleJsPdf);

  HTTP_SERVER.on("/assets/insertcoinbg.mp3", HTTP_GET, [](PsychicRequest* req, PsychicResponse* res) {
    res->addHeader("Cache-Control", "max-age=86400");
    return res->send(200, "audio/mpeg", (const uint8_t*)web_assets_insertcoinbg_mp3_start, web_assets_insertcoinbg_mp3_end - web_assets_insertcoinbg_mp3_start);
  });
  HTTP_SERVER.on("/assets/coin-received.mp3", HTTP_GET, [](PsychicRequest* req, PsychicResponse* res) {
    res->addHeader("Cache-Control", "max-age=86400");
    return res->send(200, "audio/mpeg", (const uint8_t*)web_assets_coin_received_mp3_start, web_assets_coin_received_mp3_end - web_assets_coin_received_mp3_start);
  });
  HTTP_SERVER.on("/EllaFi.webp", HTTP_GET, [](PsychicRequest* req, PsychicResponse* res) {
    res->addHeader("Cache-Control", "max-age=86400");
    return res->send(200, "image/webp", (const uint8_t*)web_EllaFi_webp_start, web_EllaFi_webp_end - web_EllaFi_webp_start);
  });
  HTTP_SERVER.on("/favicon.ico", HTTP_GET, [](PsychicRequest* req, PsychicResponse* res) {
    res->addHeader("Cache-Control", "max-age=86400");
    return res->send(200, "image/x-icon", (const uint8_t*)web_favicon_ico_start, web_favicon_ico_end - web_favicon_ico_start);
  });

  HTTP_SERVER.onNotFound(handleNotFound);
  HTTP_SERVER.begin();
  ESP_LOGI(TAG, "Web server started");
}

// ── FreeRTOS tasks ────────────────────────────────────────────────────────────

// Pushes WS status updates to the inserting client.
// Woken immediately by coinPulseTask on each coin (for instant coin count feedback),
// or fires on the 500ms timeout to keep the countdown timer in the UI updated.
// No-op when no session is active.
void updateClientTask(void*) {
  for (;;) {
    xTaskNotifyWait(0, ULONG_MAX, NULL, pdMS_TO_TICKS(UPDATE_CLIENT_INTERVAL_MILLIS));
    if (COIN_INSERT_ACTIVE.load()) pushCoinUpdateToClient();
  }
}

// WebSocketTask: drains PAUSE/RESUME/REFRESH_CACHE jobs posted by WS handlers.
// Pinned to Core 0 — runs alongside PsychicHTTP tasks that enqueue the jobs.
void webSocketTask(void*) {
  WsJob job;
  for (;;) {
    if (xQueueReceive(WEBSOCKET_JOB_QUEUE, &job, portMAX_DELAY) == pdTRUE) {
      if (job.type == JOB_PAUSE)         processPause(String(job.mac));
      if (job.type == JOB_RESUME)        processResume(String(job.mac));
      if (job.type == JOB_REFRESH_CACHE) refreshHotspotSessionCache();
    }
  }
}

void processPause(const String& mac) {
  SessionParams* session = HOTSPOT_SESSION_CACHE.findByMac(mac);
  if (!session) { ESP_LOGW(TAG, "processPause: no session for MAC=%s", mac.c_str()); return; }

  PsychicWebSocketClient* ws = WEBSOCKET_HANDLER.getClient(session->socketFd);  // may be null if WS disconnected
  uint64_t nowMillis = nowEpochMillis();

  if (session->sessionEndMillis == 0 || session->clientId.isEmpty()) {
    if (ws) ws->sendMessage("{\"type\":\"error\",\"subtype\":\"no_session\",\"message\":\"No active session to pause\"}");
    return;
  }
  if (session->sessionEndMillis <= nowMillis) {
    if (ws) ws->sendMessage("{\"type\":\"error\",\"message\":\"No time remaining\"}");
    return;
  }

  unsigned long remaining = (unsigned long)(session->sessionEndMillis - nowMillis);
  ESP_LOGI(TAG, "Pausing client %s with %lu ms remaining", session->clientMac.c_str(), remaining);

  if (!savePausedSessionFile(session->clientMac, remaining)) {
    if (ws) ws->sendMessage("{\"type\":\"error\",\"message\":\"Pause failed\",\"detail\":\"Could not save to filesystem\"}");
    return;
  }

  String errorDetail;
  if (disconnectOmadaClient(*session, errorDetail)) {
    session->pausedRemainingMillis = remaining;
    if (ws) { ws->sendMessage(buildStatusJson(*session, "paused").c_str()); ws->close(); }
  } else {
    deletePausedSessionFile(session->clientMac);
    if (ws) { ws->sendMessage(("{\"type\":\"error\",\"message\":\"Pause failed\",\"detail\":\"" + errorDetail + "\"}").c_str()); ws->close(); }
  }
}

void processResume(const String& mac) {
  SessionParams* session = HOTSPOT_SESSION_CACHE.findByMac(mac);
  if (!session) { ESP_LOGW(TAG, "processResume: no session for MAC=%s", mac.c_str()); return; }

  PsychicWebSocketClient* ws = WEBSOCKET_HANDLER.getClient(session->socketFd);  // may be null if WS disconnected
  unsigned long pausedRemainingMillis = session->pausedRemainingMillis;

  if (pausedRemainingMillis == 0) {
    if (ws) ws->sendMessage("{\"type\":\"error\",\"message\":\"No paused session to resume\"}");
    return;
  }

  ESP_LOGI(TAG, "Resuming client %s for %d minutes (%lu millis saved)",
           session->clientMac.c_str(), (int)(pausedRemainingMillis / MILLIS_PER_MINUTE), pausedRemainingMillis);

  String errorDetail;
  if (authenticateOmadaClient(*session, (unsigned long long)pausedRemainingMillis, errorDetail)) {
    session->pausedRemainingMillis = 0;
    deletePausedSessionFile(session->clientMac);
    if (ws) { ws->sendMessage(buildStatusJson(*session, "resumed").c_str()); ws->close(); }
    refreshHotspotSessionCache();  // get updated clientId from Omada
  } else {
    if (ws) { ws->sendMessage(("{\"type\":\"error\",\"message\":\"Resume failed\",\"detail\":\"" + errorDetail + "\"}").c_str()); ws->close(); }
  }
}
