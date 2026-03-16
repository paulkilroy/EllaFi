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
 *   Server: {"type":"status", ...}, {"type":"paused", ...}, {"type":"resumed", ...}, {"type":"authenticated"}, {"type":"error", ...}
 *
 * Flow: Omada redirects client -> handleRoot stores session -> WS pushes status ->
 *       coins trigger ISR -> 10s timeout -> CoinTask authenticates with Omada
 *
 * ── Threading model ───────────────────────────────────────────────────────────
 *
 *  ESP32 has 2 cores (Xtensa LX6 dual-core):
 *
 *  Core 0 — Radio & System
 *  Espressif reserves Core 0 for WiFi/BT stack. Tasks here must not starve the radio.
 *    WiFi/lwIP stack          [Espressif system, always here]
 *    PsychicHTTP task(s)      [one per connection, handles HTTP + WS frames]
 *    FreeRTOS Timer Task      [runs FreeRTOS timer callbacks — coinTimerCallback]
 *    OmadaTask                [PAUSE / RESUME queue — HTTPS ~3-4s per job]
 *
 *  Core 1 — Application
 *  Arduino loop() runs here by default.
 *    loop()                   [lean — networkLoop() + daily reboot check only]
 *    CoinTask                 [FINALIZE_COINS — HTTPS, one job ever active]
 *                               - woken by COIN_FINALIZE_SEM when timer fires
 *                               - runs finalizeCoinInsert() under OMADA_API_MUTEX
 *                               - higher priority than OmadaTask
 *    CoinPulseTask            [drains COIN_PULSE_SEM counting semaphore]
 *                               - given by coinPulse() ISR or remoteCoinPulse()
 *                               - increments COIN_COUNT, resets timer, notifies peers
 *                               - highest priority (6) among app tasks
 *    updateClientTask         [pushes WS status on coin or every 500ms]
 *                               - woken by CoinPulseTask via task notification
 *                               - also wakes on 500ms timeout during active session
 *    LedTask                  [blinks LED during coin insert window]
 *                               - started by xTaskNotify from startCoinInsert / slaveCoinInsertStart
 *                               - stopped by xTaskNotify from finalizeCoinInsert / slaveCoinInsertEnd
 *
 *  ISR (no core, interrupt level)
 *    coinPulse()              [GPIO ISR — gives COIN_PULSE_SEM counting semaphore]
 *
 *  OMADA_API_MUTEX serializes all Omada HTTPS calls across CoinTask and OmadaTask,
 *  protecting the shared operator cookie jar (CONTROLLER_COOKIEJAR).
 *
 * Coin Flow:
 * Customer initiates coin insert - clicks "Insert Coin" on captive portal
 *  1. Client sends WS {"action":"startCoinInsert"} to the server and shows coin insert screen
 *  2. Server receives startCoinInsert and
 *      set COIN_COUNT = 0, COIN_INSERT_ACTIVE=true
 *      remembers the Socket and IP of the client
 *      starts COIN_INSERT_TIMER (10s) via restartCoinInsertTimer
 *      signals LedTask to begin blinking via task notification
 *
 *  Customer inserts coin
 *  1. coinPulse() ISR -> increments COIN_PULSE_SEM
 *      or remoteCoinPulse() -> increments COIN_PULSE_SEM from UDP
 *  2. coinPulseTask drains COIN_PULSE_SEM -> increments COIN_COUNT,
 *      resets COIN_INSERT_TIMER back to 10s
 *      notifies updateClientTask for immediate WS push
 *      Gated by SHOULD_ACCEPT_COINS???
 *  3.  updateClientTask pushes coin count + timing to client immediately (and every 500ms)
 *
 * Customer finishes inserting coins after 10s of inactivity (no coins inserted)
 *  1. UI transitions to "pending" screen while waiting for server confirmation
 *  2. coinTimerCallback fires when COIN_INSERT_TIMER expires -> gives COIN_FINALIZE_SEM
 *  3. finalizeCoinTask wakes on COIN_FINALIZE_SEM -> calls finalizeCoinInsert()
 *      sets COIN_INSERT_ACTIVE=false
 *      notifies LedTask to stop
 *      authenticates with Omada, and extends or starts a new session for the customer
 *      pushes "authenticated" result via WS
 */

