#include "coin.h"
#include "files.h"

#include <WiFi.h>
#include <IPAddress.h>
#include <map>
#include <type_traits>
#include <esp_log.h>
#include "logger.h"

static const char* TAG = "EllaFi";

static const char* ERR_SUBTYPE_NO_SESSION = "no_session";
static const char* ERR_SUBTYPE_REFUNDABLE = "refundable";

// ISR-only — tracks falling edge time (micros) for pulse width measurement
static volatile unsigned long COINBUTTON_FALL_TIME = 0;
static volatile unsigned long COINSLOT_FALL_TIME   = 0;

// ISR-only — debounce timestamp for button (millis)
static volatile unsigned long LAST_COIN_PULSE_TIME = 0;

// ── ISRs ──────────────────────────────────────────────────────────────────────

// Triggers on CHANGE to measure pulse width — same pitik rejection as coin slot.
// Pitik spikes couple electromagnetically into GPIO0 (~100us); real press holds LOW for 50-200ms.
void IRAM_ATTR coinButtonPulse() {
  unsigned long now = micros();
  if (digitalRead(COINBUTTON_PIN) == LOW) {
    unsigned long nowMs = now / 1000;
    if (nowMs - LAST_COIN_PULSE_TIME < COIN_DEBOUNCE_MILLIS) return;
    LAST_COIN_PULSE_TIME = nowMs;
    COINBUTTON_FALL_TIME = now;
  } else {
    if (COINBUTTON_FALL_TIME > 0) {
      CoinPulse pulse = { PULSE_BUTTON, now - COINBUTTON_FALL_TIME, COINBUTTON_FALL_TIME };
      BaseType_t woken = pdFALSE;
      xQueueSendFromISR(COIN_PULSE_QUEUE, &pulse, &woken);
      portYIELD_FROM_ISR(woken);
    }
    COINBUTTON_FALL_TIME = 0;
  }
}

// Allan CH-926 pulse timing (fast mode, set via dip switch):
//   Fast:   20ms pulse + 100ms gap = 120ms min between pulses
//   Medium: 50ms pulse + 100ms gap = 150ms min between pulses
//   Slow:   70ms pulse + 100ms gap = 170ms min between pulses
//
// Anti-pitik (piezo lighter fraud) — filter by pulse WIDTH not just edge detection.
// Piezo spikes (10-20kV) take two paths into the ESP32:
//   GPIO4 (coin slot): the PC817 is NOT galvanically isolated here (LED anode ties to board 3V3, shared
//                      GND) — but it still band-limits the spike, attenuating it to ~5µs
//   GPIO0 (boot button): spike couples electromagnetically; no opto, ~100µs width
// A P6KE6.8A TVS diode before the PC817 clamps spikes to GND for hardware protection.
// Software rejects anything under COINSLOT_MIN_PULSE_WIDTH_MICROS (5ms) on both pins.
// Real coin pulse = 20ms; real button press = 50-200ms — both well above the threshold.
//
// Pulse phase diagram:
//
//   Real coin (20ms = 20,000µs) — ACCEPTED:
//
//     ISR fires (CHANGE=FALLING)        ISR fires (CHANGE=RISING)
//      │                                      │
//      │←────────────── 20,000µs ────────────→│
//   HI─┘                                      └─HI
//      │                                      │
//      COINSLOT_FALL_TIME = micros()     elapsed = now - COINSLOT_FALL_TIME = 20,000µs
//                                        CoinPulse{PULSE_COINSLOT, 20000, COINSLOT_FALL_TIME} → queue
//                                        task reads: 20,000µs >= 5,000µs → accepted
//                                        COINSLOT_FALL_TIME = 0
//
//   Piezo spike (~100µs) — REJECTED:
//
//     ISR fires (CHANGE=FALLING)  ISR fires (CHANGE=RISING)
//      │                               │
//      │←────── ~100µs ───────────────→│
//   HI─┘                               └─HI
//      │                               │
//      COINSLOT_FALL_TIME = micros()  elapsed = ~100µs
//                                       CoinPulse{PULSE_COINSLOT, 100, COINSLOT_FALL_TIME} → queue
//                                       task reads: 100µs < 5,000µs → rejected (pitik)
//                                       COINSLOT_FALL_TIME = 0
void IRAM_ATTR coinSlotPulse() {
  unsigned long now = micros();
  if (digitalRead(COINSLOT_PIN) == LOW) {
    COINSLOT_FALL_TIME = now;                                          // falling edge — start timer
  } else {
    if (COINSLOT_FALL_TIME > 0) {
      CoinPulse pulse = { PULSE_COINSLOT, now - COINSLOT_FALL_TIME, COINSLOT_FALL_TIME };
      BaseType_t woken = pdFALSE;
      xQueueSendFromISR(COIN_PULSE_QUEUE, &pulse, &woken);             // rising edge — send width + timestamp to task
      portYIELD_FROM_ISR(woken);
    }
    COINSLOT_FALL_TIME = 0;
  }
}

