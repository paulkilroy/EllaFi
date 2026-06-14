#include "network.h"
#include "coin.h"     // masterRecvCoinInserted, slaveRecvCoinInsert{Start,End}
#include "globals.h"  // FIRMWARE_VERSION, FIRMWARE_BUILD, OTA_REBOOT_DELAY_MILLIS
#include "files.h"     // appendErrorLog, logAppend
#include "log_buffer.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <Update.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_log.h>
#include <map>
#include <mbedtls/md.h>
#include <sys/time.h>
#include "logger.h"

static const char* TAG = "net";

static const uint16_t NET_PORT = 4210;

String   MASTER_MAC;
uint64_t NET_KEY = 0;  // shared HMAC key (from network_key); authenticates every packet, never transmitted


static WiFiUDP UDP_SOCKET;
static SemaphoreHandle_t UDP_SOCKET_MUTEX;  // serializes all UDP_SOCKET access across tasks

// ── Node map (master only) ────────────────────────────────────────────────────

static SemaphoreHandle_t NODE_MAP_MUTEX;
static std::map<String, NodeInfo> NODE_MAP;  // keyed by MAC

// Heartbeat packet body after the 10-byte [type][timestamp] header, before the trailing HMAC.
struct __attribute__((packed)) HeartbeatBody {
  uint32_t buildNumber;  // FIRMWARE_BUILD — used for version comparison
  uint32_t uptimeSec;
  uint32_t freeHeap;
  int8_t   rssi;
  char     version[16];  // null-terminated display string
  char     ip[16];       // null-terminated
  char     mac[18];      // null-terminated, e.g. "AA:BB:CC:DD:EE:FF"
};

// OTA command body after the 10-byte [type][timestamp] header, before the trailing HMAC.
struct __attribute__((packed)) OtaCmdBody {
  char targetMac[18];  // null-terminated — slave only acts if this matches its own MAC
};

// Coin-inserted body after the 10-byte [type][timestamp] header, before the trailing HMAC.
struct __attribute__((packed)) CoinInsertedBody {
  char mac[18];  // null-terminated sender MAC, e.g. "AA:BB:CC:DD:EE:FF"
};

// Log-entry body after the 10-byte [type][timestamp] header, before the trailing HMAC.
struct __attribute__((packed)) LogEntryBody {
  char level;    // 'W' or 'E'
  char tag[16];  // null-terminated
  char msg[128]; // null-terminated
  char mac[18];  // null-terminated sender MAC
};

// ── Authenticated wire format ─────────────────────────────────────────────────
// Every packet is [type][timestamp_ms][body][16-byte HMAC]. The HMAC, keyed by the
// never-transmitted network_key, authenticates the message; the timestamp gives replay
// freshness. (The old scheme mailed the key in the clear, so it proved nothing.)

static constexpr size_t   NET_HMAC_LEN        = 16;  // truncated HMAC-SHA256 tag
static constexpr size_t   NET_HEADER_LEN      = sizeof(NetMsgType) + sizeof(uint64_t);  // [type][timestamp]
static constexpr size_t   NET_MAX_PACKET      = NET_HEADER_LEN + sizeof(LogEntryBody) + NET_HMAC_LEN;
static constexpr uint64_t NET_FRESH_WINDOW_MS = 5000;  // reject messages this far off our clock

// Epoch milliseconds (NTP-synced). Finer than time(NULL)*1000 so two coins <1s apart differ.
static uint64_t netNowMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

// HMAC-SHA256(network_key, data), truncated to NET_HMAC_LEN.
static void netHmac(const uint8_t* data, size_t len, uint8_t out[NET_HMAC_LEN]) {
  uint8_t full[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                  (const uint8_t*)&NET_KEY, sizeof(NET_KEY), data, len, full);
  memcpy(out, full, NET_HMAC_LEN);
}

