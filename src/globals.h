#pragma once

#include "omada.h"
#include <PsychicHttp.h>
#include <PsychicWebSocket.h>

// ── Coin pulse queue ──────────────────────────────────────────────────────────

enum CoinPulseSource { PULSE_COINSLOT, PULSE_BUTTON, PULSE_NETWORK };
struct CoinPulse {
  CoinPulseSource source;
  unsigned long   widthMicros;      // coin slot only — µs the signal stayed LOW; 0 for button/network
  unsigned long   timestampMicros;  // micros() at falling edge (ISR time) — used for gap/fraud calculation
};

// ── WS job queue ──────────────────────────────────────────────────────────────

enum WsJobType { JOB_PAUSE, JOB_RESUME, JOB_REFRESH_CACHE };
struct WsJob {
  WsJobType type;
  char mac[18];
  WsJob() = default;
  WsJob(WsJobType t, const String& m) : type(t) {
    strncpy(mac, m.c_str(), sizeof(mac) - 1);
    mac[sizeof(mac) - 1] = '\0';
  }
};

// ── Session cache ─────────────────────────────────────────────────────────────

extern SessionCache HOTSPOT_SESSION_CACHE;

// ── HTTP/WS server ────────────────────────────────────────────────────────────

extern PsychicHttpServer       server;
extern PsychicWebSocketHandler wsHandler;

// ── FreeRTOS handles ──────────────────────────────────────────────────────────

extern QueueHandle_t     WEBSOCKET_JOB_QUEUE;
extern QueueHandle_t     COIN_PULSE_QUEUE;
extern SemaphoreHandle_t COIN_FINALIZE_SEM;
extern SemaphoreHandle_t COIN_STATE_MUTEX;
extern TaskHandle_t      UPDATE_CLIENT_TASK;
extern TimerHandle_t     COIN_INSERT_TIMER;

// ── Role ──────────────────────────────────────────────────────────────────────

extern bool IS_MASTER;

// ── Coin state ────────────────────────────────────────────────────────────────

extern volatile bool  COIN_INSERT_ACTIVE;
extern volatile bool  COINSLOT_PROGRAM_MODE;  // relay held HIGH until restart — set via /program
extern volatile int   COIN_COUNT;
extern int            COIN_SOCKET_FD;
extern String         COIN_CLIENT_IP;
extern unsigned long          COIN_SLOT_READY_MILLIS;  // suppress pulses until this millis() — boot glitch guard

#define SHOULD_ACCEPT_COINS (IS_MASTER ? (xTimerIsTimerActive(COIN_INSERT_TIMER) == pdTRUE) : COIN_INSERT_ACTIVE)

// ── WiFi config ───────────────────────────────────────────────────────────────

extern String AP_SSID;
extern String AP_PASSWORD;

// ── Constants ─────────────────────────────────────────────────────────────────

constexpr int           COINBUTTON_PIN                = 0;   // GPIO0 — BOOT button (debounced)
constexpr int           COINSLOT_PIN                  = 4;   // GPIO4 — real coin acceptor signal
constexpr int           COINSLOT_POWER_PIN            = 14;  // GPIO14 — coin slot power relay
constexpr unsigned long COIN_INSERT_TIMEOUT_MILLIS    = 10000;
constexpr int           MINUTES_PER_COIN              = 5;
constexpr unsigned long COIN_DEBOUNCE_MILLIS              = 300;
constexpr unsigned long COINSLOT_MIN_PULSE_INTERVAL_MILLIS = 100; // PSK got 116ms in testing .. lets use 100mb us a baseline 120;  // fast mode: 20ms pulse + 100ms gap
constexpr unsigned long COINSLOT_MIN_PULSE_WIDTH_MICROS    = 3000; // piezo spike ~100µs, real coin 20,000µs — reject anything shorter than 3ms (seen 3828µs on real coins)
constexpr unsigned long COINSLOT_BOOT_SUPPRESS_MILLIS      = 500;  // ignore pulses after relay closes
constexpr int           COINSLOT_MAX_FRAUD_PER_SESSION     = 3;    // end session after this many fraud detections
constexpr unsigned long MILLIS_PER_MINUTE             = 60000UL;
constexpr unsigned long WIFI_RETRY_DELAY_MILLIS       = 5000;
constexpr unsigned long UPDATE_CLIENT_INTERVAL_MILLIS = 500;
constexpr time_t        NTP_EPOCH_MIN                 = 1000000000L;  // Sep 9 2001
constexpr int           NTP_SYNC_RETRIES              = 20;
constexpr int           COIN_PULSE_QUEUE_SIZE          = 32;