// ── Network event callbacks (wired by network.cpp) ───────────────────────────

void masterRecvCoinInserted(const char* senderMac) {
  CoinPulse pulse = {};
  pulse.source         = PULSE_NETWORK;
  pulse.timestampMicros = micros();
  strncpy(pulse.mac, senderMac, sizeof(pulse.mac) - 1);
  xQueueSend(COIN_PULSE_QUEUE, &pulse, 0);
}

void slaveRecvCoinInsertStart() {
  COIN_INSERT_ACTIVE.store(true);
  xTaskNotify(LED_TASK, LED_NOTIFY_START, eSetValueWithOverwrite);
  ESP_LOGI(TAG, "Session started — accepting coins");
}

void slaveRecvCoinInsertEnd() {
  COIN_INSERT_ACTIVE.store(false);
  xTaskNotify(LED_TASK, LED_NOTIFY_STOP, eSetValueWithOverwrite);
  ESP_LOGI(TAG, "Session ended — coin acceptance disabled");
}

// ── FreeRTOS tasks ────────────────────────────────────────────────────────────

// Post a fraud abort to the session owner. Called from coinPulseTask (master).
static void abortSessionForFraud(int fraudCount) {
  ESP_LOGE(TAG, "Fraud limit (%d) reached — ending session", fraudCount);
  CoinSessionEvent e(COIN_SESSION_FRAUD_END);
  xQueueSend(COIN_SESSION_QUEUE, &e, 0);
}

