/**
 * EllaFi - ESP32 Coin-Operated WiFi Hotspot Portal
 *
 * Captive portal for coin-operated WiFi access via TP-Link Omada Controller (EAP auth, v5.11+).
 * Uses WebSockets for real-time state push. Session data keyed by client IP in HOTSPOT_SESSION_CACHE.
 *
 * Endpoints: GET / (portal page), WS /ws (real-time comms)
 *
 * WS Protocol:
 *   Client: {"action":"getStatus"}, {"action":"startCoinInsert"}, {"action":"pause"}, {"action":"resume"}, {"action":"testInsertCoin"} (TEST_MODE only)
 *   Server: {"type":"status", ...}, {"type":"authenticated"}, {"type":"error", ...}
 *
 * Flow: Omada redirects client -> handleRoot stores session -> WS pushes status ->
 *       coins trigger ISR -> 10s timeout -> finalizeCoinInsert authenticates with Omada
 *
 */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO

#include "omada.h"
#include "network.h"
#include <WiFi.h>
#include <PsychicHttp.h>
#include <PsychicWebSocket.h>
#include <PsychicFileResponse.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Ticker.h>
#include <ESPmDNS.h>
#ifdef HAS_RGB_LED
#include <Adafruit_NeoPixel.h>
#endif
#include <map>
#include <queue>
#include <time.h>

#include <esp_log.h>

//#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
static const char* TAG = "EllaFi";
static const char* ERR_SUBTYPE_NO_SESSION  = "no_session";
static const char* ERR_SUBTYPE_REFUNDABLE  = "refundable";

// WiFi — loaded from /config.json at startup
String AP_SSID;
String AP_PASSWORD;


// Omada Controller globals are declared in omada.h and defined in omada.cpp.
// Populated by loadConfig() below.

const int COIN_PIN = 0;  // GPIO0 - BOOT button for testing

#ifdef HAS_RGB_LED
#define LED_PIN   48  // Built-in WS2812 on ESP32-S3 DevKitC-1
#define LED_COUNT  1
Adafruit_NeoPixel LED_STRIP(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
#endif

PsychicHttpServer server;
PsychicWebSocketHandler wsHandler;

// SessionParams and SessionCache are defined in omada.h.

void restartCoinInsertTimer();
void IRAM_ATTR coinPulse();
esp_err_t handleRoot(PsychicRequest *request, PsychicResponse *response);
esp_err_t handleNotFound(PsychicRequest *request, PsychicResponse *response);
void finalizeCoinInsert();
void handleWsOpen(PsychicWebSocketClient* client);
esp_err_t handleWsRequest(PsychicWebSocketRequest* request, httpd_ws_frame* frame);
void handleWsClose(PsychicWebSocketClient* client);
void pushCoinUpdateToClient();
String macToFilename(const String& mac);
bool savePausedSessionFile(const String& mac, unsigned long remainingMillis);
unsigned long loadPausedSessionFile(const String& mac);
bool hasPausedSessionFile(const String& mac);
void deletePausedSessionFile(const String& mac);

// Single source of truth for client sessions, keyed by IP (secondary index by MAC)
SessionCache HOTSPOT_SESSION_CACHE;

typedef void (*ActionHandlerFn)(const String&);

void processPause(const String&);
void processResume(const String&);

struct ActionRequest {
  ActionHandlerFn handler;
  String mac;
};

SemaphoreHandle_t ACTION_QUEUE_MUTEX = NULL;
std::queue<ActionRequest> ACTION_QUEUE;

// Cached role — set in setup() after networkSetup(); safe to read from ISR
bool IS_MASTER = false;

// Coin session state
volatile int COIN_COUNT = 0;
volatile unsigned long COIN_INSERT_START_TIME = 0;
volatile bool COIN_INSERT_ACTIVE = false;
volatile bool COIN_INSERT_NEEDS_FINALIZATION = false;
volatile bool COIN_COUNT_CHANGED = false;
volatile bool COIN_PULSE_PENDING = false;  // set by ISR, drained in loop()
const unsigned long COIN_INSERT_TIMEOUT = 10000;
const int MINUTES_PER_COIN = 5;
Ticker COIN_INSERT_TIMER;
SemaphoreHandle_t COIN_STATE_MUTEX = NULL;

volatile unsigned long LAST_COIN_PULSE_TIME = 0;
const unsigned long COIN_DEBOUNCE_MILLIS = 300;

// Which client is currently inserting coins
int COIN_SOCKET_FD = -1;
String COIN_CLIENT_IP = "";

void coinTimerCallback() {
  COIN_INSERT_TIMER.detach();
  COIN_INSERT_NEEDS_FINALIZATION = true;
}

void restartCoinInsertTimer() {
  COIN_INSERT_TIMER.once(COIN_INSERT_TIMEOUT / 1000.0, coinTimerCallback);
  COIN_INSERT_START_TIME = millis();
}

void IRAM_ATTR coinPulse() {
  unsigned long now = millis();
  if (now - LAST_COIN_PULSE_TIME < COIN_DEBOUNCE_MILLIS) return;
  LAST_COIN_PULSE_TIME = now;
  COIN_PULSE_PENDING = true;
}

// Called on master when a slave reports a coin via UDP.
void remoteCoinPulse() {
  COIN_PULSE_PENDING = true;
}

String macToFilename(const String& mac) {
  String safe = mac;
  safe.replace(":", "");
  return "/pause_" + safe + ".dat";
}

bool savePausedSessionFile(const String& mac, unsigned long remainingMillis) {
  String path = macToFilename(mac);
  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) {
    ESP_LOGE(TAG, "Failed to save paused session to %s", path.c_str());
    return false;
  }
  f.write((uint8_t*)&remainingMillis, sizeof(remainingMillis));
  f.close();
  ESP_LOGI(TAG, "Saved paused session for %s: %lu ms", mac.c_str(), remainingMillis);
  return true;
}