// Constant-time tag compare — don't leak how many leading bytes matched.
static bool netTagEqual(const uint8_t* a, const uint8_t* b) {
  uint8_t diff = 0;
  for (size_t i = 0; i < NET_HMAC_LEN; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

// Build [type][timestamp][body][HMAC] and send it under the socket mutex.
static void netTransmit(IPAddress dest, NetMsgType type, const void* body, size_t bodyLen) {
  if (bodyLen > sizeof(LogEntryBody)) return;  // guards buf; never happens with our bodies
  uint8_t buf[NET_MAX_PACKET];
  size_t  off = 0;
  memcpy(buf + off, &type, sizeof(type)); off += sizeof(type);
  uint64_t ts = netNowMs();
  memcpy(buf + off, &ts, sizeof(ts));     off += sizeof(ts);
  if (body && bodyLen) { memcpy(buf + off, body, bodyLen); off += bodyLen; }
  netHmac(buf, off, buf + off);           off += NET_HMAC_LEN;

  xSemaphoreTake(UDP_SOCKET_MUTEX, portMAX_DELAY);
  UDP_SOCKET.beginPacket(dest, NET_PORT);
  UDP_SOCKET.write(buf, off);
  UDP_SOCKET.endPacket();
  xSemaphoreGive(UDP_SOCKET_MUTEX);
}

static void netBroadcast(NetMsgType type, const void* body, size_t bodyLen) {
  netTransmit(IPAddress(255, 255, 255, 255), type, body, bodyLen);
}

// Replay guard for coin inserts: reject a (sender, timestamp) we've already credited.
// Touched only by networkLoop (one task), so no lock is needed. The ring must outlast the
// freshness window at the real coin rate so a within-window replay still finds its match;
// 64 entries covers minutes of human-paced inserts (replays > NET_FRESH_WINDOW_MS are already
// dropped as stale). Distinct coins have distinct ms timestamps, so reordering is harmless.
static constexpr int COIN_SEEN_N = 64;
static struct { char mac[18]; uint64_t ts; } COIN_SEEN[COIN_SEEN_N] = {};
static int COIN_SEEN_IDX = 0;
static bool coinIsReplay(const char* mac, uint64_t ts) {
  for (auto& c : COIN_SEEN)
    if (c.ts == ts && strncmp(c.mac, mac, sizeof(c.mac)) == 0) return true;
  strncpy(COIN_SEEN[COIN_SEEN_IDX].mac, mac, sizeof(COIN_SEEN[0].mac) - 1);
  COIN_SEEN[COIN_SEEN_IDX].mac[sizeof(COIN_SEEN[0].mac) - 1] = '\0';
  COIN_SEEN[COIN_SEEN_IDX].ts = ts;
  COIN_SEEN_IDX = (COIN_SEEN_IDX + 1) % COIN_SEEN_N;
  return false;
}

// ── Slave OTA state ───────────────────────────────────────────────────────────

static volatile bool OTA_IN_PROGRESS = false;  // exactly one slaveOtaTask at a time; the worker clears it on exit

// ── Role ──────────────────────────────────────────────────────────────────────

bool isMaster() {
  String own = WiFi.macAddress();
  String cfg = MASTER_MAC;
  own.toUpperCase();
  cfg.toUpperCase();
  return !cfg.isEmpty() && own == cfg;
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setupNetwork() {
  NODE_MAP_MUTEX   = xSemaphoreCreateMutex();
  UDP_SOCKET_MUTEX = xSemaphoreCreateMutex();
  UDP_SOCKET.begin(NET_PORT);
  ESP_LOGI(TAG, "UDP listening on port %d — role: %s (own MAC: %s) build=%u",
           NET_PORT, isMaster() ? "MASTER" : "SLAVE",
           WiFi.macAddress().c_str(), (unsigned)FIRMWARE_BUILD);

  if (!isMaster()) {
    setLogForwardCallback([](char level, const char* tag, const char* msg) {
      LogEntryBody body = {};
      body.level = level;
      strncpy(body.tag, tag, sizeof(body.tag) - 1);
      strncpy(body.msg, msg, sizeof(body.msg) - 1);
      WiFi.macAddress().toCharArray(body.mac, sizeof(body.mac));
      netBroadcast(NET_MSG_LOG_ENTRY, &body, sizeof(body));
    });
  }
}

// ── Heartbeat ─────────────────────────────────────────────────────────────────

void broadcastHeartbeat() {
  ESP_LOGI(TAG, "heartbeat tx: build=%u mac=%s ip=%s",
           (unsigned)FIRMWARE_BUILD, WiFi.macAddress().c_str(), WiFi.localIP().toString().c_str());
  HeartbeatBody body = {};
  body.buildNumber = FIRMWARE_BUILD;
  body.uptimeSec   = millis() / 1000;
  body.freeHeap    = ESP.getFreeHeap();
  body.rssi        = (int8_t)WiFi.RSSI();
  strncpy(body.version, FIRMWARE_VERSION, sizeof(body.version) - 1);
  WiFi.localIP().toString().toCharArray(body.ip,  sizeof(body.ip));
  WiFi.macAddress().toCharArray(       body.mac, sizeof(body.mac));

  netBroadcast(NET_MSG_HEARTBEAT, &body, sizeof(body));
}

std::vector<NodeInfo> getKnownNodes() {
  std::vector<NodeInfo> result;
  if (xSemaphoreTake(NODE_MAP_MUTEX, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (auto& kv : NODE_MAP) result.push_back(kv.second);
    xSemaphoreGive(NODE_MAP_MUTEX);
  }
  return result;
}

// ── OTA forwarding ────────────────────────────────────────────────────────────

void masterSendOtaToNode(const String& targetMac, IPAddress targetIp) {
  OtaCmdBody body = {};
  strncpy(body.targetMac, targetMac.c_str(), sizeof(body.targetMac) - 1);

  // unicast — AP broadcast filtering blocks 255.255.255.255
  netTransmit(targetIp, NET_MSG_OTA_AVAILABLE, &body, sizeof(body));
  ESP_LOGI(TAG, "OTA send → %s (%s)", targetMac.c_str(), targetIp.toString().c_str());
}

// Stream a URL into Update, using the given update type (U_FLASH or U_SPIFFS).
// Returns true on success. Closes the HTTP connection.
static bool slaveFlashFromUrl(const String& url, int updateType) {
  HTTPClient http;
  http.begin(url);
  http.setTimeout(60000);
  int code = http.GET();
  if (code == 404) { http.end(); return true; }  // not available — skip, not an error
  if (code != 200) {
    ESP_LOGE(TAG, "Slave OTA: GET %s → HTTP %d", url.c_str(), code);
    http.end();
    return false;
  }
  int totalLen = http.getSize();
  if (totalLen <= 0) {
    ESP_LOGE(TAG, "Slave OTA: %s — unknown content length", url.c_str());
    http.end();
    return false;
  }
  ESP_LOGI(TAG, "Slave OTA: flashing %s (%d bytes, type=%d)", url.c_str(), totalLen, updateType);
  if (!Update.begin(totalLen, updateType)) {
    ESP_LOGE(TAG, "Slave OTA: Update.begin failed: %s", Update.errorString());
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  // 8 KB reads (vs 512 B) drain the TCP receive window far faster — the small buffer was
  // throttling throughput to a crawl. Heap, not stack, to keep the task stack small.
  const size_t BUF_SIZE = 8192;
  uint8_t* buf = (uint8_t*)malloc(BUF_SIZE);
  if (!buf) {
    ESP_LOGE(TAG, "Slave OTA: %u-byte buffer alloc failed", (unsigned)BUF_SIZE);
    Update.abort();
    http.end();
    return false;
  }
  int remaining = totalLen;
  unsigned long lastProgress = millis();
  while (http.connected() && remaining > 0) {
    int avail = stream->available();
    if (avail > 0) {
      int n = stream->readBytes(buf, min(avail, (int)BUF_SIZE));
      if (n > 0) { Update.write(buf, n); remaining -= n; }
      lastProgress = millis();
    } else {
      if (millis() - lastProgress > 5000) {
        ESP_LOGE(TAG, "Slave OTA: stalled — no data for 5s, %d bytes remaining", remaining);
        break;
      }
      delay(1);
    }
  }
  free(buf);
  http.end();
  if (remaining > 0) {
    ESP_LOGE(TAG, "Slave OTA: incomplete — %d of %d bytes written", totalLen - remaining, totalLen);
    Update.abort();  // release the Update session, or the next retry's Update.begin() fails
    return false;
  }
  if (!Update.end(true)) {
    ESP_LOGE(TAG, "Slave OTA: Update.end failed: %s", Update.errorString());
    return false;
  }
  ESP_LOGI(TAG, "Slave OTA: %s written OK (%d bytes)", url.c_str(), totalLen);
  return true;
}

// FreeRTOS task: fetch firmware from the master and apply OTA.
static void slaveOtaTask(void* arg) {
  uint8_t* ipBytes = (uint8_t*)arg;
  IPAddress masterIp(ipBytes[0], ipBytes[1], ipBytes[2], ipBytes[3]);
  free(ipBytes);

  String base = "http://" + masterIp.toString();

  if (!slaveFlashFromUrl(base + "/fw.bin", U_FLASH)) {
    ESP_LOGE(TAG, "Slave OTA: firmware flash failed — aborting");
    OTA_IN_PROGRESS = false;
    vTaskDelete(NULL);
    return;
  }


  ESP_LOGI(TAG, "Slave OTA: complete — deauthing then rebooting");
  WiFi.disconnect(true);  // deauth so AP drops association; prevents UDP-deaf on reconnect
  delay(500);
  esp_restart();
  vTaskDelete(NULL);
}

// ── Wire format helpers ───────────────────────────────────────────────────────

static void netSend(NetMsgType type) {
  netBroadcast(type, nullptr, 0);
}

void masterBroadcastSessionStart() { netSend(NET_MSG_SESSION_START); ESP_LOGI(TAG, "UDP broadcast session_start"); }
void masterBroadcastSessionEnd()   { netSend(NET_MSG_SESSION_END);   ESP_LOGI(TAG, "UDP broadcast session_end"); }

void slaveSendCoinInserted() {
  CoinInsertedBody body = {};
  WiFi.macAddress().toCharArray(body.mac, sizeof(body.mac));
  netBroadcast(NET_MSG_COIN_INSERTED, &body, sizeof(body));
  ESP_LOGI(TAG, "UDP coin_inserted sent from %s", body.mac);
}


// ── Main receive loop ─────────────────────────────────────────────────────────

void networkLoop() {
  // Periodic WiFi health — reconnects if the driver stalls after AUTH_LEAVE/ASSOC_LEAVE
  static unsigned long lastWifiDiag = 0;
  if (millis() - lastWifiDiag > 30000) {
    lastWifiDiag = millis();
    wl_status_t wifiStatus = WiFi.status();
    ESP_LOGD(TAG, "wifi [self]: status=%d ip=%s rssi=%d",
        (int)wifiStatus, WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    if (wifiStatus != WL_CONNECTED) {
      ESP_LOGW(TAG, "WiFi not connected (status=%d) — calling reconnect", (int)wifiStatus);
      WiFi.reconnect();
    }
  }

  // Read one whole datagram under the socket mutex. parsePacket + remoteIP + read must be
  // atomic w.r.t. concurrent sends, or remoteIP() could return a send's destination.
  uint8_t   rxBuf[NET_MAX_PACKET];
  IPAddress senderIp;
  int       size = 0, got = 0;
  xSemaphoreTake(UDP_SOCKET_MUTEX, portMAX_DELAY);
  size = UDP_SOCKET.parsePacket();
  if (size > 0) {
    senderIp = UDP_SOCKET.remoteIP();
    if (size <= (int)sizeof(rxBuf)) got = UDP_SOCKET.read(rxBuf, size);
    else while (UDP_SOCKET.available()) UDP_SOCKET.read();  // oversized/foreign — drain it
  }
  xSemaphoreGive(UDP_SOCKET_MUTEX);
  if (size <= 0) return;
  if (size < (int)(NET_HEADER_LEN + NET_HMAC_LEN) || got != size) {
    ESP_LOGW(TAG, "UDP bad packet: %d bytes from %s", size, senderIp.toString().c_str());
    return;
  }

  // Authenticate: HMAC over everything but the trailing tag. Rejects forged + foreign packets.
  uint8_t tag[NET_HMAC_LEN];
  netHmac(rxBuf, size - NET_HMAC_LEN, tag);
  if (!netTagEqual(tag, rxBuf + size - NET_HMAC_LEN)) {
    ESP_LOGI(TAG, "UDP packet from %s rejected: bad HMAC", senderIp.toString().c_str());
    return;
  }
  // Freshness: reject stale/replayed (or wildly clock-skewed) messages.
  uint64_t msgTs; memcpy(&msgTs, rxBuf + sizeof(NetMsgType), sizeof(msgTs));
  uint64_t now  = netNowMs();
  uint64_t skew = (now > msgTs) ? now - msgTs : msgTs - now;
  if (skew > NET_FRESH_WINDOW_MS) {
    ESP_LOGI(TAG, "UDP packet from %s rejected: stale by %llums", senderIp.toString().c_str(), (unsigned long long)skew);
    return;
  }

  NetMsgType type; memcpy(&type, rxBuf, sizeof(type));
  const uint8_t* bodyPtr  = rxBuf + NET_HEADER_LEN;
  const int      bodySize = size - (int)NET_HEADER_LEN - (int)NET_HMAC_LEN;

  if (type == NET_MSG_HEARTBEAT) {
    if (!IS_MASTER || bodySize < (int)sizeof(HeartbeatBody)) return;
    HeartbeatBody body;
    memcpy(&body, bodyPtr, sizeof(body));
    body.version[sizeof(body.version) - 1] = '\0';
    body.ip[sizeof(body.ip)       - 1] = '\0';
    body.mac[sizeof(body.mac)     - 1] = '\0';

    String mac = String(body.mac);
    mac.toUpperCase();

    String ownMac = WiFi.macAddress();
    ownMac.toUpperCase();
    if (mac == ownMac) return;  // ignore our own broadcast

    if (xSemaphoreTake(NODE_MAP_MUTEX, pdMS_TO_TICKS(10)) == pdTRUE) {
      NodeInfo& entry  = NODE_MAP[mac];  // creates with defaults if new
      entry.mac         = mac;
      entry.ip          = String(body.ip);
      entry.version     = String(body.version);
      entry.buildNumber = body.buildNumber;
      entry.uptimeSec   = body.uptimeSec;
      entry.freeHeap    = body.freeHeap;
      entry.rssi        = body.rssi;
      entry.lastSeenMs  = millis();
      // otaSentCount and lastOtaSentMs are preserved across heartbeats
      xSemaphoreGive(NODE_MAP_MUTEX);
    }

    // Trigger OTA if the slave is on an older build. We serve our own running image, whose true
    // size we captured at boot — so this works regardless of how we were flashed.
    ESP_LOGD(TAG, "HB from %s build=%u [mine=%u] fw=%u", mac.c_str(), body.buildNumber, (unsigned)FIRMWARE_BUILD, (unsigned)RUNNING_IMAGE_SIZE);
    if (body.buildNumber < FIRMWARE_BUILD) {  // (mac != ownMac already returned above)
      ESP_LOGI(TAG, "OTA trigger: %s build=%u < mine=%u", mac.c_str(), body.buildNumber, (unsigned)FIRMWARE_BUILD);
      masterSendOtaToNode(mac, senderIp);
      if (xSemaphoreTake(NODE_MAP_MUTEX, pdMS_TO_TICKS(10)) == pdTRUE) {
        NODE_MAP[mac].otaSentCount++;
        NODE_MAP[mac].lastOtaSentMs = millis();
        xSemaphoreGive(NODE_MAP_MUTEX);
      }
    }

  } else if (type == NET_MSG_OTA_AVAILABLE) {
    if (IS_MASTER || bodySize < (int)sizeof(OtaCmdBody)) return;
    OtaCmdBody body;
    memcpy(&body, bodyPtr, sizeof(body));
    ESP_LOGI(TAG, "OTA_AVAILABLE packet received from %s", senderIp.toString().c_str());
    // Exactly one OTA worker at a time. Spawning a second slaveOtaTask alongside a live one
    // would run a concurrent Update on the same partition — guaranteed corruption (and the
    // reason a slow transfer used to loop forever). slaveFlashFromUrl self-bounds (HTTP
    // timeout + stall guard) and clears OTA_IN_PROGRESS on exit, so a failed attempt retries
    // cleanly on the next heartbeat without a respawn timer.
    if (OTA_IN_PROGRESS) {
      ESP_LOGI(TAG, "OTA already in progress — ignoring");
      return;
    }
    body.targetMac[sizeof(body.targetMac) - 1] = '\0';

    String target = String(body.targetMac);
    String ownMac = WiFi.macAddress();
    target.toUpperCase();
    ownMac.toUpperCase();

    ESP_LOGI(TAG, "OTA_AVAILABLE: target=%s own=%s", target.c_str(), ownMac.c_str());
    if (target != ownMac) return;  // not for us

    uint8_t* ipBytes = (uint8_t*)malloc(4);
    if (!ipBytes) return;
    ipBytes[0] = senderIp[0]; ipBytes[1] = senderIp[1];
    ipBytes[2] = senderIp[2]; ipBytes[3] = senderIp[3];

    OTA_IN_PROGRESS = true;
    ESP_LOGI(TAG, "OTA_AVAILABLE for us — fetching from %s", senderIp.toString().c_str());
    if (xTaskCreate(slaveOtaTask, "SlaveOTA", 16384, ipBytes, 5, NULL) != pdPASS) {
      ESP_LOGE(TAG, "OTA: xTaskCreate failed — will retry");
      free(ipBytes);
      OTA_IN_PROGRESS = false;
    }

  } else if (type == NET_MSG_LOG_ENTRY) {
    if (!IS_MASTER || bodySize < (int)sizeof(LogEntryBody)) return;
    LogEntryBody body;
    memcpy(&body, bodyPtr, sizeof(body));
    body.tag[sizeof(body.tag) - 1] = '\0';
    body.msg[sizeof(body.msg) - 1] = '\0';
    body.mac[sizeof(body.mac) - 1] = '\0';
    char msgBuf[160];
    snprintf(msgBuf, sizeof(msgBuf), "[%s] %s", body.mac, body.msg);
    logDirect(body.level, body.tag, msgBuf);
    if (body.level == 'W' || body.level == 'E') appendErrorLog(body.tag, msgBuf);

  } else if (IS_MASTER) {
    if (type == NET_MSG_COIN_INSERTED) {
      if (bodySize < (int)sizeof(CoinInsertedBody)) return;
      CoinInsertedBody body;
      memcpy(&body, bodyPtr, sizeof(body));
      body.mac[sizeof(body.mac) - 1] = '\0';
      if (coinIsReplay(body.mac, msgTs)) {
        ESP_LOGW(TAG, "UDP coin_inserted from %s rejected: replay", body.mac);
        return;
      }
      ESP_LOGI(TAG, "UDP coin_inserted from %s (mac=%s)", senderIp.toString().c_str(), body.mac);
      masterRecvCoinInserted(body.mac);
    }
  } else {
    if (type == NET_MSG_SESSION_START) {
      ESP_LOGI(TAG, "UDP session_start");
      slaveRecvCoinInsertStart();
    } else if (type == NET_MSG_SESSION_END) {
      ESP_LOGI(TAG, "UDP session_end");
      slaveRecvCoinInsertEnd();
    }
  }
}