// Drains coin pulses from COIN_PULSE_QUEUE.
// Gated by SHOULD_ACCEPT_COINS — pulses that arrive outside the insertion window are discarded.
// Master: increments COIN_COUNT, resets the insertion timer, and broadcasts to slaves.
// Slave:  resets its own insertion timer and forwards the pulse to the master via UDP.
// Master: also notifies UPDATE_CLIENT_TASK for an immediate WS push (slaves have no web server).
void coinPulseTask(void*) {
  unsigned long prevAcceptedTimestampMicros = 0;
  int fraudCountThisSession = 0;
  for (;;) {
    CoinPulse pulse;
    xQueueReceive(COIN_PULSE_QUEUE, &pulse, portMAX_DELAY);

    // Outside insertion window — reset per-session state and discard
    if (!SHOULD_ACCEPT_COINS) {
      prevAcceptedTimestampMicros = 0;
      fraudCountThisSession = 0;
      continue;
    }

    // Boot pulse suppression — ignore pulses during acceptor startup window
    if (millis() < COIN_SLOT_READY_MILLIS) {
      ESP_LOGI(TAG, "Coin pulse suppressed - acceptor still booting");
      continue;
    }

    unsigned long intervalSinceLast = prevAcceptedTimestampMicros > 0 ? pulse.timestampMicros - prevAcceptedTimestampMicros : 0;
    ESP_LOGI(TAG, "coin pulse: src=%d width=%luus interval=%luus", pulse.source, pulse.widthMicros, intervalSinceLast);

    // Width filter — rejects piezo spikes at source on all nodes (PULSE_COINSLOT and PULSE_BUTTON)
    if (pulse.source == PULSE_COINSLOT || pulse.source == PULSE_BUTTON) {
      if (pulse.widthMicros < COINSLOT_MIN_PULSE_WIDTH_MICROS) {
        fraudCountThisSession++;
        ESP_LOGE(TAG, "Pulse width fraud #%d: width=%luus (min %luus)", fraudCountThisSession, pulse.widthMicros, COINSLOT_MIN_PULSE_WIDTH_MICROS);
        if (IS_MASTER && fraudCountThisSession >= COINSLOT_MAX_FRAUD_PER_SESSION) abortSessionForFraud(fraudCountThisSession);
        continue;
      }
    }

    // Interval fraud check at source on all nodes — consecutive pulse faster than hardware minimum (120ms fast mode)
    // Skipped for PULSE_NETWORK: two slaves could legitimately insert coins simultaneously
    if (pulse.source == PULSE_COINSLOT && prevAcceptedTimestampMicros > 0) {
      unsigned long interval = pulse.timestampMicros - prevAcceptedTimestampMicros;
      if (interval < COINSLOT_MIN_PULSE_INTERVAL_MILLIS * 1000UL) {
        fraudCountThisSession++;
        prevAcceptedTimestampMicros = 0;
        ESP_LOGE(TAG, "Interval fraud #%d: interval=%luus (min %lums)", fraudCountThisSession, interval, COINSLOT_MIN_PULSE_INTERVAL_MILLIS);
        if (IS_MASTER && fraudCountThisSession >= COINSLOT_MAX_FRAUD_PER_SESSION) abortSessionForFraud(fraudCountThisSession);
        continue;
      }
    }

    prevAcceptedTimestampMicros = pulse.timestampMicros;
    ESP_LOGI(TAG, "Coin pulse accepted - %s src=%d", IS_MASTER ? "master" : "slave", pulse.source);

    if (IS_MASTER) {
      // Determine which node the coin came from (empty mac = master's own slot)
      String nodeMac = pulse.mac[0] ? String(pulse.mac) : WiFi.macAddress();
      nodeMac.toUpperCase();
      CoinSessionEvent e(COIN_SESSION_COIN);
      strncpy(e.mac, nodeMac.c_str(), sizeof(e.mac) - 1);
      e.mac[sizeof(e.mac) - 1] = '\0';
      xQueueSend(COIN_SESSION_QUEUE, &e, 0);  // owner increments count, restarts deadline, pushes WS update
    } else {
      slaveSendCoinInserted();
    }
  }
}

// CoinSessionSlot must be trivially copyable for std::atomic. On Xtensa 8-byte atomics
// are lock-backed (not lock-free) — correct and fine at this cadence. Add -latomic to
// build_flags if the linker errors on __atomic_load_8 / __atomic_compare_exchange_8.
static_assert(std::is_trivially_copyable<CoinSessionSlot>::value, "CoinSessionSlot must be trivially copyable");

uint32_t coinIpToU32(const String& ip) {
  IPAddress a;
  return a.fromString(ip) ? (uint32_t)a : 0;
}
String coinU32ToIp(uint32_t v) {
  return IPAddress(v).toString();
}

bool coinSessionTryClaim(int socketFd, const String& clientIp) {
  bool expected = false;
  if (!COIN_INSERT_ACTIVE.compare_exchange_strong(expected, true)) return false;
  CoinSessionEvent e(socketFd, clientIp);
  if (xQueueSend(COIN_SESSION_QUEUE, &e, 0) != pdTRUE) {
    COIN_INSERT_ACTIVE.store(false);  // roll back the claim, else the slot is stuck active until reboot
    return false;
  }
  return true;
}

