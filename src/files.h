#pragma once

#include "globals.h"
#include <LittleFS.h>

bool fsExists(const char* path);  // stat()-based check — no vfs error on missing files
bool loadConfig();

// Current config.json with passwords masked (••••••••) — for pre-filling config forms. "" on error.
String maskedConfigJson();
// Merge a posted JSON object onto the current config (skipping masked password placeholders),
// validate required fields, and write it back. false + sets err on bad JSON / missing field / write
// failure. Does NOT restart — caller decides. Shared by the admin page and the SoftAP recovery portal.
bool mergeAndSaveConfig(const String& body, String& err);

bool setupFilesystem();       // mounts LittleFS; halts on failure
bool setupConfig();           // loads config.json; provisioning loop if absent
void setupLogs();             // purges old log entries
void checkSerialCommands();   // call from loop() — handles FACTORY_RESET

String macToFilename(const String& mac);
bool savePausedSessionFile(const String& mac, unsigned long remainingMillis);
unsigned long loadPausedSessionFile(const String& mac);
bool hasPausedSessionFile(const String& mac);
void deletePausedSessionFile(const String& mac);

// ── Logging ───────────────────────────────────────────────────────────────────

void purgeOldLogEntries(const char* path, time_t maxAgeSeconds = 86400, int maxEntries = 0);  // remove entries older than maxAge, then cap to the last maxEntries (0 = no cap); call at startup
void appendErrorLog(const char* tag, const char* msg);
void appendRefundLog(const String& mac, int coins, int minutes, const String& code);
void appendSaleLog(int coins, int minutes, const String& nodeMac = "");
String generateRefundCode();

// Optional callback invoked for W/E log entries — used by network.cpp to forward slave logs to master.
typedef void (*LogForwardFn)(char level, const char* tag, const char* msg);
void setLogForwardCallback(LogForwardFn fn);
