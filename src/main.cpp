/**
 * EllaFi - ESP32 Coin-Operated WiFi Hotspot Portal
 *
 * Captive portal for coin-operated WiFi access via TP-Link Omada Controller (EAP auth, v5.11+).
 * Uses WebSockets for real-time state push. Session data keyed by client MAC in HOTSPOT_SESSION_CACHE.
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
 *    WebSocketTask            [web.webSocketTask — PAUSE/RESUME/REFRESH queue, HTTPS ~3-4s per job]
 *
 *  Core 1 — Application
 *  Arduino loop() runs here by default.
 *    main.loop()              [lean — network.networkLoop() + daily reboot check only]
 *    CoinSessionTask          [coin.coinSessionTask — sole owner of coin-session state (master only)]
 *                               - drains COIN_SESSION_QUEUE (START / COIN / FRAUD_END events)
 *                               - timed receive (COIN_INSERT_TIMEOUT_MILLIS) IS the insert timer
 *                               - runs finalizeSession() (Omada HTTPS) when the window closes
 *    CoinPulseTask            [coin.coinPulseTask — drains COIN_PULSE_QUEUE]
 *                               - given by coin.coinButtonPulse() ISR or coin.coinSlotPulse() ISR or coin.masterRecvCoinInserted()
 *                               - master: validates pulse, posts COIN_SESSION_COIN to COIN_SESSION_QUEUE
 *                               - slave: forwards pulse to master via network.slaveSendCoinInserted
 *                               - highest priority (6) among app tasks
 *    UpdateClientTask         [web.updateClientTask — pushes WS status on coin or every 500ms]
 *                               - woken by CoinSessionTask via task notification
 *                               - also wakes on 500ms timeout during active session
 *    LedTask                  [led.ledTask — blinks LED during coin insert window]
 *                               - started by xTaskNotify from coin.coinSessionTask
 *                                 and coin.slaveRecvCoinInsertStart
 *                               - stopped by xTaskNotify from coin.coinSessionTask (window close)
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
 *  2. web.handleWsRequest claims the slot (CAS on COIN_INSERT_ACTIVE) and posts
 *     COIN_SESSION_START{socketFd, clientIp} to COIN_SESSION_QUEUE
 *  3. coin.coinSessionTask receives COIN_SESSION_START:
 *      resets count, records slot, opens 10s window, signals LED, broadcasts to slaves
 *
 * Customer inserts coins
 *  1. coin.coinButtonPulse() or coin.coinSlotPulse() ISR -> sends CoinPulse to COIN_PULSE_QUEUE
 *      or coin.masterRecvCoinInserted() -> sends CoinPulse to COIN_PULSE_QUEUE from UDP msg
 *  2. coin.coinPulseTask validates pulse, posts COIN_SESSION_COIN{mac} to COIN_SESSION_QUEUE
 *  3. coin.coinSessionTask increments COIN_COUNT, restarts 10s deadline, notifies UpdateClientTask
 *  4. web.updateClientTask pushes coin count + timing to client immediately (and every 500ms)
 *
 * Customer finishes inserting coins after 10s of inactivity (no coins inserted)
 *  1. UI transitions to "pending" screen while waiting for server confirmation
 *  2. coin.coinSessionTask's timed receive returns empty after 10s -> finalizeSession()
 *      sets COIN_INSERT_ACTIVE=false, notifies LedTask to stop
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
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <esp_log.h>
#include "logger.h"

#define RETRY_ON_FAIL(fn) do { while (!(fn)) { ledError(); vTaskDelay(pdMS_TO_TICKS(1000)); } } while(0)

static const char* TAG = "EllaFi";

// ── Global definitions ────────────────────────────────────────────────────────

SessionCache            HOTSPOT_SESSION_CACHE;
PsychicHttpServer       HTTP_SERVER;
PsychicWebSocketHandler WEBSOCKET_HANDLER;

QueueHandle_t     WEBSOCKET_JOB_QUEUE = NULL;
QueueHandle_t     COIN_PULSE_QUEUE    = NULL;
QueueHandle_t     COIN_SESSION_QUEUE  = NULL;
TaskHandle_t      UPDATE_CLIENT_TASK  = NULL;

bool IS_MASTER = false;
uint32_t RUNNING_IMAGE_SIZE = 0;

int                         MINUTES_PER_COIN      = 24;
int                         PRICE_PER_COIN        = 5;
std::atomic<bool>           COIN_INSERT_ACTIVE{false};
volatile bool               COINSLOT_PROGRAM_MODE  = false;
unsigned long               COIN_SLOT_READY_MILLIS = 0;
std::atomic<int>            COIN_COUNT{0};
std::atomic<CoinSessionSlot> COIN_SESSION_SLOT{ CoinSessionSlot{0, -1} };
std::atomic<unsigned long>  COIN_DEADLINE_MILLIS{0};

String MANAGEMENT_SSID;
String MANAGEMENT_PASSWORD;
String HEALTHCHECK_URL;

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
  WiFi.begin(MANAGEMENT_SSID.c_str(), MANAGEMENT_PASSWORD.c_str());
  ESP_LOGI(TAG, "Connecting to WiFi: %s", MANAGEMENT_SSID.c_str());
  for (int i = 0; i < 5; i++) {
    delay(500);
    if (WiFi.status() == WL_CONNECTED) break;
    ESP_LOGW(TAG, "WiFi not connected — attempt %d/5", i + 1);
  }
  if (WiFi.status() != WL_CONNECTED) {
    ESP_LOGE(TAG, "WiFi failed (SSID: %s)", MANAGEMENT_SSID.c_str());
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
  WEBSOCKET_JOB_QUEUE = xQueueCreate(8,                     sizeof(WsJob));
  COIN_PULSE_QUEUE    = xQueueCreate(COIN_PULSE_QUEUE_SIZE,  sizeof(CoinPulse));
  if (IS_MASTER) {
    COIN_SESSION_QUEUE = xQueueCreate(16, sizeof(CoinSessionEvent));
  }

  if (COIN_PULSE_QUEUE == NULL || WEBSOCKET_JOB_QUEUE == NULL ||
      (IS_MASTER && COIN_SESSION_QUEUE == NULL)) {
    ESP_LOGE(TAG, "Failed to create synchronization primitives");
    ledHalt();
  }

  xTaskCreatePinnedToCore(coinPulseTask, "CoinPulseTask", 4096, NULL, 6, NULL, 1);
  if (IS_MASTER) {
    xTaskCreatePinnedToCore(coinSessionTask,  "CoinSessionTask",  8192, NULL, 5, NULL,               1);
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
  RUNNING_IMAGE_SIZE = ESP.getSketchSize();  // our true image size, for serving slave OTA
  ESP_LOGI(TAG, "Running firmware: build %d (%s), image %u bytes",
           FIRMWARE_BUILD, FIRMWARE_VERSION, (unsigned)RUNNING_IMAGE_SIZE);
  setupNetwork();
  setupNtp();          // best-effort — never block the admin; loop() retries until synced
  setupLogs();
  if (IS_MASTER) {
    setupWeb();        // ALWAYS up first, so /admin is reachable even if NTP/Omada are down
    setupOmada();      // best-effort — loop() retries login + site-load in the background
  }
  setupTasks();
  ledReady();
}

// Master dead-man's switch: POST a heartbeat to Healthchecks.io. If the master loses power, crashes, or
// loses internet, the pings stop and Healthchecks alerts after its grace period — the one failure mode
// the device can't report itself. Bounded timeout so a dead link can't stall the loop (and selling is
// already off when there's no internet). Disabled when healthchecks_url is empty.
static void pingHealthcheck() {
  if (HEALTHCHECK_URL.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure wc; wc.setInsecure(); wc.setTimeout(4000);
  HTTPClient h; h.setConnectTimeout(4000); h.setTimeout(4000);
  if (!h.begin(wc, HEALTHCHECK_URL)) return;
  h.addHeader("Content-Type", "application/json");
  String body = String("{\"mac\":\"") + WiFi.macAddress() +
                "\",\"uptime_s\":" + String(millis() / 1000) +
                ",\"active_sessions\":" + String((unsigned)HOTSPOT_SESSION_CACHE.size()) + "}";
  int code = h.POST(body);
  h.end();
  if (code == 200) ESP_LOGI(TAG, "healthcheck ping ok");
  else             ESP_LOGW(TAG, "healthcheck ping failed (HTTP %d)", code);
}

void loop() {
  // Scheduled daily reboot at 3:00 AM LOCAL — MASTER-DRIVEN. Slaves never set TZ (no controller calls),
  // so a slave watching its own clock would fire at 3am UTC = 11am peak. So only the master watches the
  // clock; at 3am local it broadcasts NET_MSG_REBOOT to cycle the slaves, then restarts itself — the whole
  // village cycles together. localtime() honours the controller-set TZ (UTC+8).
  static unsigned long lastRebootCheck = 0;
  if (IS_MASTER && millis() - lastRebootCheck > MILLIS_PER_MINUTE) {
    lastRebootCheck = millis();
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    if (t->tm_hour == 3 && t->tm_min == 0) {
      ESP_LOGI(TAG, "Scheduled 3am reboot — cascading to slaves, then restarting");
      for (int i = 0; i < 3; i++) { masterBroadcastReboot(); delay(100); }
      esp_restart();
    }
  }

  // Broadcast this node's health so master can track all nodes and their firmware versions
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL_MILLIS) {
    lastHeartbeat = millis();
    broadcastHeartbeat();
  }

  // Service-health monitor (master): keep /admin reachable when NTP/Omada are down, retry them in
  // the background, and reflect the state on the LED. Coin selling is gated on isServiceReady().
  if (IS_MASTER) {
    static bool wasReady = true;
    static unsigned long lastServiceRetry = 0;
    bool ready = isServiceReady();
    if (ready != wasReady) {
      ready ? ledReady() : ledError();   // solid idle when healthy, red blink when out of service
      ESP_LOGW(TAG, ready ? "Service restored — coin selling enabled"
                          : "OUT OF SERVICE — NTP/controller down; coin selling paused (admin still up)");
      wasReady = ready;
    }
    if (!ready && millis() - lastServiceRetry > 30000) {
      lastServiceRetry = millis();
      if (time(NULL) < NTP_EPOCH_MIN)
        configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");  // non-blocking NTP re-trigger
      setupOmada();   // re-login + reload SITE_ID (recovers when controller is back / URL fixed)
    }

    static unsigned long lastHealthcheck = 0;
    if (millis() - lastHealthcheck > MILLIS_PER_MINUTE) {  // dead-man's switch, master alive + online
      lastHealthcheck = millis();
      pingHealthcheck();
    }
  }

  networkLoop();
  checkSerialCommands();
}
