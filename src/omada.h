#pragma once

#define TEST_MODE true

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <map>
#include <set>
#include <time.h>
#include "led.h"

// Halts the device permanently (used for unrecoverable startup failures)
#define halt() do { ledHalt(); } while(0)

// Returns current time as Unix epoch milliseconds (requires NTP sync in setup)
inline uint64_t nowEpochMillis() { return (uint64_t)time(NULL) * 1000ULL; }

// ---- Session data model ----

struct SessionParams {
  String clientMac;
  String clientIp;
  String apMac;
  String ssidName;
  String radioId;
  uint64_t sessionStartMillis = 0;       // session start, Unix epoch ms; 0 = unknown
  uint64_t sessionEndMillis = 0;         // session expiry, Unix epoch ms; 0 = no active session
  String clientId;                // Omada internal session ID (hex) — use for extend/disconnect
  unsigned long pausedRemainingMillis = 0; // 0 = not paused
  int socketFd = -1;                   // last known WS socket
};

// Session store keyed by MAC (stable identity), with an IP→MAC index for O(log n) lookup by IP.
// Keying by MAC means a DHCP re-IP only re-points the index — the SessionParams object is never
// destroyed or moved. So a raw pointer held across a slow Omada call stays valid (no use-after-free),
// and paused/socket state survives the re-IP. Ownership: values live in _byMac; _ipToMac maps the
// current IP → MAC. Thread safety: methods lock internally; field mutations after a find/upsert
// don't need the lock (map value addresses are stable). Use lock()/unlock() only for iteration.
class SessionCache {
  std::map<String, SessionParams> _byMac;    // owns the objects
  std::map<String, String>        _ipToMac;  // current IP → MAC
  SemaphoreHandle_t _mutex;
public:
  SessionCache() : _mutex(xSemaphoreCreateRecursiveMutex()) {}

  void lock()   { xSemaphoreTakeRecursive(_mutex, portMAX_DELAY); }
  void unlock() { xSemaphoreGiveRecursive(_mutex); }

  // Create or update the entry for `mac`, recording `ip` as its current IP. An IP change just
  // re-points the index — the object (and any pointer into it) is preserved. Thread-safe. If
  // mutating the returned reference, hold lock() around the block.
  SessionParams& upsert(const String& ip, const String& mac = "") {
    lock();
    String key = mac;
    if (key.isEmpty()) {                          // no mac given — resolve via the IP index
      auto ix = _ipToMac.find(ip);
      key = (ix != _ipToMac.end()) ? ix->second : ip;  // last resort: key by ip (degenerate)
    }
    SessionParams& s = _byMac[key];               // creates with defaults if new
    s.clientMac = key;
    if (!ip.isEmpty()) {
      if (s.clientIp != ip && !s.clientIp.isEmpty()) {
        auto old = _ipToMac.find(s.clientIp);     // drop our previous IP mapping (only if still ours)
        if (old != _ipToMac.end() && old->second == key) _ipToMac.erase(old);
      }
      _ipToMac[ip] = key;
      s.clientIp = ip;
    }
    unlock();
    return s;
  }

  // Returns nullptr for empty mac or if not found. Thread-safe (locks internally).
  SessionParams* findByMac(const String& mac) {
    if (mac.isEmpty()) return nullptr;
    lock();
    auto it = _byMac.find(mac);
    SessionParams* result = (it != _byMac.end()) ? &it->second : nullptr;
    unlock();
    return result;
  }

  // Returns nullptr for empty ip or if not found. Thread-safe (locks internally).
  SessionParams* findByIp(const String& ip) {
    if (ip.isEmpty()) return nullptr;
    lock();
    SessionParams* result = nullptr;
    auto ix = _ipToMac.find(ip);
    if (ix != _ipToMac.end()) {
      auto it = _byMac.find(ix->second);
      if (it != _byMac.end()) result = &it->second;
    }
    unlock();
    return result;
  }

  std::map<String, SessionParams>::iterator begin() { return _byMac.begin(); }
  std::map<String, SessionParams>::iterator end()   { return _byMac.end();   }
  size_t size() const { return _byMac.size(); }
  bool empty()  const { return _byMac.empty(); }
};

