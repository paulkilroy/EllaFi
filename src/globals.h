#pragma once

#include "omada.h"
#include <PsychicHttp.h>
#include <PsychicWebSocket.h>
#include <atomic>

// ── Coin pulse queue ──────────────────────────────────────────────────────────

enum CoinPulseSource { PULSE_COINSLOT, PULSE_BUTTON, PULSE_NETWORK };
struct CoinPulse {
  CoinPulseSource source;
  unsigned long   widthMicros;      // coin slot only — µs the signal stayed LOW; 0 for button/network
  unsigned long   timestampMicros;  // micros() at falling edge (ISR time) — used for gap/fraud calculation
  char            mac[18];          // sender MAC for PULSE_NETWORK; empty = master's own slot
};

// ── Session events (consumed only by coinSessionTask, the session-state owner) ────

enum CoinSessionEventType {
  COIN_SESSION_START,      // web startCoinInsert: open the insertion window
  COIN_SESSION_COIN,       // coinPulseTask: an accepted coin pulse
  COIN_SESSION_FRAUD_END,  // coinPulseTask: fraud limit hit — abort the session
};
struct CoinSessionEvent {
  CoinSessionEventType type;
  int  socketFd;        // COIN_SESSION_START only
  char clientIp[16];    // COIN_SESSION_START only
  char mac[18];         // COIN_SESSION_COIN only — node MAC (empty = master's own slot)
  CoinSessionEvent() = default;
  CoinSessionEvent(CoinSessionEventType t) : type(t), socketFd(-1) { clientIp[0] = '\0'; mac[0] = '\0'; }
  CoinSessionEvent(int fd, const String& ip) : type(COIN_SESSION_START), socketFd(fd) {
    strncpy(clientIp, ip.c_str(), sizeof(clientIp) - 1);
    clientIp[sizeof(clientIp) - 1] = '\0';
    mac[0] = '\0';
  }
};

// The client↔socket binding as ONE atomic so a reader can never observe a torn pair
// (new fd with the previous client's ip). clientIpV4 = 0 means no client.
// Must stay trivially copyable for std::atomic. 8 bytes → lock-backed on Xtensa (correct).
struct CoinSessionSlot {
  uint32_t clientIpV4;
  int32_t  socketFd;
};

