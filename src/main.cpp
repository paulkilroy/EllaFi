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
#include "reprovision.h"
#include "omada_device.h"
#include "money_store.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <esp_log.h>
#include <esp_system.h>       // esp_reset_reason() — crash vs clean-reboot forensics
#include <esp_heap_caps.h>    // heap_caps_register_failed_alloc_callback()
#include <nvs.h>              // nvs_get_stats() — where the money record lives
#include <freertos/task.h>   // uxTaskGetSystemState() — per-task stack head-room
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

// Alloc-failure counters — the registered hook bumps these the instant ANY malloc fails. The hook must
// not allocate (it would re-enter), so it only counts; the count surfaces in logResources().
static std::atomic<uint32_t> ALLOC_FAIL_COUNT{0};
static std::atomic<uint32_t> ALLOC_FAIL_LAST{0};   // byte size of the most recent failed allocation

static void logResources(const char* ctx);   // defined below; called from setup()'s boot baseline
static void logStacks(const char* ctx);

// PANIC/WDT/BROWNOUT = crash (heap/socket logs won't show it); SW = clean 3am reboot; POWERON = power cut.
static const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON (power cut)";
    case ESP_RST_SW:        return "SW (clean restart)";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "INT_WDT (watchdog)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (watchdog)";
    case ESP_RST_WDT:       return "WDT (watchdog)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (power dip)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    default:                return "other";
  }
}

static TaskHandle_t COIN_SESSION_TASK = nullptr;   // heaviest stacks (TLS+JSON) — watched by logStacks()
static TaskHandle_t WEBSOCKET_TASK    = nullptr;
TaskHandle_t      UPDATE_CLIENT_TASK  = NULL;

bool IS_MASTER = false;
uint32_t RUNNING_IMAGE_SIZE = 0;

int                         MINUTES_PER_COIN      = 24;
int                         PRICE_PER_COIN        = 1;
int                         FALLBACK_PRICE_PER_VOUCHER = 5;
std::atomic<bool>           COIN_INSERT_ACTIVE{false};
volatile bool               COINSLOT_PROGRAM_MODE  = false;
unsigned long               COIN_SLOT_READY_MILLIS = 0;
std::atomic<int>            COIN_COUNT{0};
std::atomic<uint16_t>       COIN_REJECT_WIDTH_COUNT{0};
std::atomic<uint16_t>       COIN_REJECT_INTERVAL_COUNT{0};
std::atomic<CoinSessionSlot> COIN_SESSION_SLOT{ CoinSessionSlot{0, -1} };
std::atomic<unsigned long>  COIN_DEADLINE_MILLIS{0};

String MANAGEMENT_SSID;
String MANAGEMENT_PASSWORD;
String HEALTHCHECK_URL;
bool   OMADA_DEVICE_ENABLE = false;
String STATIC_IP;
String STATIC_GATEWAY;
String STATIC_SUBNET;
String STATIC_DNS;

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
  // Optional static IP (config static_ip). Supply just the IP and we default the rest for a /24:
  // gateway = <ip>.1, subnet = 255.255.255.0, DNS = gateway. Empty static_ip = DHCP.
  if (!STATIC_IP.isEmpty()) {
    IPAddress ip, gw, mask, dns;
    if (!ip.fromString(STATIC_IP)) {
      ESP_LOGE(TAG, "static_ip invalid: %s — using DHCP", STATIC_IP.c_str());
    } else {
      String gwStr = STATIC_GATEWAY.isEmpty()
                       ? STATIC_IP.substring(0, STATIC_IP.lastIndexOf('.')) + ".1"
                       : STATIC_GATEWAY;
      gw.fromString(gwStr);
      mask.fromString(STATIC_SUBNET.isEmpty() ? "255.255.255.0" : STATIC_SUBNET);
      dns.fromString(STATIC_DNS.isEmpty() ? gwStr : STATIC_DNS);
      if (WiFi.config(ip, gw, mask, dns))
        ESP_LOGI(TAG, "Static IP: %s  GW: %s  mask: %s  DNS: %s",
                 STATIC_IP.c_str(), gwStr.c_str(), mask.toString().c_str(), dns.toString().c_str());
      else
        ESP_LOGE(TAG, "WiFi.config(static) failed — using DHCP");
    }
  }
  // IPv6 link-local on every (re)connect: without it mDNS never answers AAAA queries and Apple
  // resolvers wait out a 5s timeout on EVERY ellafi.local connection (the record can't be
  // negatively cached). On the event, not one-shot — a WiFi drop would otherwise lose the address.
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
    bool ok = WiFi.enableIpV6();
    if (!ok) ESP_LOGE(TAG, "enableIpV6 failed — ellafi.local will stall 5s on Apple resolvers");
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);   // GOT_IP, not CONNECTED — the netif isn't up yet at CONNECTED
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    ESP_LOGI(TAG, "IPv6 link-local up: " IPV6STR, IPV62STR(info.got_ip6.ip6_info.ip));
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP6);
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

