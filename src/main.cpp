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
 * ── Threading model ───────────────────────────────────────────────────────────
 *
 *  ESP32 has 2 cores (Xtensa LX6 dual-core):
 *
 *  Core 0 — Radio & System
 *  Espressif reserves Core 0 for WiFi/BT stack. Tasks here must not starve the radio.
 *    WiFi/lwIP stack          [Espressif system, always here]
 *    PsychicHTTP task(s)      [one per connection, handles HTTP + WS frames]
 *    FreeRTOS Timer Task      [runs FreeRTOS timer callbacks — coin.coinTimerCallback]
 *    WebSocketTask            [web.webSocketTask — PAUSE/RESUME/REFRESH queue, HTTPS ~3-4s per job]
 *
 *  Core 1 — Application
 *  Arduino loop() runs here by default.
 *    main.loop()              [lean — network.networkLoop() + daily reboot check only]
 *    FinalizeCoinTask         [coin.finalizeCoinTask — woken by COIN_FINALIZE_SEM when timer fires]
 *                               - runs coin.finalizeCoinInsert() with task-local OmadaSession
 *    CoinPulseTask            [coin.coinPulseTask — drains COIN_PULSE_QUEUE]
 *                               - given by coin.coinButtonPulse() ISR or coin.coinSlotPulse() ISR or coin.masterRecvCoinInserted()
 *                               - master: increments COIN_COUNT, resets timer, notifies UpdateClientTask
 *                               - slave: forwards pulse to master via network.slaveSendCoinInserted
 *                               - highest priority (6) among app tasks
 *    UpdateClientTask         [web.updateClientTask — pushes WS status on coin or every 500ms]
 *                               - woken by CoinPulseTask via task notification
 *                               - also wakes on 500ms timeout during active session
 *    LedTask                  [led.ledTask — blinks LED during coin insert window]
 *                               - started by xTaskNotify from web.handleWsRequest("startCoinInsert")
 *                                 and coin.slaveRecvCoinInsertStart
 *                               - stopped by xTaskNotify from coin.finalizeCoinInsert
 *                                 and coin.slaveRecvCoinInsertEnd
 *
 *  ISR (no core, interrupt level)
 *    coin.coinButtonPulse()   [GPIO ISR, debounced — COINBUTTON_PIN (boot button)]
 *    coin.coinSlotPulse()     [GPIO ISR, no debounce — COINSLOT_PIN (real acceptor)]
 *
 * ── Coin Flow ───────────────────────────────────────────────────────────
 * 
 * Customer clicks "Insert Coin" on captive portal
 *  1. Browser sends WS {"action":"startCoinInsert"} to the server and shows coin insert screen
 *  2. web.handleWsRequest receives startCoinInsert:
 *      sets COIN_COUNT = 0, COIN_INSERT_ACTIVE=true
 *      remembers the socket and IP of the client to ensure only that client owns this transaction
 *      starts COIN_INSERT_TIMER (10s) via coin.restartCoinInsertTimer
 *      signals LedTask to begin blinking via xTaskNotify
 *      broadcasts session start to slaves via network.masterBroadcastSessionStart
 *
 * Customer inserts coins
 *  1. coin.coinButtonPulse() or coin.coinSlotPulse() ISR -> sends CoinPulse to COIN_PULSE_QUEUE
 *      or coin.masterRecvCoinInserted() -> sends CoinPulse to COIN_PULSE_QUEUE from UDP msg
 *  2. coin.coinPulseTask drains COIN_PULSE_QUEUE -> increments COIN_COUNT,
 *      resets COIN_INSERT_TIMER back to 10s via xTimerReset
 *      notifies UpdateClientTask to push update to client
 *  3. web.updateClientTask pushes coin count + timing to client immediately (and every 500ms)
 *
 * Customer finishes inserting coins after 10s of inactivity (no coins inserted)
 *  1. UI transitions to "pending" screen while waiting for server confirmation
 *  2. coin.coinTimerCallback fires when COIN_INSERT_TIMER expires -> gives COIN_FINALIZE_SEM
 *  3. coin.finalizeCoinTask wakes on COIN_FINALIZE_SEM -> calls coin.finalizeCoinInsert()
 *      sets COIN_INSERT_ACTIVE=false
 *      notifies LedTask to stop
 *      calls omada.authenticateOmadaClient or omada.extendOmadaClient
 *      pushes "authenticated" result via WS
 */

#include "globals.h"
#include "files.h"
#include "coin.h"
#include "web.h"

#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <esp_log.h>
#include "logger.h"

static const char* TAG = "EllaFi";

// ── Global definitions ────────────────────────────────────────────────────────

SessionCache            HOTSPOT_SESSION_CACHE;
PsychicHttpServer       server;
PsychicWebSocketHandler wsHandler;

QueueHandle_t     WEBSOCKET_JOB_QUEUE = NULL;
QueueHandle_t     COIN_PULSE_QUEUE    = NULL;
SemaphoreHandle_t COIN_FINALIZE_SEM   = NULL;
SemaphoreHandle_t COIN_STATE_MUTEX    = NULL;
TaskHandle_t      UPDATE_CLIENT_TASK  = NULL;
TimerHandle_t     COIN_INSERT_TIMER   = NULL;

bool IS_MASTER = false;