unsigned long loadPausedSessionFile(const String& mac) {
  String path = macToFilename(mac);
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return 0;
  unsigned long remainingMillis = 0;
  f.read((uint8_t*)&remainingMillis, sizeof(remainingMillis));
  f.close();
  ESP_LOGI(TAG, "Loaded paused session for %s: %lu ms", mac.c_str(), remainingMillis);
  return remainingMillis;
}

bool hasPausedSessionFile(const String& mac) {
  return LittleFS.exists(macToFilename(mac));
}

void deletePausedSessionFile(const String& mac) {
  String path = macToFilename(mac);
  if (LittleFS.exists(path)) {
    LittleFS.remove(path);
    ESP_LOGI(TAG, "Deleted paused session file: %s", path.c_str());
  }
}

bool loadConfig() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) {
    ESP_LOGE(TAG, "config.json not found");
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    ESP_LOGE(TAG, "Failed to parse config.json: %s", err.c_str());
    return false;
  }
  AP_SSID               = doc["wifi_ssid"]               | "";
  AP_PASSWORD           = doc["wifi_password"]            | "";
  CONTROLLER_BASE_URL   = doc["omada_url"]                | "";
  CONTROLLER_ID         = doc["omada_controller_id"]      | "";
  OPERATOR_USERNAME     = doc["omada_operator_username"]  | "";
  OPERATOR_PASSWORD     = doc["omada_operator_password"]  | "";
  ADMIN_USERNAME        = doc["omada_username"]           | "";
  ADMIN_PASSWORD        = doc["omada_password"]           | "";
  MASTER_MAC            = doc["master_mac"]               | "";
  NET_PSK               = doc["network_key"]              | 0ULL;
  if (AP_SSID.isEmpty())             { ESP_LOGE(TAG, "config.json missing: wifi_ssid");              return false; }
  if (AP_PASSWORD.isEmpty())         { ESP_LOGE(TAG, "config.json missing: wifi_password");           return false; }
  if (CONTROLLER_BASE_URL.isEmpty()) { ESP_LOGE(TAG, "config.json missing: omada_url");               return false; }
  if (CONTROLLER_ID.isEmpty())       { ESP_LOGE(TAG, "config.json missing: omada_controller_id");     return false; }
  if (OPERATOR_USERNAME.isEmpty())   { ESP_LOGE(TAG, "config.json missing: omada_operator_username"); return false; }
  if (OPERATOR_PASSWORD.isEmpty())   { ESP_LOGE(TAG, "config.json missing: omada_operator_password"); return false; }
  if (ADMIN_USERNAME.isEmpty())      { ESP_LOGE(TAG, "config.json missing: omada_username");          return false; }
  if (ADMIN_PASSWORD.isEmpty())      { ESP_LOGE(TAG, "config.json missing: omada_password");          return false; }
  if (NET_PSK == 0)                  { ESP_LOGE(TAG, "config.json missing: network_key");             return false; }
  ESP_LOGI(TAG, "Config loaded: ssid=%s url=%s siteId=%s", AP_SSID.c_str(), CONTROLLER_BASE_URL.c_str(), SITE_ID.c_str());
  return true;
}