// Queues must exist BEFORE setupWeb() — its handlers (/admin/recheck, /diag/hammer) enqueue to
// WEBSOCKET_JOB_QUEUE, so a request arriving before the queue is created would xQueueSend(NULL) → crash.
// Consumer tasks come later (setupTasks); enqueuing before the consumer exists just buffers.
static void setupQueues() {
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
}

static void setupTasks() {
  xTaskCreatePinnedToCore(coinPulseTask, "CoinPulseTask", 4096, NULL, 6, NULL, 1);
  if (IS_MASTER) {
    xTaskCreatePinnedToCore(coinSessionTask,  "CoinSessionTask",  8192, NULL, 5, &COIN_SESSION_TASK, 1);
    xTaskCreatePinnedToCore(webSocketTask,    "WebSocketTask",    8192, NULL, 4, &WEBSOCKET_TASK,    0);
    xTaskCreatePinnedToCore(updateClientTask, "UpdateClientTask", 4096, NULL, 3, &UPDATE_CLIENT_TASK, 1);
  }
}

void setup() {
  delay(1000);
  Serial.begin(115200);
  logBufferSetup();
  // Count every failed heap allocation from the very start. The hook must NOT allocate (it would re-enter),
  // so it only bumps counters; logResources() surfaces them.
  heap_caps_register_failed_alloc_callback([](size_t size, uint32_t, const char*) {
    ALLOC_FAIL_COUNT.fetch_add(1); ALLOC_FAIL_LAST.store((uint32_t)size);
  });
  delay(5000);

  pinMode(COINSLOT_POWER_PIN, OUTPUT);
  digitalWrite(COINSLOT_POWER_PIN, LOW);

  setupLed();
  RETRY_ON_FAIL(setupFilesystem());
  RETRY_ON_FAIL(setupConfig());
  moneyStoreBegin();   // NVS money record — best-effort, never blocks boot
  // Legacy file cleanup (2026-08-09): sellers moved to config.json, voucher day-cache moved to RAM
  LittleFS.remove("/sellers.json");
  LittleFS.remove("/voucher_history.json");
  RETRY_ON_FAIL(setupPins());
  if (!setupWifi()) runWifiRecovery();  // solid red + BOOT-button → SoftAP re-provisioning; never returns
  RUNNING_IMAGE_SIZE = ESP.getSketchSize();  // our true image size, for serving slave OTA
  ESP_LOGI(TAG, "Running firmware: build %d (%s), image %u bytes",
           FIRMWARE_BUILD, FIRMWARE_VERSION, (unsigned)RUNNING_IMAGE_SIZE);
  // Early INFO for anyone watching serial; the persisted WARN copy is at the END of setup(), after
  // the NTP attempt — a power-cut reset (POWERON/BROWNOUT) wipes the clock, and a pre-sync WARN here
  // got stamped 1970 and purged, losing the forensics for exactly the resets brownouts cause.
  ESP_LOGI(TAG, "BOOT: build %d, last reset = %s", FIRMWARE_BUILD, resetReasonStr());
  setupNetwork();
  setupNtp();          // best-effort — never block the admin; loop() retries until synced
  setupLogs();
  setupQueues();       // before setupWeb so its handlers never enqueue to a not-yet-created queue
  if (IS_MASTER) {
    setupWeb();        // ALWAYS up first, so /admin is reachable even if NTP/Omada are down
    setupOmada();      // best-effort — loop() retries login + site-load in the background
  }
  setupOmadaDevice();  // Omada device plane (fake-AP) — M1: crypto self-test only; dormant otherwise
  setupTasks();
  // Persisted boot forensics — logged here (after the NTP attempt) so the timestamp is valid whenever
  // sync succeeded. On a WAN-down boot the clock is still bad, but appendErrorLog now keeps pre-sync
  // entries as boot-relative instead of 1970-stamping them into the purge.
  ESP_LOGW(TAG, "BOOT: build %d, last reset = %s", FIRMWARE_BUILD, resetReasonStr());
  logResources("boot");   // healthy baseline for later comparison
  logStacks("boot");
  ledReady();
}

