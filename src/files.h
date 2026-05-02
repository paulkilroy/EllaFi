#pragma once

#include "globals.h"
#include <LittleFS.h>

bool fsExists(const char* path);  // stat()-based check — no vfs error on missing files
bool loadConfig();

String macToFilename(const String& mac);
bool savePausedSessionFile(const String& mac, unsigned long remainingMillis);
unsigned long loadPausedSessionFile(const String& mac);
bool hasPausedSessionFile(const String& mac);
void deletePausedSessionFile(const String& mac);

// ── Logging ───────────────────────────────────────────────────────────────────

void purgeOldLogEntries(const char* path);            // remove entries older than 24h; call at startup
void appendErrorLog(const char* tag, const char* msg);
void appendFormattedErrorLog(const char* tag, const char* fmt, ...);
void appendRefundLog(const String& mac, int coins, int minutes, const String& code);
String generateRefundCode();