// ── Network event handlers ────────────────────────────────────────────────────

void slaveCoinInsertStart() {
  COIN_INSERT_ACTIVE = true;
  ESP_LOGI(TAG, "Session started — accepting coins");
}

void slaveCoinInsertEnd() {
  COIN_INSERT_ACTIVE = false;
  ESP_LOGI(TAG, "Session ended — coin acceptance disabled");
}

// ─────────────────────────────────────────────────────────────────────────────

void setup() {
  delay(1000);
  Serial.begin(115200);
  esp_log_level_set("*", ESP_LOG_ERROR);
  esp_log_level_set("EllaFi", ESP_LOG_INFO);

  delay(2000);

  if (!LittleFS.begin(true)) {
    ESP_LOGE(TAG, "LittleFS Mount Failed");
    halt();
  }
  ESP_LOGI(TAG, "LittleFS Mounted Successfully");
  if (!loadConfig()) {
    ESP_LOGE(TAG, "Failed to load config — halting");
    halt();
  }

#ifdef HAS_RGB_LED
  LED_STRIP.begin();
  LED_STRIP.setBrightness(80);
  LED_STRIP.clear();
  LED_STRIP.show();
#endif

  pinMode(COIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COIN_PIN), coinPulse, FALLING);

  WiFi.mode(WIFI_STA);
  WiFi.begin(AP_SSID.c_str(), AP_PASSWORD.c_str());

  ESP_LOGI(TAG, "Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    if(WiFi.status() == WL_NO_SSID_AVAIL) {
      ESP_LOGE(TAG, "WiFi network not found: %s", AP_SSID.c_str());
      delay(5000);
    } else if(WiFi.status() == WL_CONNECT_FAILED) {
      ESP_LOGE(TAG, "WiFi connection failed with provided credentials");
      delay(5000);
    } else {
      delay(500);
      ESP_LOGI(TAG, ".");
    }
  }
  ESP_LOGI(TAG, "WiFi Connected");
  ESP_LOGI(TAG, "IP Address: %s", WiFi.localIP().toString().c_str());

  networkSetup();
  IS_MASTER = isMaster();
  ESP_LOGI(TAG, "Role: %s (MAC: %s)", IS_MASTER ? "MASTER" : "SLAVE", WiFi.macAddress().c_str());

  // Sync wall clock via NTP (required for comparing against Omada epoch timestamps)
  configTime(0, 0, "pool.ntp.org");
  ESP_LOGI(TAG, "Waiting for NTP time sync...");
  {
    time_t now = 0;
    int retries = 0;
    while (now < 1000000000L && retries++ < 20) { delay(500); time(&now); }
    if (now < 1000000000L) {
      ESP_LOGW(TAG, "NTP sync timed out — epoch timestamps may be incorrect");
    } else {
      ESP_LOGI(TAG, "NTP sync OK: epoch=%lld", (long long)now);
    }
  }

  omadaSetup();

  if (MDNS.begin("ellafi")) {
    ESP_LOGI(TAG, "mDNS started: ellafi.local");
  } else {
    ESP_LOGW(TAG, "mDNS failed to start");
  }

  server.config.stack_size = 16384; // Larger stack for SSL calls in WS handlers

  server.on("/", HTTP_GET, handleRoot);

  wsHandler.onOpen(handleWsOpen);
  wsHandler.onFrame(handleWsRequest);
  wsHandler.onClose(handleWsClose);
  server.on("/ws", &wsHandler);

  server.on("/config.json", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(403, "text/plain", "Forbidden");
  });
  server.serveStatic("/", LittleFS, "/", "max-age=86400");
  server.onNotFound(handleNotFound); // Must be set AFTER serveStatic

  server.begin();
  ESP_LOGI(TAG, "Server started");

  COIN_STATE_MUTEX = xSemaphoreCreateMutex();
  ACTION_QUEUE_MUTEX = xSemaphoreCreateMutex();

  if (COIN_STATE_MUTEX == NULL || ACTION_QUEUE_MUTEX == NULL) {
    ESP_LOGE(TAG, "Failed to create synchronization primitives");
  }

}

