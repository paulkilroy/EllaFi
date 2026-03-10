#pragma once

#define TEST_MODE true

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <map>
#include <time.h>

// Halts the device permanently (used for unrecoverable startup failures)
#define halt() do { while(1); } while(0)

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

// Dual-key session store: O(log n) lookup by IP or by MAC.
// Ownership: SessionParams values live in _byIp; _byMac holds raw pointers into that map.
// std::map guarantees references to existing values are stable across inserts/erases of other keys.
// Thread safety: upsert/findByIp/findByMac lock the maps internally; SessionParams field
// mutations after a find/upsert do not need the lock. Use lock()/unlock() only for iteration.
class SessionCache {
  std::map<String, SessionParams> _byIp;
  std::map<String, SessionParams*> _byMac;
  SemaphoreHandle_t _mutex;
public:
  SessionCache() : _mutex(xSemaphoreCreateRecursiveMutex()) {}

  void lock()   { xSemaphoreTakeRecursive(_mutex, portMAX_DELAY); }
  void unlock() { xSemaphoreGiveRecursive(_mutex); }

  // Create or update entry keyed by IP. Also indexes by mac if non-empty.
  // If mac already exists at a different IP, byMac is redirected here (old IP entry untouched).
  // Thread-safe (locks internally). If mutating the returned reference, hold lock() around the block.
  SessionParams& upsert(const String& ip, const String& mac = "") {
    lock();
    auto it = _byIp.find(ip);
    if (it != _byIp.end()) {
      SessionParams& s = it->second;
      if (!mac.isEmpty() && s.clientMac != mac) {
        _byMac.erase(s.clientMac);
        s.clientMac = mac;
        _byMac[mac] = &s;
      }
      unlock();
      return s;
    }
    SessionParams& s = _byIp[ip];
    s.clientIp = ip;
    if (!mac.isEmpty()) {
      s.clientMac = mac;
      _byMac[mac] = &s;
    }
    unlock();
    return s;
  }

  // Returns nullptr for empty ip or if not found. Thread-safe (locks internally).
  SessionParams* findByIp(const String& ip) {
    if (ip.isEmpty()) return nullptr;
    lock();
    auto it = _byIp.find(ip);
    SessionParams* result = it != _byIp.end() ? &it->second : nullptr;
    unlock();
    return result;
  }

  // Returns nullptr for empty mac or if not found. Thread-safe (locks internally).
  SessionParams* findByMac(const String& mac) {
    if (mac.isEmpty()) return nullptr;
    lock();
    auto it = _byMac.find(mac);
    SessionParams* result = it != _byMac.end() ? it->second : nullptr;
    unlock();
    return result;
  }

  void removeByIp(const String& ip) {
    lock();
    auto it = _byIp.find(ip);
    if (it != _byIp.end()) {
      _byMac.erase(it->second.clientMac);
      _byIp.erase(it);
    }
    unlock();
  }

  std::map<String, SessionParams>::iterator begin() { return _byIp.begin(); }
  std::map<String, SessionParams>::iterator end()   { return _byIp.end();   }
  size_t size() const { return _byIp.size(); }
  bool empty()  const { return _byIp.empty(); }
};

// ---- Omada config globals (defined in omada.cpp, populated by loadConfig in main.cpp) ----

extern String CONTROLLER_BASE_URL;
extern String CONTROLLER_ID;
extern String OPERATOR_USERNAME;
extern String OPERATOR_PASSWORD;
extern String SITE_ID;
extern String SITE_NAME;
extern String ADMIN_USERNAME;
extern String ADMIN_PASSWORD;

// ---- Omada API functions ----

bool loginToController(String& errorDetail);
bool loginToAdminController(String& errorDetail);
bool loadOmadaSites(String& errorDetail);
bool authenticateOmadaClient(SessionParams& session, unsigned long long durationMillis, String& errorDetail);
bool extendOmadaClient(SessionParams& session, unsigned long long durationMillis, String& errorDetail);
bool disconnectOmadaClient(SessionParams& session, String& errorDetail);
JsonDocument getHotspotClientsJson();
JsonDocument getAllClientsJson();
JsonDocument mergeClientLists(JsonArray hotspotClients, JsonArray allClients, uint64_t nowMs);
void refreshHotspotSessionCache();
void omadaSetup(); // Call from setup() after NTP sync — loads sites and primes session cache
void omadaLoop();  // Call from loop() — runs periodic cache refresh