bool coinSessionUpdateSocket(const String& clientIp, int socketFd) {
  uint32_t ip = coinIpToU32(clientIp);
  CoinSessionSlot cur = COIN_SESSION_SLOT.load();
  for (;;) {
    if (cur.clientIpV4 != ip) return false;
    CoinSessionSlot next{ ip, (int32_t)socketFd };
    if (COIN_SESSION_SLOT.compare_exchange_weak(cur, next)) return true;
  }
}

CoinSessionStatus coinSessionSnapshot() {
  CoinSessionSlot slot = COIN_SESSION_SLOT.load();
  CoinSessionStatus s;
  s.accepting  = COIN_INSERT_ACTIVE.load();
  s.count      = COIN_COUNT.load();
  s.socketFd   = slot.socketFd;
  s.clientIpV4 = slot.clientIpV4;
  s.deadlineMs = COIN_DEADLINE_MILLIS.load();
  return s;
}

static void finalizeSession(bool fraudEnded, const std::map<String, int>& nodeCounts);

// coinSessionTask: sole owner of COIN_INSERT_ACTIVE, COIN_COUNT, COIN_DEADLINE_MILLIS,
// COIN_SESSION_SLOT. Timed queue receive replaces the FreeRTOS timer + semaphore.
void coinSessionTask(void*) {
  CoinSessionEvent evt;
  for (;;) {
    if (xQueueReceive(COIN_SESSION_QUEUE, &evt, portMAX_DELAY) != pdTRUE) continue;
    if (evt.type != COIN_SESSION_START) continue;  // stray event with no open session — drop

    COIN_COUNT.store(0);
    COIN_SESSION_SLOT.store(CoinSessionSlot{ coinIpToU32(String(evt.clientIp)), (int32_t)evt.socketFd });
    COIN_SLOT_READY_MILLIS = millis() + COINSLOT_BOOT_SUPPRESS_MILLIS;
    COIN_DEADLINE_MILLIS.store(millis() + COIN_INSERT_TIMEOUT_MILLIS);
    xTaskNotify(LED_TASK, LED_NOTIFY_START, eSetValueWithOverwrite);
    masterBroadcastSessionStart();
    xTaskNotify(UPDATE_CLIENT_TASK, 1, eSetBits);
    ESP_LOGI(TAG, "Session started — accepting coins (fd=%d ip=%s)", evt.socketFd, evt.clientIp);

    bool fraudEnded = false;
    std::map<String, int> nodeCounts;
    for (;;) {
      if (xQueueReceive(COIN_SESSION_QUEUE, &evt, pdMS_TO_TICKS(COIN_INSERT_TIMEOUT_MILLIS)) != pdTRUE)
        break;  // timeout — finalize
      if (evt.type == COIN_SESSION_COIN) {
        int n = COIN_COUNT.fetch_add(1) + 1;
        nodeCounts[String(evt.mac)]++;
        COIN_DEADLINE_MILLIS.store(millis() + COIN_INSERT_TIMEOUT_MILLIS);
        ESP_LOGI(TAG, "count=%d node=%s", n, evt.mac);
        xTaskNotify(UPDATE_CLIENT_TASK, 1, eSetBits);
        continue;
      }
      if (evt.type == COIN_SESSION_FRAUD_END) { fraudEnded = true; break; }
      // COIN_SESSION_START while already active — ignore
    }

    COIN_INSERT_ACTIVE.store(false);
    xTaskNotify(LED_TASK, LED_NOTIFY_STOP, eSetValueWithOverwrite);
    finalizeSession(fraudEnded, nodeCounts);
  }
}

