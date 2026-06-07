#include "coin.h"
#include "files.h"

#include <WiFi.h>
#include <map>
#include <esp_log.h>
#include "logger.h"

// Coins contributed per node MAC in the current insertion session.
// Keyed by uppercase MAC string. Guarded by COIN_STATE_MUTEX.
static std::map<String, int> s_nodeCoinCounts;

static const char* TAG = "EllaFi";

static const char* ERR_SUBTYPE_NO_SESSION = "no_session";
static const char* ERR_SUBTYPE_REFUNDABLE = "refundable";

// Set by reportFraud when fraud limit ends the session — tells finalizeCoinInsert to skip WS messaging
static volatile bool FRAUD_ENDED_SESSION = false;

// ISR-only — tracks falling edge time (micros) for pulse width measurement
static volatile unsigned long COINBUTTON_FALL_TIME = 0;
static volatile unsigned long COINSLOT_FALL_TIME   = 0;

// ISR-only — debounce timestamp for button (millis)
static volatile unsigned long LAST_COIN_PULSE_TIME = 0;

// ── ISR + timer ───────────────────────────────────────────────────────────────

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
//   GPIO5 (coin slot): PC817 provides galvanic isolation but attenuates the spike to ~5µs
//   GPIO0 (boot button): spike couples electromagnetically; no isolation, ~100µs width
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

void coinTimerCallback(TimerHandle_t xTimer) {
  xTaskNotify(LED_TASK, LED_NOTIFY_STOP, eSetValueWithOverwrite);  // cut coin acceptor power immediately
  xSemaphoreGive(COIN_FINALIZE_SEM);
}

void restartCoinInsertTimer() {
  xTimerReset(COIN_INSERT_TIMER, 0);  // starts if inactive, restarts if active
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
  COIN_INSERT_ACTIVE = true;
  xTaskNotify(LED_TASK, LED_NOTIFY_START, eSetValueWithOverwrite);
  ESP_LOGI(TAG, "Session started — accepting coins");
}

void slaveRecvCoinInsertEnd() {
  COIN_INSERT_ACTIVE = false;
  xTaskNotify(LED_TASK, LED_NOTIFY_STOP, eSetValueWithOverwrite);
  ESP_LOGI(TAG, "Session ended — coin acceptance disabled");
}

// ── FreeRTOS tasks ────────────────────────────────────────────────────────────

// Master-only: send WS fraud error and finalize session if limit reached.
static void reportFraud(int fraudCount, unsigned long& prevAcceptedTimestampMicros) {
  SessionParams* session = HOTSPOT_SESSION_CACHE.findByIp(COIN_CLIENT_IP);
  ESP_LOGE(TAG, "Fraud #%d MAC=%s", fraudCount, session ? session->clientMac.c_str() : "unknown");
  PsychicWebSocketClient* ws = (COIN_SOCKET_FD >= 0) ? wsHandler.getClient(COIN_SOCKET_FD) : nullptr;
  if (fraudCount >= COINSLOT_MAX_FRAUD_PER_SESSION) {
    ESP_LOGE(TAG, "Fraud limit reached - ending session");
    if (ws) ws->sendMessage("{\"type\":\"error\",\"subtype\":\"fraud\",\"message\":\"Session ended: repeated fraud detected\"}");
    xTimerStop(COIN_INSERT_TIMER, 0);  // close acceptance window immediately — SHOULD_ACCEPT_COINS goes false
    xTaskNotify(LED_TASK, LED_NOTIFY_STOP, eSetValueWithOverwrite);
    xSemaphoreTake(COIN_STATE_MUTEX, portMAX_DELAY);
    COIN_COUNT = 0;
    s_nodeCoinCounts.clear();
    xSemaphoreGive(COIN_STATE_MUTEX);
    prevAcceptedTimestampMicros = 0;
    FRAUD_ENDED_SESSION = true;
    xSemaphoreGive(COIN_FINALIZE_SEM);
  }
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
      ESP_LOGE(TAG, "Coin pulse suppressed - acceptor still booting");
      continue;
    }

    unsigned long intervalSinceLast = prevAcceptedTimestampMicros > 0 ? pulse.timestampMicros - prevAcceptedTimestampMicros : 0;
    ESP_LOGE(TAG, "coin pulse: src=%d width=%luus interval=%luus", pulse.source, pulse.widthMicros, intervalSinceLast);

    // Width filter — rejects piezo spikes at source on all nodes (PULSE_COINSLOT and PULSE_BUTTON)
    if (pulse.source == PULSE_COINSLOT || pulse.source == PULSE_BUTTON) {
      if (pulse.widthMicros < COINSLOT_MIN_PULSE_WIDTH_MICROS) {
        fraudCountThisSession++;
        ESP_LOGE(TAG, "Pulse width fraud #%d: width=%luus (min %luus)", fraudCountThisSession, pulse.widthMicros, COINSLOT_MIN_PULSE_WIDTH_MICROS);
        if (IS_MASTER) reportFraud(fraudCountThisSession, prevAcceptedTimestampMicros);
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
        if (IS_MASTER) reportFraud(fraudCountThisSession, prevAcceptedTimestampMicros);
        continue;
      }
    }

    prevAcceptedTimestampMicros = pulse.timestampMicros;
    ESP_LOGE(TAG, "Coin pulse accepted - %s src=%d", IS_MASTER ? "master" : "slave", pulse.source);

    if (IS_MASTER) {
      // Determine which node the coin came from (empty mac = master's own slot)
      String nodeMac = pulse.mac[0] ? String(pulse.mac) : WiFi.macAddress();
      nodeMac.toUpperCase();
      xSemaphoreTake(COIN_STATE_MUTEX, portMAX_DELAY);
      COIN_COUNT++;
      s_nodeCoinCounts[nodeMac]++;
      xSemaphoreGive(COIN_STATE_MUTEX);
      ESP_LOGE(TAG, "count=%d node=%s", COIN_COUNT, nodeMac.c_str());
      xTimerReset(COIN_INSERT_TIMER, 0);
      xTaskNotify(UPDATE_CLIENT_TASK, 1, eSetBits);
    } else {
      slaveSendCoinInserted();
    }
  }
}