void loop() {
  if (COIN_INSERT_NEEDS_FINALIZATION) {
    COIN_INSERT_NEEDS_FINALIZATION = false;
    finalizeCoinInsert();
  }

  // Push on coin count change or every 500ms during active session
  static unsigned long lastCoinPush = 0;
  if (COIN_COUNT_CHANGED || (COIN_INSERT_ACTIVE && millis() - lastCoinPush > 500)) {
    COIN_COUNT_CHANGED = false;
    lastCoinPush = millis();
    pushCoinUpdateToClient();
  }

#ifdef HAS_RGB_LED
  // Cycle LED R→G→B while coin slot is active, clear when session ends
  static unsigned long lastLedUpdate = 0;
  uint32_t ledColor = LED_STRIP.getPixelColor(0);
  if (COIN_INSERT_ACTIVE && millis() - lastLedUpdate > 200) {
    lastLedUpdate = millis();
    LED_STRIP.setPixelColor(0, (ledColor >> 8) ? (ledColor >> 8) : 0xFF0000);
    LED_STRIP.show();
  } else if (!COIN_INSERT_ACTIVE && ledColor) {
    LED_STRIP.clear();
    LED_STRIP.show();
  }
#endif

  // Drain queued actions (pause/resume) from WS handler threads — before omadaLoop() so a
  // background cache refresh (3-4s of HTTPS) doesn't delay user-initiated actions
  xSemaphoreTake(ACTION_QUEUE_MUTEX, portMAX_DELAY);
  while (!ACTION_QUEUE.empty()) {
    ActionRequest req = std::move(ACTION_QUEUE.front());
    ACTION_QUEUE.pop();
    xSemaphoreGive(ACTION_QUEUE_MUTEX);
    req.handler(req.mac);
    xSemaphoreTake(ACTION_QUEUE_MUTEX, portMAX_DELAY);
  }
  xSemaphoreGive(ACTION_QUEUE_MUTEX);

  omadaLoop();

  // Scheduled daily reboot at 3:00 AM UTC — simplest way to keep memory and cache clean
  static unsigned long lastRebootCheck = 0;
  if (millis() - lastRebootCheck > 60000) {
    lastRebootCheck = millis();
    time_t now = time(NULL);
    struct tm* t = gmtime(&now);
    if (t->tm_hour == 3 && t->tm_min == 0) {
      ESP_LOGI(TAG, "Scheduled 3am reboot");
      esp_restart();
    }
  }

  networkLoop();

  // Drain coin pulse: master counts locally, slave forwards to master via UDP
  if (COIN_PULSE_PENDING) {
    COIN_PULSE_PENDING = false;
    if (IS_MASTER && COIN_INSERT_ACTIVE) {
      COIN_COUNT++;
      COIN_COUNT_CHANGED = true;
      restartCoinInsertTimer();
      ESP_LOGI(TAG, "Coin registered (total: %d)", COIN_COUNT);
    } else if (!IS_MASTER) {
      netSendCoinInserted();
    }
  }
}