// Master dead-man's switch: POST a heartbeat to Healthchecks.io. If the master loses power, crashes, or
// loses internet, the pings stop and Healthchecks alerts after its grace period — the one failure mode
// the device can't report itself. Bounded timeout so a dead link can't stall the loop (and selling is
// already off when there's no internet). Disabled when healthchecks_url is empty.
// Snapshot the resources whose exhaustion hangs or drops the firmware, logged at WARN so it persists to
// errors.log (survives the reboot). A future "it hung at time X" then always carries the state at that moment:
//   sockets  — web pool usage; shares the 16 LWIP sockets with every OUTBOUND TLS call, so near-full = outbound starves
//   largest  — largest contiguous internal-DRAM block; mbedTLS needs ~40 KB or the handshake fails (-32512 / read timeout)
//   minHeap  — lowest internal-DRAM the firmware has ever seen (the true low-water mark)
//   psram    — 8 MB pool where the big JSON parses live now; should stay large
//   wsq/tls  — WS job backlog and concurrent TLS handshakes (contention)
static void logResources(const char* ctx) {
  // Two <128-char lines (LOG_BUF_MSG_LEN) so neither truncates in the ring buffer / errors.log.
  ESP_LOGW(TAG, "RES %s mem: dramFree=%u largest=%u minHeap=%u psram=%u allocFails=%u(last=%u)",
           ctx,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)ALLOC_FAIL_COUNT.load(), (unsigned)ALLOC_FAIL_LAST.load());

  nvs_stats_t nv = {}; nvs_get_stats(nullptr, &nv);
  size_t fsTot = LittleFS.totalBytes();
  ESP_LOGW(TAG, "RES %s io: sockets=%u/%u wsq=%u pulseQ=%u sessQ=%u tls=%d fsFree=%u nvsFree=%u/%u",
           ctx,
           (unsigned)HTTP_SERVER.getClientList().size(), (unsigned)HTTP_SERVER.config.max_open_sockets,
           (unsigned)uxQueueMessagesWaiting(WEBSOCKET_JOB_QUEUE),
           (unsigned)(COIN_PULSE_QUEUE   ? uxQueueMessagesWaiting(COIN_PULSE_QUEUE)   : 0),
           (unsigned)(COIN_SESSION_QUEUE ? uxQueueMessagesWaiting(COIN_SESSION_QUEUE) : 0),
           omadaTlsInFlight(),
           (unsigned)(fsTot ? fsTot - LittleFS.usedBytes() : 0),
           (unsigned)nv.free_entries, (unsigned)nv.total_entries);
}

// Per-task stack head-room — the tightest task is the crash risk (stack overflow → panic reboot, which
// heap/socket metrics never show). Enumerates all tasks; logs the minimum high-water mark and which task.
// WARN (→ errors.log) only for the once-per-boot baseline or when head-room is actually tight — the
// healthy every-10-min line stays at INFO (ring buffer only) so it can't drown errors.log.
constexpr UBaseType_t STACK_TIGHT_FREE_BYTES = 1536;
static void logStacks(const char* ctx) {
  // uxTaskGetSystemState needs the trace facility (off here), so poll the risk tasks by handle instead.
  UBaseType_t minHW = uxTaskGetStackHighWaterMark(nullptr); const char* who = "loop";   // current (loop) task
  TaskHandle_t hs[] = { COIN_SESSION_TASK, WEBSOCKET_TASK, UPDATE_CLIENT_TASK };
  const char*  ns[] = { "CoinSession",     "WebSocket",    "UpdateClient" };
  for (int i = 0; i < 3; i++) {
    if (!hs[i]) continue;
    UBaseType_t hw = uxTaskGetStackHighWaterMark(hs[i]);
    if (hw < minHW) { minHW = hw; who = ns[i]; }
  }
  if (minHW < STACK_TIGHT_FREE_BYTES || strcmp(ctx, "boot") == 0)
    ESP_LOGW(TAG, "STACK %s: minFree=%u in '%s' (of loop/coin/ws/upd)", ctx, (unsigned)minHW, who);
  else
    ESP_LOGD(TAG, "STACK %s: minFree=%u in '%s' (of loop/coin/ws/upd)", ctx, (unsigned)minHW, who);
}

