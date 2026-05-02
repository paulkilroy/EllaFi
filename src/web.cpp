#include "web.h"

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_log.h>
#include "logger.h"

static const char* TAG = "EllaFi";

// ── HTTP / WS handlers ────────────────────────────────────────────────────────

// Serve captive portal page. Omada redirects here with query params:
// ?clientMac=...&apMac=...&ssidName=...&radioId=...&site=...&redirectUrl=...
esp_err_t handleRoot(PsychicRequest* request, PsychicResponse* response) {
  ESP_LOGI(TAG, "======handleRoot request");

#ifdef TEST_MODE
  String clientIp = request->hasParam("clientIp") ? request->getParam("clientIp")->value() : request->client()->remoteIP().toString();
#else
  String clientIp = request->client()->remoteIP().toString();
#endif
  String clientMac = request->hasParam("clientMac") ? request->getParam("clientMac")->value() : "";

  // Guard: if clientMac is missing, look up by IP — do NOT upsert yet (would create a phantom entry)
  if (clientMac.isEmpty()) {
    SessionParams* existing = HOTSPOT_SESSION_CACHE.findByIp(clientIp);
    if (!existing || existing->clientMac.isEmpty()) {
      ESP_LOGW(TAG, "No MAC for IP %s — rejecting portal request", clientIp.c_str());
      PsychicFileResponse errorResponse(response, LittleFS, "/unidentified.html");
      return errorResponse.send();
    }
    clientMac = existing->clientMac;
    ESP_LOGI(TAG, "Restored session for IP %s -> MAC %s", clientIp.c_str(), clientMac.c_str());
  }

  SessionParams& session = HOTSPOT_SESSION_CACHE.upsert(clientIp, clientMac);
  if (request->hasParam("apMac"))    session.apMac    = request->getParam("apMac")->value();
  if (request->hasParam("ssidName")) session.ssidName = request->getParam("ssidName")->value();
  if (request->hasParam("radioId"))  session.radioId  = request->getParam("radioId")->value();
  ESP_LOGI(TAG, "Stored session for IP %s -> MAC %s", clientIp.c_str(), clientMac.c_str());

  // Load paused session from disk if needed (outside session mutex — only LittleFS I/O)
  if (hasPausedSessionFile(clientMac) && session.pausedRemainingMillis == 0)
    session.pausedRemainingMillis = loadPausedSessionFile(clientMac);

  ESP_LOGI(TAG, "Client MAC: %s, IP: %s", clientMac.c_str(), clientIp.c_str());

  // Force new connection so browser doesn't reuse keep-alive for WS upgrade
  response->addHeader("Connection", "close");

  PsychicFileResponse fileResponse(response, LittleFS, "/index.html");
  return fileResponse.send();
}

esp_err_t handleNotFound(PsychicRequest* request, PsychicResponse* response) {
  ESP_LOGI(TAG, "404 Not Found: %s", request->path().c_str());

  String message = "<!DOCTYPE html><html><head><title>404 Not Found</title></head><body>";
  message += "<h1>404 - Not Found</h1>";
  message += "<p>The requested resource was not found on this server.</p>";
  message += "<p>Path: " + request->path() + "</p>";
  message += "</body></html>";

  return response->send(404, "text/html", message.c_str());
}