// Serve captive portal page. Omada redirects here with query params:
// ?clientMac=...&apMac=...&ssidName=...&radioId=...&site=...&redirectUrl=...
esp_err_t handleRoot(PsychicRequest *request, PsychicResponse *response) {
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

void handleWsClose(PsychicWebSocketClient* client) {
  ESP_LOGI(TAG, "WS client disconnected: socket #%d", client->socket());
}

String buildStatusJson(const SessionParams& session) {
  bool myCoinInsertActive = COIN_INSERT_ACTIVE && session.clientIp == COIN_CLIENT_IP;
  int coinInsertTimeLeft = 0;
  if (myCoinInsertActive) {
    unsigned long elapsed = millis() - COIN_INSERT_START_TIME;
    if (elapsed < COIN_INSERT_TIMEOUT) coinInsertTimeLeft = (COIN_INSERT_TIMEOUT - elapsed) / 1000;
  }

  String json = "{";
  json += "\"type\":\"status\",";
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
      restartCoinInsertTimer();
      coinInsertStarted = true;
    }
    xSemaphoreGive(COIN_STATE_MUTEX);

    if (coinInsertStarted) {
      ESP_LOGI(TAG, "Coin insertion started for MAC: %s via WS", session->clientMac.c_str());
      netBroadcastSessionStart();
      request->client()->sendMessage(buildStatusJson(*session).c_str());
    } else {
      request->reply("{\"type\":\"error\",\"message\":\"Another user already inserting coins\"}");
    }

  } else if (action == "pause") {
    xSemaphoreTake(ACTION_QUEUE_MUTEX, portMAX_DELAY);
    ACTION_QUEUE.push({processPause, session->clientMac});
    xSemaphoreGive(ACTION_QUEUE_MUTEX);

  } else if (action == "resume") {
    xSemaphoreTake(ACTION_QUEUE_MUTEX, portMAX_DELAY);
    ACTION_QUEUE.push({processResume, session->clientMac});
    xSemaphoreGive(ACTION_QUEUE_MUTEX);

#ifdef TEST_MODE
  } else if (action == "testInsertCoin") {
    // Simulate a coin pulse (assumes startCoinInsert was called first)
    coinPulse();
    ESP_LOGI(TAG, "Test coin inserted for MAC: %s (count: %d)", session->clientMac.c_str(), COIN_COUNT);
#endif
  } else {
    request->reply("{\"type\":\"error\",\"message\":\"Unknown action\"}");
  }

  return ESP_OK;
}

void processPause(const String& mac) {
  SessionParams* params = HOTSPOT_SESSION_CACHE.findByMac(mac);
  if (!params) { ESP_LOGW(TAG, "processPause: no session for MAC=%s", mac.c_str()); return; }

  PsychicWebSocketClient* ws = wsHandler.getClient(params->socketFd);  // may be null if WS disconnected
  uint64_t nowMillis = nowEpochMillis();

  if (params->sessionEndMillis == 0 || params->clientId.isEmpty()) {
    if (ws) ws->sendMessage("{\"type\":\"error\",\"subtype\":\"no_session\",\"message\":\"No active session to pause\"}");
    return;
  }
  if (params->sessionEndMillis <= nowMillis) {
    if (ws) ws->sendMessage("{\"type\":\"error\",\"message\":\"No time remaining\"}");
    return;
  }

  unsigned long remaining = (unsigned long)(params->sessionEndMillis - nowMillis);
  ESP_LOGI(TAG, "Pausing client %s with %lu ms remaining", params->clientMac.c_str(), remaining);

  if (!savePausedSessionFile(params->clientMac, remaining)) {
    if (ws) ws->sendMessage("{\"type\":\"error\",\"message\":\"Pause failed\",\"detail\":\"Could not save to filesystem\"}");
    return;
  }

  String errorDetail;
  if (disconnectOmadaClient(*params, errorDetail)) {
    params->pausedRemainingMillis = remaining;
    if (ws) ws->sendMessage(buildStatusJson(*params).c_str());
  } else {
    deletePausedSessionFile(params->clientMac);
    if (ws) ws->sendMessage(("{\"type\":\"error\",\"message\":\"Pause failed\",\"detail\":\"" + errorDetail + "\"}").c_str());
  }
}

void processResume(const String& mac) {
  SessionParams* params = HOTSPOT_SESSION_CACHE.findByMac(mac);
  if (!params) { ESP_LOGW(TAG, "processResume: no session for MAC=%s", mac.c_str()); return; }

  PsychicWebSocketClient* ws = wsHandler.getClient(params->socketFd);  // may be null if WS disconnected
  unsigned long pausedRemainingMillis = params->pausedRemainingMillis;

  if (pausedRemainingMillis == 0) {
    if (ws) ws->sendMessage("{\"type\":\"error\",\"message\":\"No paused session to resume\"}");
    return;
  }

  ESP_LOGI(TAG, "Resuming client %s for %d minutes (%lu millis saved)",
           params->clientMac.c_str(), (int)(pausedRemainingMillis / 60000), pausedRemainingMillis);

  String errorDetail;
  if (authenticateOmadaClient(*params, (unsigned long long)pausedRemainingMillis, errorDetail)) {
    params->pausedRemainingMillis = 0;
    deletePausedSessionFile(params->clientMac);
    refreshHotspotSessionCache();  // get updated clientId from Omada
    if (ws) ws->sendMessage(buildStatusJson(*params).c_str());
  } else {
    if (ws) ws->sendMessage(("{\"type\":\"error\",\"message\":\"Resume failed\",\"detail\":\"" + errorDetail + "\"}").c_str());
  }
}