static void pingHealthcheck() {
  if (HEALTHCHECK_URL.isEmpty() || WiFi.status() != WL_CONNECTED) return;

  // Fold coin-node health into the master's dead-man's switch: any node we've seen that has gone
  // silent (missed ~4 heartbeats) flips the SAME check to /fail, so the operator gets one generic
  // "something's down" alert and pulls up /admin to see which node. All healthy → normal success ping.
  // (A dead master trips it too — it stops pinging entirely.)
  String ownMac = WiFi.macAddress(); ownMac.toUpperCase();
  String downMacs; int downCount = 0;
  for (const NodeInfo& ni : getKnownNodes()) {
    String mac = ni.mac; mac.toUpperCase();
    if (mac == ownMac) continue;                                          // skip the master's own echo
    if (millis() - ni.lastSeenMs < NODE_OFFLINE_THRESHOLD_MILLIS) continue;
    if (downCount++) downMacs += ",";
    downMacs += mac;
  }

  // Plain HTTP — a dead-man's-switch heartbeat needs no TLS. The check UUID is already in the URL, and
  // dropping the ~40 KB TLS handshake stops the ping's connect from starving internal DRAM (the HTTP -1s)
  // when the Omada refresh task happens to be mid-handshake on the other core. hc-ping.com serves plain
  // HTTP directly (no https redirect). Tradeoff: the UUID + benign stats travel cleartext — fine for a ping.
  WiFiClient client;
  String url = HEALTHCHECK_URL; url.replace("https://", "http://");
  if (downCount) url += "/fail";
  // 10 s, not 4: Starlink's latency tail (retransmit backoff after a lost packet) blew the old 4 s
  // cap and logged latency spikes as outages — the "4006ms" failure cluster. A heartbeat can wait.
  HTTPClient h; h.setConnectTimeout(10000); h.setTimeout(10000);
  if (!h.begin(client, url)) return;
  h.addHeader("Content-Type", "application/json");
  String body = String("{\"mac\":\"") + ownMac +
                "\",\"uptime_s\":" + String(millis() / 1000) +
                ",\"active_sessions\":" + String((unsigned)HOTSPOT_SESSION_CACHE.size()) +
                ",\"nodes_down\":" + String(downCount) +
                (downCount ? (",\"down\":\"" + downMacs + "\"") : String("")) + "}";
  unsigned long t0 = millis();
  int code = h.POST(body);
  h.end();
  // Contention snapshot on failure: elapsed separates instant connect/DNS fails (-1, ~0ms) from
  // timeouts (-11, ~4000ms); tls counts concurrent Omada handshakes (the DRAM competitor); wsq is
  // the Omada job backlog; dram/largest expose heap squeeze/fragmentation. All ~equal across many
  // failures + tls=0 + healthy heap = the WAN is dropping, not the firmware.
  if (code != 200) {
    ESP_LOGW(TAG, "healthcheck ping failed (HTTP %d) after %lums — sockets=%u/%u tls=%d wsq=%u rssi=%d dram=%u largest=%u",
             code, millis() - t0,
             (unsigned)HTTP_SERVER.getClientList().size(), (unsigned)HTTP_SERVER.config.max_open_sockets,
             omadaTlsInFlight(),
             (unsigned)uxQueueMessagesWaiting(WEBSOCKET_JOB_QUEUE), WiFi.RSSI(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    // Trisect the failure for the PtP-bridge theory (north↔south Bakhaw): the local AP is fine
    // (rssi above), so probe the gateway and the controller directly. gw FAIL = near segment;
    // gw OK + controller FAIL = far segment / bridge; both OK = the WAN upstream is what dropped.
    String ctrlHost = CONTROLLER_BASE_URL;                       // e.g. https://192.168.1.83:443
    ctrlHost = ctrlHost.substring(ctrlHost.indexOf("//") + 2);
    if (ctrlHost.indexOf(':') > 0) ctrlHost = ctrlHost.substring(0, ctrlHost.indexOf(':'));
    auto tcpAlive = [](const char* host, uint16_t port) {
      WiFiClient c; bool ok = c.connect(host, port, 1500); c.stop(); return ok;
    };
    // dish = the Starlink terminal's fixed management address (gRPC port, always listening).
    // wan1/wan2 = IP-literal anycast targets (no DNS): both FAIL = the internet is truly down;
    // both OK + ping failed = the outage was DNS, the endpoint, or a latency spike — see dns/retry.
    // dns = resolving the healthcheck host (the one lookup the ping itself depends on).
    // retry = an immediate second ping: OK here means the "outage" was a moment, not a state.
    String hcHost = url;
    hcHost.replace("http://", "");
    if (hcHost.indexOf('/') > 0) hcHost = hcHost.substring(0, hcHost.indexOf('/'));
    IPAddress dnsIp; unsigned long d0 = millis();
    bool dnsOk = WiFi.hostByName(hcHost.c_str(), dnsIp) == 1;
    unsigned long dnsMs = millis() - d0;
    bool gw   = tcpAlive(WiFi.gatewayIP().toString().c_str(), 80);
    bool ctrl = tcpAlive(ctrlHost.c_str(), 443);
    bool dish = tcpAlive("192.168.100.1", 9200);
    bool wan1 = tcpAlive("1.1.1.1", 443);
    bool wan2 = tcpAlive("8.8.8.8", 53);
    unsigned long r0 = millis(); int rcode = -999;
    { HTTPClient h2; WiFiClient c2; h2.setConnectTimeout(10000); h2.setTimeout(10000);
      if (h2.begin(c2, url)) { h2.addHeader("Content-Type", "application/json"); rcode = h2.POST(body); h2.end(); } }
    ESP_LOGW(TAG, "path probe: gw=%s ctrl=%s dish=%s wan1=%s wan2=%s dns=%s(%lums) retry=%d(%lums)",
             gw ? "OK" : "FAIL", ctrl ? "OK" : "FAIL", dish ? "OK" : "FAIL",
             wan1 ? "OK" : "FAIL", wan2 ? "OK" : "FAIL",
             dnsOk ? "OK" : "FAIL", dnsMs, rcode, millis() - r0);
  }
  else if (downCount) ESP_LOGW(TAG, "healthcheck /fail sent — %d node(s) down: %s", downCount, downMacs.c_str());
  else                ESP_LOGD(TAG, "healthcheck ping ok");
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
    // 2:45am: money rollup (voucher groups + income refresh) — before the 3am reboot wipes RAM state
    if (t->tm_hour == 2 && t->tm_min == 45) moneyNightlyRollup();
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

  // Periodic stack head-room check (all nodes) — slow-moving, so 10 min is plenty; catches a task creeping
  // toward overflow before it panics.
  static unsigned long lastStackCheck = 0;
  if (millis() - lastStackCheck > 10 * MILLIS_PER_MINUTE) {
    lastStackCheck = millis();
    logStacks("periodic");
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
      if (!ready) logResources("at OUT OF SERVICE");   // capture WHAT was exhausted at the moment we dropped
      wasReady = ready;
    }

    // Proactive early-warning: if sockets are near the pool cap or DRAM can't fit a TLS handshake, record it
    // BEFORE it turns into an outage — so a slow degradation leaves a breadcrumb trail. The check runs every
    // 5s (the heap-walk isn't free, so NOT every loop); the WARN itself is throttled to once / 2 min while tight.
    static unsigned long lastResCheck = 0, lastResWarn = 0, lastSockWarn = 0;
    if (millis() - lastResCheck > 5000) {
      lastResCheck = millis();
      unsigned sockInUse = HTTP_SERVER.getClientList().size();
      // Socket utilization over 10 (of 12) = portal pool nearly exhausted. A distinct, named event
      // (separate from the resource dump below) so it reads clearly and, once the Phase-B device port
      // is on-device, maps to an Omada controller Alert — it rides the W/E → errors.log + forward path.
      if (sockInUse > 10u && millis() - lastSockWarn > 2 * MILLIS_PER_MINUTE) {
        lastSockWarn = millis();
        ESP_LOGW(TAG, "SOCKETS high: %u/%u in use — portal pool nearly exhausted",
                 sockInUse, (unsigned)HTTP_SERVER.config.max_open_sockets);
      }
      if ((sockInUse >= HTTP_SERVER.config.max_open_sockets - 1u ||
           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 45000u) &&
          millis() - lastResWarn > 2 * MILLIS_PER_MINUTE) {
        lastResWarn = millis();
        logResources("TIGHT");
      }
    }

    // Feed the admin's resource-utilization graph — sample every 30s; sampleResources() folds these
    // into 10-min worst-case buckets (144 = last 24 h).
    static unsigned long lastResSample = 0;
    if (millis() - lastResSample > 30000) {
      lastResSample = millis();
      sampleResources();
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
