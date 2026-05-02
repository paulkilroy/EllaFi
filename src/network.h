#pragma once

#include <Arduino.h>

// UDP message types (uint16_t, 2 bytes on the wire)
enum NetMsgType : uint16_t {
  NET_MSG_COIN_INSERTED = 1,
  NET_MSG_SESSION_START = 2,
  NET_MSG_SESSION_END   = 3,
};

// Populated by loadConfig() in main.cpp
extern String MASTER_MAC;
extern uint64_t NET_PSK;

// Returns true if this device's WiFi MAC matches MASTER_MAC.
// Determines role at runtime — no separate firmware build needed.
bool isMaster();

// Call after WiFi is connected. Starts UDP listener.
void networkSetup();

// Call from loop() — polls UDP socket and dispatches callbacks.
void networkLoop();

// Master → slaves: broadcast that a coin session has started / ended.
void masterBroadcastSessionStart();
void masterBroadcastSessionEnd();

// Slave → master: report a coin pulse.
void slaveSendCoinInserted();