#include "omada.h"
#include "network.h"
#include "led.h"
#include <WiFi.h>
#include <PsychicHttp.h>
#include <PsychicWebSocket.h>
#include <PsychicFileResponse.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <map>
#include <time.h>

#include <esp_log.h>

//#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
static const char* TAG = "EllaFi";
static const char* ERR_SUBTYPE_NO_SESSION  = "no_session";
static const char* ERR_SUBTYPE_REFUNDABLE  = "refundable";

// WiFi — loaded from /config.json at startup
String AP_SSID;
String AP_PASSWORD;

const int COIN_PIN = 0;  // GPIO0 - BOOT button for testing

PsychicHttpServer server;
PsychicWebSocketHandler wsHandler;

// ISR + timer
void IRAM_ATTR coinPulse();
void coinTimerCallback(TimerHandle_t xTimer);
void restartCoinInsertTimer();

// Config + network events
bool loadConfig();
void masterRecvCoinInserted();
void slaveRecvCoinInsertStart();
void slaveRecvCoinInsertEnd();

// FreeRTOS tasks
void coinPulseTask(void*);
void updateClientTask(void*);
void finalizeCoinTask(void*);
void omadaTask(void*);

// Web / WS handlers
esp_err_t handleRoot(PsychicRequest *request, PsychicResponse *response);
esp_err_t handleNotFound(PsychicRequest *request, PsychicResponse *response);
void handleWsOpen(PsychicWebSocketClient* client);
esp_err_t handleWsRequest(PsychicWebSocketRequest* request, httpd_ws_frame* frame);
void handleWsClose(PsychicWebSocketClient* client);
String buildStatusJson(const SessionParams& session, const char* type = "status");
void pushCoinUpdateToClient();
void finalizeCoinInsert();
void processPause(const String& mac);
void processResume(const String& mac);

// Paused session file helpers
String macToFilename(const String& mac);
bool savePausedSessionFile(const String& mac, unsigned long remainingMillis);
unsigned long loadPausedSessionFile(const String& mac);
bool hasPausedSessionFile(const String& mac);
void deletePausedSessionFile(const String& mac);

// Single source of truth for client sessions, keyed by IP (secondary index by MAC)
SessionCache HOTSPOT_SESSION_CACHE;

// OmadaTask job types
enum OmadaJobType { JOB_PAUSE, JOB_RESUME, JOB_REFRESH_CACHE };
struct OmadaJob {
  OmadaJobType type;
  char mac[18];  // fixed-size to avoid heap alloc in queue item
  OmadaJob() = default;
  OmadaJob(OmadaJobType t, const String& m) : type(t) {
    strncpy(mac, m.c_str(), sizeof(mac) - 1);
    mac[sizeof(mac) - 1] = '\0';
  }
};

QueueHandle_t     OMADA_JOB_QUEUE   = NULL;  // OmadaTask input: PAUSE / RESUME jobs
SemaphoreHandle_t COIN_FINALIZE_SEM = NULL;  // CoinTask doorbell: given by coinTimerCallback
SemaphoreHandle_t OMADA_API_MUTEX   = NULL;  // serializes all Omada HTTPS across both tasks
SemaphoreHandle_t COIN_PULSE_SEM    = NULL;  // counting semaphore — given by ISR/remoteCoinPulse, drained by coinPulseTask
TaskHandle_t      UPDATE_CLIENT_TASK = NULL;  // notified on each coin; also wakes every 500ms

// Cached role — set in setup() after networkSetup(); safe to read from ISR
bool IS_MASTER = false;

// Coin session state
const unsigned long COIN_INSERT_TIMEOUT = 10000;
const int           MINUTES_PER_COIN    = 5;
TimerHandle_t       COIN_INSERT_TIMER = NULL;
SemaphoreHandle_t   COIN_STATE_MUTEX    = NULL;

// ISR-only — written by coinPulse(); no mutex needed
volatile unsigned long LAST_COIN_PULSE_TIME = 0; // just used for debouncing
const unsigned long COIN_DEBOUNCE_MILLIS  = 300;