// Coherent read bundle for the WS status path.
struct CoinSessionStatus {
  bool          accepting;
  int           count;
  int           socketFd;
  uint32_t      clientIpV4;
  unsigned long deadlineMs;
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

extern PsychicHttpServer       HTTP_SERVER;
extern PsychicWebSocketHandler WEBSOCKET_HANDLER;

// ── FreeRTOS handles ──────────────────────────────────────────────────────────

extern QueueHandle_t     WEBSOCKET_JOB_QUEUE;
extern QueueHandle_t     COIN_PULSE_QUEUE;
extern QueueHandle_t     COIN_SESSION_QUEUE;      // events into the session-state owner
extern TaskHandle_t      UPDATE_CLIENT_TASK;

// ── Role ──────────────────────────────────────────────────────────────────────

extern bool IS_MASTER;

// ── Coin state ────────────────────────────────────────────────────────────────
// Written by coinSessionTask (master) or slaveRecvCoinInsert* (slave).
// Atomic so coinPulseTask and the WS push path read coherent values without a mutex.

extern std::atomic<bool>            COIN_INSERT_ACTIVE;
extern volatile bool                COINSLOT_PROGRAM_MODE;  // relay held HIGH until restart — set via /program
extern std::atomic<int>             COIN_COUNT;
// Rejected-pulse counters — acceptor health, shown per node on the admin dashboard.
// Since boot only (the 3am nightly reboot makes these "today"); durable history is errors.log.
extern std::atomic<uint16_t>        COIN_REJECT_WIDTH_COUNT;     // sub-width pulses (pitik/EMI/bounce)
extern std::atomic<uint16_t>        COIN_REJECT_INTERVAL_COUNT;  // pulses faster than the acceptor can emit
extern std::atomic<CoinSessionSlot> COIN_SESSION_SLOT;    // client ip (u32) + WS fd as one CAS-able unit
extern std::atomic<unsigned long>   COIN_DEADLINE_MILLIS;     // millis() the window expires; for UI countdown
extern unsigned long                COIN_SLOT_READY_MILLIS;  // suppress pulses until this millis() — boot glitch guard

#define SHOULD_ACCEPT_COINS (COIN_INSERT_ACTIVE.load())

// ── WiFi config ───────────────────────────────────────────────────────────────

extern String MANAGEMENT_SSID;      // the SSID our nodes live on (from wifi_ssid); a hotspot client here = isolation breach
extern String HEALTHCHECK_URL;      // Healthchecks.io ping URL (master dead-man's switch); empty = disabled
extern String MANAGEMENT_PASSWORD;
extern String STATIC_IP;        // optional static-IP override (config static_ip); empty = DHCP
extern String STATIC_GATEWAY;   // optional; default = the .1 of the IP's /24
extern String STATIC_SUBNET;    // optional; default = 255.255.255.0
extern String STATIC_DNS;       // optional; default = the gateway

// ── Omada auth types ─────────────────────────────────────────────────────────

constexpr int OMADA_AUTH_TYPE_EXTERNAL_PORTAL = 4;  // coin-operated / External Portal Server
constexpr int OMADA_AUTH_TYPE_VOUCHER         = 3;

// ── Constants ─────────────────────────────────────────────────────────────────

// ── Firmware version ─────────────────────────────────────────────────────────

#define FIRMWARE_VERSION "1.4.2"
#define FIRMWARE_BUILD   84       // increment on every release; used for slave version comparison

// True byte size of the running firmware image, computed once at boot (ESP.getSketchSize()).
// This is what the master serves to slaves for OTA — derived from the running partition, so it's
// correct no matter how we were flashed (a stale NVS value once truncated serial-flashed builds).
extern uint32_t RUNNING_IMAGE_SIZE;

// ── Hardware pins ─────────────────────────────────────────────────────────────

constexpr int           COINBUTTON_PIN                = 0;   // GPIO0 — BOOT button (debounced)
// COINSLOT_PIN: coin acceptor signal (CH-926 style open-collector). The acceptor never drives the
// Coin acceptor pinout reference: https://www.cable-tester.com/coin-acceptor-connector-pin-out/
// line HIGH — it only pulls it to its own GND on a coin pulse. Safe to wire directly to a 3.3V GPIO.
//   GPIO4 ---+--- 2.2k resistor (R1) --- 3V3   (INPUT_PULLUP too — harmless in parallel)
//            |
//           coin (open-collector: LOW = coin pulse, open = idle)
// HW coupling: the acceptor's GND returns through Q1 (COINSLOT_POWER_PIN, low-side switch), so a coin
// pulse only reaches this pin while COINSLOT_POWER_PIN is HIGH (acceptor enabled) — none when disabled.
constexpr int           COINSLOT_PIN                  = 4;
constexpr int           COINSLOT_POWER_PIN            = 14;  // GPIO14 — gate of Q1 (AOD4184A, logic-level): HIGH enables the coin acceptor. 2-wire panels: low-side GND switch via Q1. 3-wire panels: also drives the panel enable on J2.2 — sim-verified 3.3V-logic-safe (even 12V-pullup panels: D4 clamps the pin ~2.1V during boot), but 3.3V only marginally drives panels with ~10k series inputs; measure the panel first (memory/hardware_notes.md, kicad/sim/board/tb_panel3wire.cir)
constexpr unsigned long COIN_INSERT_TIMEOUT_MILLIS    = 10000;
extern int              MINUTES_PER_COIN;              // minutes of access per coin — loaded from config
constexpr int           COMMISSION_RATE_PERCENT       = 20;  // flat seller commission (global by decision 2026-08-09; make configurable if rates ever diverge)
extern int              PRICE_PER_COIN;                // currency value of one coin pulse — revenue multiplier for reports only; from config, default 1 (1 pulse = 1 unit). Bump if the acceptor is set to N pulses/coin.
extern int              FALLBACK_PRICE_PER_VOUCHER;    // voucher revenue fallback when Omada returns a bucket with no amount: revenue = count * this. Reports only; from config, default 5.

constexpr unsigned long COIN_DEBOUNCE_MILLIS              = 300;
constexpr unsigned long COINSLOT_MIN_PULSE_INTERVAL_MILLIS = 100; // PSK got 116ms in testing .. lets use 100mb us a baseline 120;  // fast mode: 20ms pulse + 100ms gap
constexpr unsigned long COINSLOT_MIN_PULSE_WIDTH_MICROS    = 3000; // piezo spike ~100µs, real coin 20,000µs — reject anything shorter than 3ms (seen 3828µs on real coins)
constexpr unsigned long COINSLOT_BOOT_SUPPRESS_MILLIS      = 500;  // ignore pulses after relay closes
constexpr int           COINSLOT_MAX_FRAUD_PER_SESSION     = 3;    // end session after this many fraud detections
constexpr unsigned long MILLIS_PER_MINUTE             = 60000UL;
constexpr unsigned long WIFI_RETRY_DELAY_MILLIS       = 5000;
constexpr unsigned long UPDATE_CLIENT_INTERVAL_MILLIS = 500;
constexpr time_t        NTP_EPOCH_MIN                 = 1000000000L;  // Sep 9 2001
constexpr int           NTP_SYNC_RETRIES              = 40;
constexpr int           COIN_PULSE_QUEUE_SIZE          = 32;
constexpr unsigned long HEARTBEAT_INTERVAL_MILLIS      = 15000;  // how often each node broadcasts its health
constexpr unsigned long NODE_OFFLINE_THRESHOLD_MILLIS  = 60000;  // ~4 missed heartbeats → node counted down (admin dot + healthcheck /fail)
constexpr unsigned long OTA_REBOOT_DELAY_MILLIS        = 30000;  // wait for slaves to fetch /fw.bin before master reboots