volatile bool COIN_INSERT_ACTIVE    = false;
volatile bool         COINSLOT_PROGRAM_MODE  = false;
unsigned long         COIN_SLOT_READY_MILLIS = 0;
volatile int  COIN_COUNT         = 0;
int           COIN_SOCKET_FD     = -1;
String        COIN_CLIENT_IP     = "";

String AP_SSID;
String AP_PASSWORD;

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
    ESP_LOGE(TAG, "Failed to load config - halting");
    halt();
  }

  pinMode(COINBUTTON_PIN,    INPUT_PULLUP);
  pinMode(COINSLOT_PIN, INPUT_PULLUP);
  pinMode(COINSLOT_POWER_PIN,   OUTPUT);
  digitalWrite(COINSLOT_POWER_PIN, LOW);  // coin slot off until session starts
  attachInterrupt(digitalPinToInterrupt(COINBUTTON_PIN),    coinButtonPulse,     CHANGE);
  attachInterrupt(digitalPinToInterrupt(COINSLOT_PIN), coinSlotPulse, CHANGE);

  WiFi.mode(WIFI_STA);
  WiFi.begin(AP_SSID.c_str(), AP_PASSWORD.c_str());

  IS_MASTER = isMaster();
  ESP_LOGI(TAG, "Role: %s (MAC: %s)", IS_MASTER ? "MASTER" : "SLAVE", WiFi.macAddress().c_str());
  ledSetup();
  xTaskCreatePinnedToCore(ledTask, "LedTask", 2048, NULL, 2, &LED_TASK, 1);

  ESP_LOGI(TAG, "Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    if (WiFi.status() == WL_NO_SSID_AVAIL) {
      ESP_LOGE(TAG, "WiFi network not found: %s", AP_SSID.c_str());
      delay(WIFI_RETRY_DELAY_MILLIS);
    } else if (WiFi.status() == WL_CONNECT_FAILED) {
      ESP_LOGE(TAG, "WiFi connection failed with provided credentials");
      delay(WIFI_RETRY_DELAY_MILLIS);
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
    while (now < NTP_EPOCH_MIN && retries++ < NTP_SYNC_RETRIES) { delay(500); time(&now); }
    if (now < NTP_EPOCH_MIN) {
      ESP_LOGW(TAG, "NTP sync timed out — epoch timestamps may be incorrect");
    } else {
      ESP_LOGI(TAG, "NTP sync OK: epoch=%lld", (long long)now);
    }
  }

  purgeOldLogEntries("/errors.log");
  purgeOldLogEntries("/refunds.log");

  if (IS_MASTER) {
    omadaSetup();

    if (MDNS.begin("ellafi")) {
      ESP_LOGI(TAG, "mDNS started: ellafi.local");
    } else {
      ESP_LOGW(TAG, "mDNS failed to start");
    }

    server.config.stack_size = 16384;  // Larger stack for SSL calls in WS handlers

    server.on("/", HTTP_GET, handleRoot);

    wsHandler.onOpen(handleWsOpen);
    wsHandler.onFrame(handleWsRequest);
    wsHandler.onClose(handleWsClose);
    server.on("/ws", &wsHandler);

    server.on("/refunds",     HTTP_GET, handleRefunds);
    server.on("/errors",      HTTP_GET, handleErrors);
    server.on("/program",     HTTP_GET, handleProgram);
    server.on("/config.json", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
      return response->send(403, "text/plain", "Forbidden");
    });
    server.serveStatic("/", LittleFS, "/", "max-age=86400");
    server.onNotFound(handleNotFound);  // Must be set AFTER serveStatic

    server.begin();
    ESP_LOGI(TAG, "Server started");
  }

  COIN_STATE_MUTEX  = xSemaphoreCreateMutex();
  COIN_FINALIZE_SEM = xSemaphoreCreateBinary();
  if (IS_MASTER) {
    COIN_INSERT_TIMER = xTimerCreate("CoinTimer", pdMS_TO_TICKS(COIN_INSERT_TIMEOUT_MILLIS), pdFALSE, NULL, coinTimerCallback);
  }
  WEBSOCKET_JOB_QUEUE = xQueueCreate(8,                    sizeof(WsJob));
  COIN_PULSE_QUEUE    = xQueueCreate(COIN_PULSE_QUEUE_SIZE, sizeof(CoinPulse));

  if (COIN_STATE_MUTEX == NULL ||
      COIN_FINALIZE_SEM == NULL ||
      COIN_PULSE_QUEUE == NULL ||
      WEBSOCKET_JOB_QUEUE == NULL ||
      (IS_MASTER && COIN_INSERT_TIMER == NULL)) {
    ESP_LOGE(TAG, "Failed to create synchronization primitives");
  }

  xTaskCreatePinnedToCore(coinPulseTask,    "CoinPulseTask",    4096, NULL, 6, NULL,               1);
  if (IS_MASTER) {
    xTaskCreatePinnedToCore(finalizeCoinTask, "FinalizeCoinTask", 8192, NULL, 5, NULL,              1);
    xTaskCreatePinnedToCore(webSocketTask,    "WebSocketTask",    8192, NULL, 4, NULL,              0);
    xTaskCreatePinnedToCore(updateClientTask, "UpdateClientTask", 4096, NULL, 3, &UPDATE_CLIENT_TASK, 1);
  }

  ledReady();
}

void loop() {
  // Scheduled daily reboot at 3:00 AM UTC — simplest way to keep memory and cache clean
  static unsigned long lastRebootCheck = 0;
  if (millis() - lastRebootCheck > MILLIS_PER_MINUTE) {
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
