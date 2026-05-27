#pragma once

#include <Arduino.h>
#include <vector>

// UDP message types (uint16_t, 2 bytes on the wire)
enum NetMsgType : uint16_t {
  NET_MSG_COIN_INSERTED  = 1,
  NET_MSG_SESSION_START  = 2,
  NET_MSG_SESSION_END    = 3,
  NET_MSG_HEARTBEAT      = 4,  // all nodes broadcast periodically; master stores in NODE_MAP
  NET_MSG_OTA_AVAILABLE  = 5,  // master → slaves after OTA upload; payload = master IP (4 bytes)
};

// Snapshot of one node's health, populated from UDP heartbeats.
struct NodeInfo {
  String   mac;
  String   ip;
  String   version;
  uint32_t buildNumber      = 0;
  uint32_t uptimeSec        = 0;
  uint32_t freeHeap         = 0;
  int8_t   rssi             = 0;
  unsigned long lastSeenMs    = 0;
  uint32_t      otaSentCount  = 0;  // how many OTA_AVAILABLE packets master has sent to this node
  unsigned long lastOtaSentMs = 0;  // millis() when last OTA_AVAILABLE was sent (0 = never)
};

// Populated by loadConfig() in main.cpp
extern String   MASTER_MAC;
extern uint64_t NET_PSK;

bool isMaster();

// Call after WiFi is connected.
void networkSetup();

// Call from loop() — polls UDP socket and dispatches callbacks.
void networkLoop();

// Master → slaves: session lifecycle events.
void masterBroadcastSessionStart();
void masterBroadcastSessionEnd();

// Slave → master: report a coin pulse.
void slaveSendCoinInserted();

// All nodes: broadcast own health. Call from loop() on a timer.
void broadcastHeartbeat();

// Master: send targeted OTA command to one slave by MAC + unicast IP.
// Slave only acts on it if the MAC matches its own and no OTA is already in progress.
void masterSendOtaToNode(const String& targetMac, IPAddress targetIp);

// Returns a copy of all known nodes seen via heartbeat (master-only; thread-safe).
std::vector<NodeInfo> getKnownNodes();