// Gated by COIN_INSERT_ACTIVE — valid only while a session is active or processing
volatile bool COIN_INSERT_ACTIVE = false;  // true from startCoinInsert until finalizeCoinInsert completes
int           COIN_SOCKET_FD     = -1;     // WS socket for the inserting client
String        COIN_CLIENT_IP     = "";     // IP of the inserting client

// Gated by COIN_INSERT_TIMER (master) or COIN_INSERT_ACTIVE (slave) — valid while the insertion window is open
volatile int           COIN_COUNT             = 0;
#define SHOULD_ACCEPT_COINS (IS_MASTER ? (xTimerIsTimerActive(COIN_INSERT_TIMER) == pdTRUE) : COIN_INSERT_ACTIVE)

// ── ISR + timer ───────────────────────────────────────────────────────────────

// callback from interrupt on PIN - cant do complex work -- debounce and give COIN_PULSE_SEM
void IRAM_ATTR coinPulse() {
  unsigned long now = millis();
  if (now - LAST_COIN_PULSE_TIME < COIN_DEBOUNCE_MILLIS) return;
  LAST_COIN_PULSE_TIME = now;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(COIN_PULSE_SEM, &woken);
  portYIELD_FROM_ISR(woken);
}

void coinTimerCallback(TimerHandle_t xTimer) {
  xSemaphoreGive(COIN_FINALIZE_SEM);
}

void restartCoinInsertTimer() {
  xTimerReset(COIN_INSERT_TIMER, 0);  // starts if inactive, restarts if active
}

// ── Config + network events ───────────────────────────────────────────────────

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

// Called on master when a slave reports a coin via UDP.
void masterRecvCoinInserted() {
  xSemaphoreGive(COIN_PULSE_SEM);
}

void slaveRecvCoinInsertStart() {
  COIN_INSERT_ACTIVE = true;
  xTaskNotify(LED_TASK, LED_NOTIFY_START, eSetValueWithOverwrite);
  ESP_LOGI(TAG, "Session started — accepting coins");
}

void slaveRecvCoinInsertEnd() {
  COIN_INSERT_ACTIVE = false;
  xTaskNotify(LED_TASK, LED_NOTIFY_STOP, eSetValueWithOverwrite);
  ESP_LOGI(TAG, "Session ended — coin acceptance disabled");
}

// ── FreeRTOS tasks ────────────────────────────────────────────────────────────

// Drains coin pulses from the ISR (via COIN_PULSE_SEM counting semaphore).
// Gated by SHOULD_ACCEPT_COINS — pulses that arrive outside the insertion window
// (e.g. after the timer has expired but before finalizeCoinInsert clears things) are discarded.
// Master: increments COIN_COUNT, resets the insertion timer, and broadcasts to slaves.
// Slave:  resets its own insertion timer and forwards the pulse to the master via UDP.
// Master: also notifies CLIENT_UPDATE_TASK for an immediate WS push (slaves have no web server).
void coinPulseTask(void*) {
  for (;;) {
    xSemaphoreTake(COIN_PULSE_SEM, portMAX_DELAY);
    // Interrupts are always sent to COIN_PULSE_SEM - this is the only gate
    ESP_LOGI(TAG, "coin pulse — %s", IS_MASTER ? "master" : "slave");
    if (!SHOULD_ACCEPT_COINS) continue;
    if (IS_MASTER) {
      COIN_COUNT++;
      ESP_LOGI(TAG, "Coin pulse accepted — count: %d", COIN_COUNT);
      xTimerReset(COIN_INSERT_TIMER, 0);
      xTaskNotify(UPDATE_CLIENT_TASK, 1, eSetBits);
    } else {
      slaveSendCoinInserted();
    }
  }
}

// Pushes WS status updates to the inserting client.
// Woken immediately by coinPulseTask on each coin (for instant coin count feedback),
// or fires on the 500ms timeout to keep the countdown timer in the UI updated.
// No-op when no session is active.
void updateClientTask(void*) {
  for (;;) {
    xTaskNotifyWait(0, ULONG_MAX, NULL, pdMS_TO_TICKS(500));
    if (COIN_INSERT_ACTIVE) pushCoinUpdateToClient();
  }
}

