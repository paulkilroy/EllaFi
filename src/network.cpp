#include "network.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_log.h>

static const char* TAG = "net";

// UDP port used by all nodes — master and slaves all listen on this port.
// All messages are broadcast so every node receives every packet.
static const uint16_t NET_PORT = 4210;

String MASTER_MAC;
uint64_t NET_PSK = 0;

static WiFiUDP udp;

// Defined in main.cpp
extern void remoteCoinPulse();
extern void slaveCoinInsertStart();
extern void slaveCoinInsertEnd();

bool isMaster() {
  String own = WiFi.macAddress();
  String cfg = MASTER_MAC;
  own.toUpperCase();
  cfg.toUpperCase();
  return !cfg.isEmpty() && own == cfg;
}

void networkSetup() {
  udp.begin(NET_PORT);
  ESP_LOGI(TAG, "UDP listening on port %d — role: %s (own MAC: %s)",
           NET_PORT, isMaster() ? "MASTER" : "SLAVE", WiFi.macAddress().c_str());
}

void networkLoop() {
  int size = udp.parsePacket();
  if (size < (int)(sizeof(NetMsgType) + sizeof(uint64_t))) return;

  NetMsgType type;
  uint64_t   psk;
  udp.read((uint8_t*)&type, sizeof(type));
  udp.read((uint8_t*)&psk,  sizeof(psk));

  if (psk != NET_PSK) {
    ESP_LOGW(TAG, "UDP packet from %s rejected: bad PSK", udp.remoteIP().toString().c_str());
    return;
  }

  if (isMaster()) {
    if (type == NET_MSG_COIN_INSERTED) {
      ESP_LOGI(TAG, "UDP coin_inserted from %s", udp.remoteIP().toString().c_str());
      remoteCoinPulse();
    }
  } else {
    if (type == NET_MSG_SESSION_START) {
      ESP_LOGI(TAG, "UDP session_start");
      slaveCoinInsertStart();
    } else if (type == NET_MSG_SESSION_END) {
      ESP_LOGI(TAG, "UDP session_end");
      slaveCoinInsertEnd();
    }
  }
}

// Wire format: [uint16_t type][uint16_t psk] — 4 bytes total
static void netSend(NetMsgType type) {
  udp.beginPacket(IPAddress(255, 255, 255, 255), NET_PORT);
  udp.write((const uint8_t*)&type,    sizeof(type));
  udp.write((const uint8_t*)&NET_PSK, sizeof(NET_PSK));
  udp.endPacket();
}

void netBroadcastSessionStart() {
  netSend(NET_MSG_SESSION_START);
  ESP_LOGI(TAG, "UDP broadcast session_start");
}

void netBroadcastSessionEnd() {
  netSend(NET_MSG_SESSION_END);
  ESP_LOGI(TAG, "UDP broadcast session_end");
}

void netSendCoinInserted() {
  netSend(NET_MSG_COIN_INSERTED);
  ESP_LOGI(TAG, "UDP coin_inserted sent");
}