esp_err_t handleProgram(PsychicRequest* request, PsychicResponse* response) {
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

  // Keep COIN_SOCKET_FD fresh so finalizeCoinInsert can reach a reconnecting client
  if (clientIp == COIN_CLIENT_IP && COIN_SOCKET_FD != socketFd) {
    ESP_LOGI(TAG, "Updating COIN_SOCKET_FD from %d to %d (client reconnected)", COIN_SOCKET_FD, socketFd);
    COIN_SOCKET_FD = socketFd;
  }
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
    if (clientIp == COIN_CLIENT_IP && socketFd != COIN_SOCKET_FD) {
      ESP_LOGI(TAG, "Updating COIN_SOCKET_FD from %d to %d (client reconnected)", COIN_SOCKET_FD, socketFd);
      COIN_SOCKET_FD = socketFd;
    }
    request->client()->sendMessage(buildStatusJson(*session).c_str());

  } else if (action == "startCoinInsert") {
    bool coinInsertStarted = false;
    xSemaphoreTake(COIN_STATE_MUTEX, portMAX_DELAY);
    if (!COIN_INSERT_ACTIVE) {
      COIN_INSERT_ACTIVE = true;
      COIN_SOCKET_FD = socketFd;
      COIN_CLIENT_IP = clientIp;
      COIN_COUNT = 0;
      COIN_SLOT_READY_MILLIS = millis() + COINSLOT_BOOT_SUPPRESS_MILLIS;
      restartCoinInsertTimer();
      xTaskNotify(LED_TASK, LED_NOTIFY_START, eSetValueWithOverwrite);
      coinInsertStarted = true;
    }
    xSemaphoreGive(COIN_STATE_MUTEX);

    if (coinInsertStarted) {
      ESP_LOGI(TAG, "Coin insertion started for MAC: %s via WS", session->clientMac.c_str());
      masterBroadcastSessionStart();
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
    ESP_LOGI(TAG, "Test coin inserted for MAC: %s (count: %d)", session->clientMac.c_str(), COIN_COUNT);
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
  bool myCoinInsertActive = COIN_INSERT_ACTIVE && session.clientIp == COIN_CLIENT_IP;
  int coinInsertTimeLeft = 0;
  if (myCoinInsertActive) {
    TickType_t expiry = xTimerGetExpiryTime(COIN_INSERT_TIMER);
    TickType_t now = xTaskGetTickCount();
    if (expiry > now) coinInsertTimeLeft = ((expiry - now) * portTICK_PERIOD_MS) / 1000;
  }

  String json = "{";
  json += "\"type\":\"" + String(type) + "\",";
  json += "\"coinCount\":" + String(myCoinInsertActive ? COIN_COUNT : 0) + ",";
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
  if (COIN_SOCKET_FD < 0 || COIN_CLIENT_IP.length() == 0) return;

  SessionParams* session = HOTSPOT_SESSION_CACHE.findByIp(COIN_CLIENT_IP);
  if (!session) return;

  PsychicWebSocketClient* ws = wsHandler.getClient(COIN_SOCKET_FD);
  if (ws != nullptr) ws->sendMessage(buildStatusJson(*session).c_str());
}

// ── FreeRTOS tasks ────────────────────────────────────────────────────────────

// Pushes WS status updates to the inserting client.
// Woken immediately by coinPulseTask on each coin (for instant coin count feedback),
// or fires on the 500ms timeout to keep the countdown timer in the UI updated.
// No-op when no session is active.
void updateClientTask(void*) {
  for (;;) {
    xTaskNotifyWait(0, ULONG_MAX, NULL, pdMS_TO_TICKS(UPDATE_CLIENT_INTERVAL_MILLIS));
    if (COIN_INSERT_ACTIVE) pushCoinUpdateToClient();
  }
}

// WebSocketTask: drains PAUSE/RESUME/REFRESH_CACHE jobs posted by WS handlers.
// Owns its own operator + admin sessions — no shared mutex needed.
// Pinned to Core 0 — runs alongside PsychicHTTP tasks that enqueue the jobs.
void webSocketTask(void*) {
  static OmadaSession opSession;
  static OmadaSession adminSession;
  WsJob job;
  for (;;) {
    if (xQueueReceive(WEBSOCKET_JOB_QUEUE, &job, portMAX_DELAY) == pdTRUE) {
      if (job.type == JOB_PAUSE)         processPause(String(job.mac), opSession);
      if (job.type == JOB_RESUME)        processResume(String(job.mac), opSession, adminSession);
      if (job.type == JOB_REFRESH_CACHE) refreshHotspotSessionCache(opSession, adminSession);
    }
  }
}

void processPause(const String& mac, OmadaSession& op) {
  SessionParams* session = HOTSPOT_SESSION_CACHE.findByMac(mac);
  if (!session) { ESP_LOGW(TAG, "processPause: no session for MAC=%s", mac.c_str()); return; }

  PsychicWebSocketClient* ws = wsHandler.getClient(session->socketFd);  // may be null if WS disconnected
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
  if (disconnectOmadaClient(op, *session, errorDetail)) {
    session->pausedRemainingMillis = remaining;
    if (ws) ws->sendMessage(buildStatusJson(*session, "paused").c_str());
  } else {
    deletePausedSessionFile(session->clientMac);
    if (ws) ws->sendMessage(("{\"type\":\"error\",\"message\":\"Pause failed\",\"detail\":\"" + errorDetail + "\"}").c_str());
  }
}

void processResume(const String& mac, OmadaSession& op, OmadaSession& admin) {
  SessionParams* session = HOTSPOT_SESSION_CACHE.findByMac(mac);
  if (!session) { ESP_LOGW(TAG, "processResume: no session for MAC=%s", mac.c_str()); return; }

  PsychicWebSocketClient* ws = wsHandler.getClient(session->socketFd);  // may be null if WS disconnected
  unsigned long pausedRemainingMillis = session->pausedRemainingMillis;

  if (pausedRemainingMillis == 0) {
    if (ws) ws->sendMessage("{\"type\":\"error\",\"message\":\"No paused session to resume\"}");
    return;
  }

  ESP_LOGI(TAG, "Resuming client %s for %d minutes (%lu millis saved)",
           session->clientMac.c_str(), (int)(pausedRemainingMillis / MILLIS_PER_MINUTE), pausedRemainingMillis);

  String errorDetail;
  if (authenticateOmadaClient(op, *session, (unsigned long long)pausedRemainingMillis, errorDetail)) {
    session->pausedRemainingMillis = 0;
    deletePausedSessionFile(session->clientMac);
    if (ws) ws->sendMessage(buildStatusJson(*session, "resumed").c_str());
    refreshHotspotSessionCache(op, admin);  // get updated clientId from Omada
  } else {
    if (ws) ws->sendMessage(("{\"type\":\"error\",\"message\":\"Resume failed\",\"detail\":\"" + errorDetail + "\"}").c_str());
  }
}