// FinalizeCoinTask: sleeps until coinTimerCallback() signals COIN_FINALIZE_SEM, then
// runs finalizeCoinInsert() under OMADA_API_MUTEX so it never races OmadaTask.
// Pinned to Core 1 alongside loop() — never touches the radio stack on Core 0.
void finalizeCoinTask(void*) {
  for (;;) {
    xSemaphoreTake(COIN_FINALIZE_SEM, portMAX_DELAY);
    xSemaphoreTake(OMADA_API_MUTEX, portMAX_DELAY);
    finalizeCoinInsert();
    xSemaphoreGive(OMADA_API_MUTEX);
  }
}

// OmadaTask: drains PAUSE/RESUME jobs posted by WS handlers. Each job takes
// OMADA_API_MUTEX before HTTPS so it never races CoinTask.
// Pinned to Core 0 — runs alongside PsychicHTTP tasks that enqueue the jobs.
void omadaTask(void*) {
  OmadaJob job;
  for (;;) {
    if (xQueueReceive(OMADA_JOB_QUEUE, &job, portMAX_DELAY) == pdTRUE) {
      xSemaphoreTake(OMADA_API_MUTEX, portMAX_DELAY);
      if (job.type == JOB_PAUSE)         processPause(String(job.mac));
      if (job.type == JOB_RESUME)        processResume(String(job.mac));
      if (job.type == JOB_REFRESH_CACHE) refreshHotspotSessionCache();
      xSemaphoreGive(OMADA_API_MUTEX);
    }
  }
}

// ── Arduino entry points ──────────────────────────────────────────────────────

void setup() {
  delay(1000);
  Serial.begin(115200);
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

  pinMode(COIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COIN_PIN), coinPulse, FALLING);

  WiFi.mode(WIFI_STA);
  WiFi.begin(AP_SSID.c_str(), AP_PASSWORD.c_str());

  IS_MASTER = isMaster();
  ESP_LOGI(TAG, "Role: %s (MAC: %s)", IS_MASTER ? "MASTER" : "SLAVE", WiFi.macAddress().c_str());
  ledSetup();
  xTaskCreatePinnedToCore(ledTask, "LedTask", 2048, NULL, 2, &LED_TASK, 1);

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

  if (IS_MASTER) {
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
  }

  COIN_STATE_MUTEX  = xSemaphoreCreateMutex();
  OMADA_API_MUTEX   = xSemaphoreCreateMutex();
  COIN_FINALIZE_SEM = xSemaphoreCreateBinary();
  if (IS_MASTER) {
    COIN_INSERT_TIMER = xTimerCreate("CoinTimer", pdMS_TO_TICKS(COIN_INSERT_TIMEOUT), pdFALSE, NULL, coinTimerCallback);
  }
  OMADA_JOB_QUEUE   = xQueueCreate(8, sizeof(OmadaJob));
  COIN_PULSE_SEM = xSemaphoreCreateCounting(100, 0);

  if (COIN_STATE_MUTEX == NULL || OMADA_API_MUTEX == NULL ||
      COIN_FINALIZE_SEM == NULL ||
      COIN_PULSE_SEM == NULL ||
      OMADA_JOB_QUEUE == NULL ||
      (IS_MASTER && COIN_INSERT_TIMER == NULL)) {
    ESP_LOGE(TAG, "Failed to create synchronization primitives");
  }

  xTaskCreatePinnedToCore(coinPulseTask, "CoinPulseTask", 4096, NULL, 6, NULL, 1);
  if (IS_MASTER) {
    xTaskCreatePinnedToCore(finalizeCoinTask, "FinalizeCoinTask", 8192, NULL, 5, NULL,               1);
    xTaskCreatePinnedToCore(omadaTask,        "OmadaTask",        8192, NULL, 4, NULL,               0);
    xTaskCreatePinnedToCore(updateClientTask, "UpdateClientTask", 4096, NULL, 3, &UPDATE_CLIENT_TASK, 1);
  }

  ledReady();
}

void loop() {
  // omadaLoop(); // disabled — cache refreshed explicitly after each auth/extend/pause/resume

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
}

