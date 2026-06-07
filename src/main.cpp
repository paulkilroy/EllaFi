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
 *                               - runs coin.finalizeCoinInsert() using shared credential store (omada.cpp)
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
#include "log_buffer.h"
#include "led.h"
#include "network.h"

#include <WiFi.h>
#include <LittleFS.h>
#include <esp_log.h>
#include "logger.h"

#define RETRY_ON_FAIL(fn) do { while (!(fn)) { ledError(); vTaskDelay(pdMS_TO_TICKS(1000)); } } while(0)

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

int           MINUTES_PER_COIN      = 24;
int           PRICE_PER_COIN        = 5;
volatile bool COIN_INSERT_ACTIVE    = false;
volatile bool         COINSLOT_PROGRAM_MODE  = false;
unsigned long         COIN_SLOT_READY_MILLIS = 0;
volatile int  COIN_COUNT         = 0;
int           COIN_SOCKET_FD     = -1;
String        COIN_CLIENT_IP     = "";

String AP_SSID;
String AP_PASSWORD;

// ── Arduino entry points ──────────────────────────────────────────────────────

static bool setupPins() {
  pinMode(COINBUTTON_PIN, INPUT_PULLUP);
  pinMode(COINSLOT_PIN,   INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COINBUTTON_PIN), coinButtonPulse, CHANGE);
  attachInterrupt(digitalPinToInterrupt(COINSLOT_PIN),   coinSlotPulse,   CHANGE);
  WiFi.mode(WIFI_STA);
  IS_MASTER = isMaster();
  ledRoleKnown();
  ESP_LOGI(TAG, "Role: %s (MAC: %s)", IS_MASTER ? "MASTER" : "SLAVE", WiFi.macAddress().c_str());
  return true;
}

static bool setupWifi() {
  WiFi.begin(AP_SSID.c_str(), AP_PASSWORD.c_str());
  ESP_LOGI(TAG, "Connecting to WiFi: %s", AP_SSID.c_str());
  for (int i = 0; i < 5; i++) {
    delay(500);
    if (WiFi.status() == WL_CONNECTED) break;
    ESP_LOGW(TAG, "WiFi not connected — attempt %d/5", i + 1);
  }
  if (WiFi.status() != WL_CONNECTED) {
    ESP_LOGE(TAG, "WiFi failed (SSID: %s)", AP_SSID.c_str());
    WiFi.disconnect(true);
    return false;
  }
  int rssi = WiFi.RSSI();
  ESP_LOGI(TAG, "WiFi connected — IP: %s  GW: %s  RSSI: %d dBm",
    WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), rssi);
  if (rssi < -75) ESP_LOGW(TAG, "Weak signal (%d dBm) — check antenna", rssi);
  return true;
}

static bool setupNtp() {
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
  ESP_LOGI(TAG, "Waiting for NTP time sync...");
  time_t now = 0;
  int retries = 0;
  while (now < NTP_EPOCH_MIN && retries++ < NTP_SYNC_RETRIES) { delay(500); time(&now); }
  if (now < NTP_EPOCH_MIN) {
    ESP_LOGE(TAG, "NTP sync timed out");
    return false;
  }
  ESP_LOGI(TAG, "NTP sync OK: epoch=%lld", (long long)now);
  return true;
}

static void setupTasks() {
  COIN_STATE_MUTEX  = xSemaphoreCreateMutex();
  COIN_FINALIZE_SEM = xSemaphoreCreateBinary();
  if (IS_MASTER) {
    COIN_INSERT_TIMER = xTimerCreate("CoinTimer", pdMS_TO_TICKS(COIN_INSERT_TIMEOUT_MILLIS), pdFALSE, NULL, coinTimerCallback);
  }
  WEBSOCKET_JOB_QUEUE = xQueueCreate(8,                     sizeof(WsJob));
  COIN_PULSE_QUEUE    = xQueueCreate(COIN_PULSE_QUEUE_SIZE,  sizeof(CoinPulse));

  if (COIN_STATE_MUTEX == NULL || COIN_FINALIZE_SEM == NULL ||
      COIN_PULSE_QUEUE == NULL || WEBSOCKET_JOB_QUEUE == NULL ||
      (IS_MASTER && COIN_INSERT_TIMER == NULL)) {
    ESP_LOGE(TAG, "Failed to create synchronization primitives");
    ledHalt();
  }

  xTaskCreatePinnedToCore(coinPulseTask, "CoinPulseTask", 4096, NULL, 6, NULL, 1);
  if (IS_MASTER) {
    xTaskCreatePinnedToCore(finalizeCoinTask, "FinalizeCoinTask", 8192, NULL, 5, NULL,               1);
    xTaskCreatePinnedToCore(webSocketTask,    "WebSocketTask",    8192, NULL, 4, NULL,               0);
    xTaskCreatePinnedToCore(updateClientTask, "UpdateClientTask", 4096, NULL, 3, &UPDATE_CLIENT_TASK, 1);
  }
}

void setup() {
  delay(1000);
  Serial.begin(115200);
  logBufferSetup();
  delay(5000);

  pinMode(COINSLOT_POWER_PIN, OUTPUT);
  digitalWrite(COINSLOT_POWER_PIN, LOW);

  setupLed();
  RETRY_ON_FAIL(setupFilesystem());
  RETRY_ON_FAIL(setupConfig());
  RETRY_ON_FAIL(setupPins());
  RETRY_ON_FAIL(setupWifi());
  setupNetwork();
  RETRY_ON_FAIL(setupNtp());
  setupLogs();
  if (IS_MASTER) {
    RETRY_ON_FAIL(setupOmada());
    setupWeb();
  }
  setupTasks();
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

  // Broadcast this node's health so master can track all nodes and their firmware versions
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL_MILLIS) {
    lastHeartbeat = millis();
    broadcastHeartbeat();
  }

  networkLoop();
  checkSerialCommands();
}