// Called 10s after last coin insertion. Authenticates with Omada and pushes result via WS.
void finalizeCoinInsert() {
  ESP_LOGI(TAG, "======finalizeCoinInsert called");

  // Snapshot volatile coin state before ISR can change it
  int coinCount = COIN_COUNT;
  int savedSocketFd = COIN_SOCKET_FD;
  String errorMsg;
  String errorDetail;
  String errorSubtype;
  bool success = false;

  SessionParams* session = HOTSPOT_SESSION_CACHE.findByIp(COIN_CLIENT_IP);

  if (coinCount > 0 && session) {
    unsigned long long additionalMillis = coinCount * MINUTES_PER_COIN * 60000ULL;
    uint64_t nowMillis = nowEpochMillis();

    if (session->sessionEndMillis > nowMillis && !session->clientId.isEmpty()) {
      // Active session — extend by new coins only
      ESP_LOGI(TAG, "Extending %s: +%d min (%d remaining)",
               session->clientMac.c_str(),
               coinCount * MINUTES_PER_COIN,
               (int)((session->sessionEndMillis - nowMillis) / 60000));

      if (extendOmadaClient(*session, additionalMillis, errorDetail)) {
        success = true;
      } else {
        ESP_LOGE(TAG, "Extend failed: %s", errorDetail.c_str());
        errorMsg = "Failed to extend session";
        errorSubtype = ERR_SUBTYPE_REFUNDABLE;
      }
    } else {
      // No active session — fresh auth
      ESP_LOGI(TAG, "Authenticating %s for %d minutes",
               session->clientMac.c_str(), coinCount * MINUTES_PER_COIN);

      if (authenticateOmadaClient(*session, additionalMillis, errorDetail)) {
        success = true;
      } else {
        ESP_LOGE(TAG, "Auth failed: %s", errorDetail.c_str());
        errorMsg = "Failed to connect to controller";
        errorSubtype = ERR_SUBTYPE_REFUNDABLE;
      }
    }

    if (success) refreshHotspotSessionCache();  // sync accurate sessionEndMillis + clientId
  } else if (coinCount == 0) {
    errorMsg = "No coins inserted";
  } else {
    errorMsg = "No portal session found for this device";
    errorSubtype = ERR_SUBTYPE_NO_SESSION;
  }

  PsychicWebSocketClient* ws = (savedSocketFd >= 0) ? wsHandler.getClient(savedSocketFd) : nullptr;

  xSemaphoreTake(COIN_STATE_MUTEX, portMAX_DELAY);
  COIN_INSERT_ACTIVE = false;
  COIN_COUNT = 0;
  COIN_INSERT_START_TIME = 0;
  COIN_SOCKET_FD = -1;
  COIN_CLIENT_IP = "";
  xSemaphoreGive(COIN_STATE_MUTEX);
  netBroadcastSessionEnd();

  if (ws) {
    if (success) {
      ws->sendMessage("{\"type\":\"authenticated\"}");
    } else {
      String errJson = "{\"type\":\"error\"";
      if (errorSubtype.length() > 0) errJson += ",\"subtype\":\"" + errorSubtype + "\"";
      errJson += ",\"message\":\"" + errorMsg + "\"";
      if (errorDetail.length() > 0) errJson += ",\"detail\":\"" + errorDetail + "\"";
      errJson += "}";
      ws->sendMessage(errJson.c_str());
    }
  } else {
    ESP_LOGW(TAG, "Auth result not delivered: WS socket #%d already disconnected", savedSocketFd);
  }
}

esp_err_t handleNotFound(PsychicRequest *request, PsychicResponse *response) {
  ESP_LOGI(TAG, "404 Not Found: %s", request->path().c_str());

  String message = "<!DOCTYPE html><html><head><title>404 Not Found</title></head><body>";
  message += "<h1>404 - Not Found</h1>";
  message += "<p>The requested resource was not found on this server.</p>";
  message += "<p>Path: " + request->path() + "</p>";
  message += "</body></html>";

  return response->send(404, "text/html", message.c_str());
}

