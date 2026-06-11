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
#include "logger.h"

static const char* TAG = "net";

static const uint16_t NET_PORT = 4210;

String   MASTER_MAC;
uint64_t NET_PSK = 0;


static WiFiUDP UDP_SOCKET;

// ── Node map (master only) ────────────────────────────────────────────────────

static SemaphoreHandle_t NODE_MAP_MUTEX;
static std::map<String, NodeInfo> NODE_MAP;  // keyed by MAC

// Heartbeat packet body after the 10-byte [type][psk] header.
struct __attribute__((packed)) HeartbeatBody {
  uint32_t buildNumber;  // FIRMWARE_BUILD — used for version comparison
  uint32_t uptimeSec;
  uint32_t freeHeap;
  int8_t   rssi;
  char     version[16];  // null-terminated display string
  char     ip[16];       // null-terminated
  char     mac[18];      // null-terminated, e.g. "AA:BB:CC:DD:EE:FF"
};

// OTA command body after the 10-byte [type][psk] header.
struct __attribute__((packed)) OtaCmdBody {
  char targetMac[18];  // null-terminated — slave only acts if this matches its own MAC
};

// Coin-inserted body after the 10-byte [type][psk] header.
struct __attribute__((packed)) CoinInsertedBody {
  char mac[18];  // null-terminated sender MAC, e.g. "AA:BB:CC:DD:EE:FF"
};

// Log-entry body after the 10-byte [type][psk] header.
struct __attribute__((packed)) LogEntryBody {
  char level;    // 'W' or 'E'
  char tag[16];  // null-terminated
  char msg[128]; // null-terminated
  char mac[18];  // null-terminated sender MAC
};

// ── Slave OTA state ───────────────────────────────────────────────────────────

static volatile bool     OTA_IN_PROGRESS    = false;
static volatile uint32_t OTA_START_MILLIS       = 0;
static constexpr uint32_t OTA_TIMEOUT_MILLIS    = 120000;  // 2 min max for fetch+flash

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
  NODE_MAP_MUTEX = xSemaphoreCreateMutex();
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
      NetMsgType type = NET_MSG_LOG_ENTRY;
      UDP_SOCKET.beginPacket(IPAddress(255, 255, 255, 255), NET_PORT);
      UDP_SOCKET.write((const uint8_t*)&type,    sizeof(type));
      UDP_SOCKET.write((const uint8_t*)&NET_PSK, sizeof(NET_PSK));
      UDP_SOCKET.write((const uint8_t*)&body,    sizeof(body));
      UDP_SOCKET.endPacket();
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

  NetMsgType type = NET_MSG_HEARTBEAT;
  UDP_SOCKET.beginPacket(IPAddress(255, 255, 255, 255), NET_PORT);
  UDP_SOCKET.write((const uint8_t*)&type,    sizeof(type));
  UDP_SOCKET.write((const uint8_t*)&NET_PSK, sizeof(NET_PSK));
  UDP_SOCKET.write((const uint8_t*)&body,    sizeof(body));
  UDP_SOCKET.endPacket();
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

  NetMsgType type = NET_MSG_OTA_AVAILABLE;
  UDP_SOCKET.beginPacket(targetIp, NET_PORT);  // unicast — AP broadcast filtering blocks 255.255.255.255
  UDP_SOCKET.write((const uint8_t*)&type,    sizeof(type));
  UDP_SOCKET.write((const uint8_t*)&NET_PSK, sizeof(NET_PSK));
  UDP_SOCKET.write((const uint8_t*)&body,    sizeof(body));
  UDP_SOCKET.endPacket();
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
  uint8_t buf[512];
  int remaining = totalLen;
  unsigned long lastProgress = millis();
  while (http.connected() && remaining > 0) {
    int avail = stream->available();
    if (avail > 0) {
      int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
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
  http.end();
  if (remaining > 0) {
    ESP_LOGE(TAG, "Slave OTA: incomplete — %d of %d bytes written", totalLen - remaining, totalLen);
    return false;
  }
  if (!Update.end(true)) {
    ESP_LOGE(TAG, "Slave OTA: Update.end failed: %s", Update.errorString());
    return false;
  }
  ESP_LOGI(TAG, "Slave OTA: %s written OK (%d bytes)", url.c_str(), totalLen);
  return true;
}

// FreeRTOS task: fetch firmware (and filesystem if available) from master and apply OTA.
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
  UDP_SOCKET.beginPacket(IPAddress(255, 255, 255, 255), NET_PORT);
  UDP_SOCKET.write((const uint8_t*)&type,    sizeof(type));
  UDP_SOCKET.write((const uint8_t*)&NET_PSK, sizeof(NET_PSK));
  UDP_SOCKET.endPacket();
}

