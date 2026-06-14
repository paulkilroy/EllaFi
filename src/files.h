#pragma once

#include "globals.h"
#include <LittleFS.h>

bool fsExists(const char* path);  // stat()-based check — no vfs error on missing files
bool loadConfig();

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