// Authenticates with Omada and pushes result via WS, then clears the session slot.
static void finalizeSession(bool fraudEnded, const std::map<String, int>& nodeCounts) {
  ESP_LOGI(TAG, "======finalizeSession (fraud=%d)", (int)fraudEnded);

  int coinCount = fraudEnded ? 0 : COIN_COUNT.load();
  COIN_COUNT.store(0);
  masterBroadcastSessionEnd();

  CoinSessionSlot slot = COIN_SESSION_SLOT.load();

  if (fraudEnded) {
    PsychicWebSocketClient* fws = (slot.socketFd >= 0) ? WEBSOCKET_HANDLER.getClient(slot.socketFd) : nullptr;
    if (fws) {
      fws->sendMessage("{\"type\":\"error\",\"subtype\":\"fraud\",\"message\":\"Session ended: repeated fraud detected\"}");
      fws->close();
    }
    COIN_SESSION_SLOT.store(CoinSessionSlot{0, -1});
    return;
  }

  String errorMsg;
  String errorDetail;
  String errorSubtype;
  bool success = false;

  SessionParams* session = HOTSPOT_SESSION_CACHE.findByIp(coinU32ToIp(slot.clientIpV4));

  if (coinCount > 0 && session) {
    unsigned long long additionalMillis = (unsigned long long)coinCount * MINUTES_PER_COIN * MILLIS_PER_MINUTE;
    uint64_t nowMillis = nowEpochMillis();

    if (session->sessionEndMillis > nowMillis && !session->clientId.isEmpty()) {
      ESP_LOGI(TAG, "Extending %s: +%d min (%d remaining)",
               session->clientMac.c_str(),
               coinCount * MINUTES_PER_COIN,
               (int)((session->sessionEndMillis - nowMillis) / MILLIS_PER_MINUTE));

      if (extendOmadaClient(*session, additionalMillis, errorDetail)) {
        success = true;
        for (auto& kv : nodeCounts)
          appendSaleLog(kv.second, kv.second * MINUTES_PER_COIN, kv.first);
      } else {
        ESP_LOGE(TAG, "Extend failed: %s", errorDetail.c_str());
        errorMsg = "Failed to extend session";
        errorSubtype = ERR_SUBTYPE_REFUNDABLE;
      }
    } else {
      ESP_LOGI(TAG, "Authenticating %s for %d minutes",
               session->clientMac.c_str(), coinCount * MINUTES_PER_COIN);

      if (authenticateOmadaClient(*session, additionalMillis, errorDetail)) {
        success = true;
        for (auto& kv : nodeCounts)
          appendSaleLog(kv.second, kv.second * MINUTES_PER_COIN, kv.first);
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

  // Re-read slot: a reconnect may have swapped the fd during the Omada HTTPS call
  int fd = COIN_SESSION_SLOT.load().socketFd;
  PsychicWebSocketClient* ws = (fd >= 0) ? WEBSOCKET_HANDLER.getClient(fd) : nullptr;

  if (ws) {
    if (success) {
      ws->sendMessage("{\"type\":\"authenticated\"}");
      { WsJob j(JOB_REFRESH_CACHE, ""); xQueueSend(WEBSOCKET_JOB_QUEUE, &j, 0); }
    } else {
      String refundCode;
      if (errorSubtype == ERR_SUBTYPE_REFUNDABLE && session) {
        refundCode = generateRefundCode();
        appendRefundLog(session->clientMac, coinCount, coinCount * MINUTES_PER_COIN, refundCode);
      }
      JsonDocument err;  // ArduinoJson escapes errorDetail (Omada's msg can contain quotes)
      err["type"] = "error";
      if (errorSubtype.length() > 0) err["subtype"]    = errorSubtype;
      err["message"] = errorMsg;
      if (errorDetail.length() > 0)  err["detail"]     = errorDetail;
      if (refundCode.length() > 0)   err["refundCode"] = refundCode;
      String errJson;
      serializeJson(err, errJson);
      ws->sendMessage(errJson.c_str());
    }
    ws->close();
  } else {
    ESP_LOGI(TAG, "Auth result not delivered — client already disconnected (auth still succeeded)");
  }

  COIN_SESSION_SLOT.store(CoinSessionSlot{0, -1});
}
