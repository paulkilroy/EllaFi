#pragma once

#include "globals.h"
#include <LittleFS.h>

bool fsExists(const char* path);  // stat()-based check — no vfs error on missing files
bool loadConfig();

bool setupFilesystem();  // mounts LittleFS; halts on failure
bool setupConfig();      // loads config.json; halts on failure
void setupLogs();        // purges old log entries

String macToFilename(const String& mac);
bool savePausedSessionFile(const String& mac, unsigned long remainingMillis);
unsigned long loadPausedSessionFile(const String& mac);
bool hasPausedSessionFile(const String& mac);
void deletePausedSessionFile(const String& mac);

// ── Logging ───────────────────────────────────────────────────────────────────

void purgeOldLogEntries(const char* path);            // remove entries older than 24h; call at startup
void appendErrorLog(const char* tag, const char* msg);
void logAppend(char level, const char* tag, const char* fmt, ...);  // W/E → errors.log + slave forward; all levels → ring buffer
void appendRefundLog(const String& mac, int coins, int minutes, const String& code);
String generateRefundCode();

// Optional callback invoked by logAppend for W/E entries — used by network.cpp to forward slave logs to master.
typedef void (*LogForwardFn)(char level, const char* tag, const char* msg);
void setLogForwardCallback(LogForwardFn fn);