void masterBroadcastSessionStart() { netSend(NET_MSG_SESSION_START); ESP_LOGI(TAG, "UDP broadcast session_start"); }
void masterBroadcastSessionEnd()   { netSend(NET_MSG_SESSION_END);   ESP_LOGI(TAG, "UDP broadcast session_end"); }

void slaveSendCoinInserted() {
  CoinInsertedBody body = {};
  WiFi.macAddress().toCharArray(body.mac, sizeof(body.mac));
  NetMsgType type = NET_MSG_COIN_INSERTED;
  UDP_SOCKET.beginPacket(IPAddress(255, 255, 255, 255), NET_PORT);
  UDP_SOCKET.write((const uint8_t*)&type,    sizeof(type));
  UDP_SOCKET.write((const uint8_t*)&NET_PSK, sizeof(NET_PSK));
  UDP_SOCKET.write((const uint8_t*)&body,    sizeof(body));
  UDP_SOCKET.endPacket();
  ESP_LOGI(TAG, "UDP coin_inserted sent from %s", body.mac);
}


// ── Main receive loop ─────────────────────────────────────────────────────────

void networkLoop() {
  // Stale bytes left from a partial read would cause parsePacket() to return 0 forever.
  // Log and flush so we can catch any future handler that forgets to consume its packet.
  {
    int stale = UDP_SOCKET.available();
    if (stale > 0) {
      ESP_LOGW(TAG, "%d stale bytes before parsePacket — unread from previous packet", stale);
      while (UDP_SOCKET.available()) UDP_SOCKET.read();
    }
  }

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

  int size = UDP_SOCKET.parsePacket();
  if (size == 0) return;
  if (size < (int)(sizeof(NetMsgType) + sizeof(uint64_t))) {
    ESP_LOGW(TAG, "UDP short packet: %d bytes from %s", size, UDP_SOCKET.remoteIP().toString().c_str());
    return;
  }

  NetMsgType type;
  uint64_t   psk;
  UDP_SOCKET.read((uint8_t*)&type, sizeof(type));
  UDP_SOCKET.read((uint8_t*)&psk,  sizeof(psk));

  if (psk != NET_PSK) {
    ESP_LOGI(TAG, "UDP packet from %s rejected: bad PSK", UDP_SOCKET.remoteIP().toString().c_str());  // foreign packet on our port — not our error
    return;
  }

  if (type == NET_MSG_HEARTBEAT) {
    int bodySize = size - sizeof(NetMsgType) - sizeof(uint64_t);
    if (bodySize < (int)sizeof(HeartbeatBody)) return;

    IPAddress senderIp = UDP_SOCKET.remoteIP();  // capture before any further reads
    HeartbeatBody body;
    UDP_SOCKET.read((uint8_t*)&body, sizeof(body));  // always read before any early return
    if (!isMaster()) return;
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

    // Trigger OTA if slave is on an older build and we have firmware ready
    Preferences prefs;
    prefs.begin("ellafi", false);
    uint32_t fw_size = prefs.getUInt("fw_size", 0);
    prefs.end();
    ESP_LOGD(TAG, "HB from %s build=%u [mine=%u] fw=%u", mac.c_str(), body.buildNumber, (unsigned)FIRMWARE_BUILD, (unsigned)fw_size);
    if (mac != ownMac && body.buildNumber < FIRMWARE_BUILD && fw_size > 0) {
      ESP_LOGI(TAG, "OTA trigger: %s build=%u < mine=%u", mac.c_str(), body.buildNumber, (unsigned)FIRMWARE_BUILD);
      masterSendOtaToNode(mac, senderIp);
      if (xSemaphoreTake(NODE_MAP_MUTEX, pdMS_TO_TICKS(10)) == pdTRUE) {
        NODE_MAP[mac].otaSentCount++;
        NODE_MAP[mac].lastOtaSentMs = millis();
        xSemaphoreGive(NODE_MAP_MUTEX);
      }
    } else if (mac != ownMac && body.buildNumber < FIRMWARE_BUILD && fw_size == 0) {
      ESP_LOGI(TAG, "Slave %s needs update but fw_size=0 in Preferences — deploy firmware first", mac.c_str());
    }

  } else if (type == NET_MSG_OTA_AVAILABLE) {
    int bodySize = size - sizeof(NetMsgType) - sizeof(uint64_t);
    if (bodySize < (int)sizeof(OtaCmdBody)) return;

    OtaCmdBody body;
    UDP_SOCKET.read((uint8_t*)&body, sizeof(body));  // always read before any early return

    if (isMaster()) return;
    ESP_LOGI(TAG, "OTA_AVAILABLE packet received from %s", UDP_SOCKET.remoteIP().toString().c_str());
    if (OTA_IN_PROGRESS) {
      if (millis() - OTA_START_MILLIS < OTA_TIMEOUT_MILLIS) {
        ESP_LOGI(TAG, "OTA already in progress — ignoring");
        return;
      }
      ESP_LOGW(TAG, "OTA timed out after %us — retrying", OTA_TIMEOUT_MILLIS / 1000);
      OTA_IN_PROGRESS = false;
    }
    body.targetMac[sizeof(body.targetMac) - 1] = '\0';

    String target = String(body.targetMac);
    String ownMac = WiFi.macAddress();
    target.toUpperCase();
    ownMac.toUpperCase();

    ESP_LOGI(TAG, "OTA_AVAILABLE: target=%s own=%s", target.c_str(), ownMac.c_str());
    if (target != ownMac) return;  // not for us

    // Capture master IP before any other UDP call
    IPAddress masterIp = UDP_SOCKET.remoteIP();
    uint8_t* ipBytes = (uint8_t*)malloc(4);
    if (!ipBytes) return;
    ipBytes[0] = masterIp[0]; ipBytes[1] = masterIp[1];
    ipBytes[2] = masterIp[2]; ipBytes[3] = masterIp[3];

    OTA_IN_PROGRESS = true;
    OTA_START_MILLIS    = millis();
    ESP_LOGI(TAG, "OTA_AVAILABLE for us — fetching from %s", masterIp.toString().c_str());
    if (xTaskCreate(slaveOtaTask, "SlaveOTA", 16384, ipBytes, 5, NULL) != pdPASS) {
      ESP_LOGE(TAG, "OTA: xTaskCreate failed — will retry");
      free(ipBytes);
      OTA_IN_PROGRESS = false;
    }

  } else if (type == NET_MSG_LOG_ENTRY) {
    int bodySize = size - sizeof(NetMsgType) - sizeof(uint64_t);
    if (bodySize < (int)sizeof(LogEntryBody)) return;
    LogEntryBody body;
    UDP_SOCKET.read((uint8_t*)&body, sizeof(body));
    if (!isMaster()) return;
    body.tag[sizeof(body.tag) - 1] = '\0';
    body.msg[sizeof(body.msg) - 1] = '\0';
    body.mac[sizeof(body.mac) - 1] = '\0';
    char msgBuf[160];
    snprintf(msgBuf, sizeof(msgBuf), "[%s] %s", body.mac, body.msg);
    logDirect(body.level, body.tag, msgBuf);
    if (body.level == 'W' || body.level == 'E') appendErrorLog(body.tag, msgBuf);

  } else if (isMaster()) {
    if (type == NET_MSG_COIN_INSERTED) {
      CoinInsertedBody body = {};
      int bodySize = size - sizeof(NetMsgType) - sizeof(uint64_t);
      if (bodySize >= (int)sizeof(CoinInsertedBody))
        UDP_SOCKET.read((uint8_t*)&body, sizeof(body));
      body.mac[sizeof(body.mac) - 1] = '\0';
      ESP_LOGI(TAG, "UDP coin_inserted from %s (mac=%s)", UDP_SOCKET.remoteIP().toString().c_str(), body.mac);
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