// FinalizeCoinTask: sleeps until coinTimerCallback() signals COIN_FINALIZE_SEM.
// Pinned to Core 1 alongside loop() — never touches the radio stack on Core 0.
void finalizeCoinTask(void*) {
  for (;;) {
    xSemaphoreTake(COIN_FINALIZE_SEM, portMAX_DELAY);
    finalizeCoinInsert();
  }
}

// Called 10s after last coin insertion. Authenticates with Omada and pushes result via WS.
void finalizeCoinInsert() {
  ESP_LOGI(TAG, "======finalizeCoinInsert called");

  // Close the insertion window. COIN_INSERT_ACTIVE stays true so no new session can start
  // and COIN_SOCKET_FD/COIN_CLIENT_IP remain valid for WS reconnect tracking during HTTPS.
  std::map<String, int> nodeCounts;
  xSemaphoreTake(COIN_STATE_MUTEX, portMAX_DELAY);
  int coinCount = COIN_COUNT;
  COIN_COUNT = 0;
  nodeCounts = s_nodeCoinCounts;
  s_nodeCoinCounts.clear();
  xTimerStop(COIN_INSERT_TIMER, 0);  // no-op if already stopped (fraud path or timer already fired)
  xSemaphoreGive(COIN_STATE_MUTEX);
  masterBroadcastSessionEnd();  // tell slaves to stop accepting coins immediately

  String errorMsg;
  String errorDetail;
  String errorSubtype;
  bool success = false;

  SessionParams* session = HOTSPOT_SESSION_CACHE.findByIp(COIN_CLIENT_IP);

  if (coinCount > 0 && session) {
    unsigned long long additionalMillis = coinCount * MINUTES_PER_COIN * MILLIS_PER_MINUTE;
    uint64_t nowMillis = nowEpochMillis();

    if (session->sessionEndMillis > nowMillis && !session->clientId.isEmpty()) {
      // Active session — extend by new coins only
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
      // No active session — fresh auth
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
    if (!FRAUD_ENDED_SESSION) errorMsg = "No coins inserted";
    FRAUD_ENDED_SESSION = false;
  } else {
    errorMsg = "No portal session found for this device";
    errorSubtype = ERR_SUBTYPE_NO_SESSION;
  }

  PsychicWebSocketClient* ws = (COIN_SOCKET_FD >= 0) ? wsHandler.getClient(COIN_SOCKET_FD) : nullptr;

  if (ws) {
    if (success) {
      ws->sendMessage("{\"type\":\"authenticated\"}");
      { WsJob j(JOB_REFRESH_CACHE, ""); xQueueSend(WEBSOCKET_JOB_QUEUE, &j, 0); }  // background: sync sessionEndMillis + clientId
    } else {
      String refundCode;
      if (errorSubtype == ERR_SUBTYPE_REFUNDABLE && session) {
        refundCode = generateRefundCode();
        appendRefundLog(session->clientMac, coinCount, coinCount * MINUTES_PER_COIN, refundCode);
      }
      String errJson = "{\"type\":\"error\"";
      if (errorSubtype.length() > 0) errJson += ",\"subtype\":\"" + errorSubtype + "\"";
      errJson += ",\"message\":\"" + errorMsg + "\"";
      if (errorDetail.length() > 0) errJson += ",\"detail\":\"" + errorDetail + "\"";
      if (refundCode.length() > 0)  errJson += ",\"refundCode\":\"" + refundCode + "\"";
      errJson += "}";
      ws->sendMessage(errJson.c_str());
    }
    ws->close();
  } else {
    ESP_LOGW(TAG, "Auth result not delivered — client already disconnected");
  }

  xSemaphoreTake(COIN_STATE_MUTEX, portMAX_DELAY);
  COIN_INSERT_ACTIVE = false;
  COIN_SOCKET_FD = -1;
  COIN_CLIENT_IP = "";
  xSemaphoreGive(COIN_STATE_MUTEX);
}