// ── Web / WS handlers ─────────────────────────────────────────────────────────

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
  esp_err_t err = fileResponse.send();
  ESP_LOGI(TAG, "handleRoot: file send done (%s)", err == ESP_OK ? "ok" : "err");
  return err;
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
      restartCoinInsertTimer();
      xTaskNotify(LED_TASK, LED_NOTIFY_START, eSetValueWithOverwrite);
      coinInsertStarted = true;
    }
    xSemaphoreGive(COIN_STATE_MUTEX);

    if (coinInsertStarted) {
      ESP_LOGI(TAG, "Coin insertion started for MAC: %s via WS", session->clientMac.c_str());
      masterBroadcastCoinInserted();
      request->client()->sendMessage(buildStatusJson(*session).c_str());
    } else {
      request->reply("{\"type\":\"error\",\"message\":\"Another user already inserting coins\"}");
    }

  } else if (action == "pause") {
    OmadaJob job(JOB_PAUSE, session->clientMac);
    xQueueSend(OMADA_JOB_QUEUE, &job, 0);

  } else if (action == "resume") {
    OmadaJob job(JOB_RESUME, session->clientMac);
    xQueueSend(OMADA_JOB_QUEUE, &job, 0);

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

// Called 10s after last coin insertion. Authenticates with Omada and pushes result via WS.
void finalizeCoinInsert() {
  ESP_LOGI(TAG, "======finalizeCoinInsert called");

  // Close the insertion window. COIN_INSERT_ACTIVE stays true so no new session can start
  // and COIN_SOCKET_FD/COIN_CLIENT_IP remain valid for WS reconnect tracking during HTTPS.
  xSemaphoreTake(COIN_STATE_MUTEX, portMAX_DELAY);
  int coinCount = COIN_COUNT;
  COIN_COUNT = 0;
  xTimerStop(COIN_INSERT_TIMER, 0);
  xTaskNotify(LED_TASK, LED_NOTIFY_STOP, eSetValueWithOverwrite);
  xSemaphoreGive(COIN_STATE_MUTEX);
  masterBroadcastSessionEnd();  // tell slaves to stop accepting coins immediately

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

  } else if (coinCount == 0) {
    errorMsg = "No coins inserted";
  } else {
    errorMsg = "No portal session found for this device";
    errorSubtype = ERR_SUBTYPE_NO_SESSION;
  }

  PsychicWebSocketClient* ws = (COIN_SOCKET_FD >= 0) ? wsHandler.getClient(COIN_SOCKET_FD) : nullptr;

  if (ws) {
    if (success) {
      ws->sendMessage("{\"type\":\"authenticated\"}");
      { OmadaJob j(JOB_REFRESH_CACHE, ""); xQueueSend(OMADA_JOB_QUEUE, &j, 0); }  // background: sync sessionEndMillis + clientId
    } else {
      String errJson = "{\"type\":\"error\"";
      if (errorSubtype.length() > 0) errJson += ",\"subtype\":\"" + errorSubtype + "\"";
      errJson += ",\"message\":\"" + errorMsg + "\"";
      if (errorDetail.length() > 0) errJson += ",\"detail\":\"" + errorDetail + "\"";
      errJson += "}";
      ws->sendMessage(errJson.c_str());
    }
  } else {
    ESP_LOGW(TAG, "Auth result not delivered — client already disconnected");
  }

  xSemaphoreTake(COIN_STATE_MUTEX, portMAX_DELAY);
  COIN_INSERT_ACTIVE = false;
  COIN_SOCKET_FD = -1;
  COIN_CLIENT_IP = "";
  xSemaphoreGive(COIN_STATE_MUTEX);
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
    if (ws) ws->sendMessage(buildStatusJson(*params, "paused").c_str());
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
    if (ws) ws->sendMessage(buildStatusJson(*params, "resumed").c_str());
  } else {
    if (ws) ws->sendMessage(("{\"type\":\"error\",\"message\":\"Resume failed\",\"detail\":\"" + errorDetail + "\"}").c_str());
  }
}

// ── Paused session file helpers ───────────────────────────────────────────────

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