// ---- Omada config globals (defined in omada.cpp, populated by loadConfig in main.cpp) ----

extern String CONTROLLER_BASE_URL;
extern String CONTROLLER_ID;
extern String SITE_ID;
extern String SITE_NAME;
extern String ADMIN_USERNAME;
extern String ADMIN_PASSWORD;

// Cached controller status (GET settings/system/status) for the admin dashboard.
// Identity strings (model/version/timeZone) are set ONCE on first successful fetch and never
// rewritten → safe to read concurrently. Counters/flags refresh at boot and on each
// refreshHotspotSessionCache as plain 32-bit writes (hardware-atomic on ESP32). Display-only.
struct ControllerStatus {
  String        model;
  String        version;
  String        timeZone;
  bool          reachable     = false;
  unsigned long uptimeMillis  = 0;   // controller uptime (ms)
  unsigned long fetchedMillis = 0;   // millis() at last successful fetch
  int           clockSkewSec  = 0;   // controller systemTime − device NTP, seconds (valid when reachable)
  bool          mgmtSsidBreach = false;  // a hotspot client was seen on MANAGEMENT_SSID = isolation breach
};
extern ControllerStatus CONTROLLER_STATUS;

// True when the node can actually sell WiFi: NTP synced + controller reachable + site loaded.
// When false the node is "out of service" (coin acceptance off, red LED, dashboard banner) — but
// the web server stays up so the config (e.g. a changed controller URL) can always be fixed.
bool isServiceReady();

// Re-fetch GET settings/system/status into CONTROLLER_STATUS (reachable + counters + TZ). Best-effort,
// bounded timeout — safe to call periodically from the master service-health monitor. On failure it
// flips reachable=false and forces a re-login on the next call so recovery is prompt.
void refreshControllerStatus();

// ---- Omada API functions ----
// Credential management is internal — callers pass no session state.
// Concurrent calls from different tasks are safe: each call gets a credential
// snapshot and builds its own local HTTPClient.

bool loadOmadaSites(String& errorDetail);
bool authenticateOmadaClient(SessionParams& session, unsigned long long durationMillis, String& errorDetail);
bool extendOmadaClient(SessionParams& session, unsigned long long durationMillis, String& errorDetail);
bool disconnectOmadaClient(SessionParams& session, String& errorDetail);
JsonDocument getHotspotClientsJson();
JsonDocument getAllClientsJson();

// Contention instrumentation: number of Omada TLS operations currently in flight
// (each ~40KB of internal DRAM during handshake). Read by the healthcheck ping's
// failure log to separate firmware contention from WAN loss.
int omadaTlsInFlight();
bool setClientName(const String& mac, const String& name, String& errorDetail);  // PATCH a client's Omada alias
JsonDocument mergeClientLists(JsonArray hotspotClients, JsonArray allClients, uint64_t nowMs);
void refreshHotspotSessionCache();
JsonDocument getDeviceGridJson();       // GET /grid/devices → parsed doc (null on failure)
void refreshDeviceStatus();             // poll Omada device list → {mac:status} snapshot (captive AP map)
String deviceStatusJson();              // current {mac:status} snapshot for /status & /ws ("{}" if none)
unsigned long getOmadaSessionAgeMs();   // ms since last Omada login; 0 if never logged in
unsigned long getLastCacheRefreshMs();  // ms since last refreshHotspotSessionCache(); 0 if never run

JsonDocument getVoucherGroupsJson();
JsonDocument getVoucherHistoryJson(time_t startSec, time_t endSec);
bool createVoucherGroup(const String& name, int durationMin, int totalCount,
                        const String& description,
                        String& groupId, String& errorDetail);
JsonDocument getVoucherCodesJson(const String& groupId, int page);
bool deleteVoucherGroup(const String& groupId, String& errorDetail);

bool setupOmada(); // Call from setup() after NTP sync — initialises mutex, loads sites, primes cache

extern volatile int VOUCHER_ACTIVE_COUNT;
