#pragma once
#include <Arduino.h>

#define LOG_BUF_ENTRIES 200
#define LOG_BUF_TAG_LEN 16
#define LOG_BUF_MSG_LEN 128

struct LogEntry {
  uint32_t seq;
  uint32_t uptimeMs;
  time_t   ts;
  char     level;               // E W I D V
  char     tag[LOG_BUF_TAG_LEN];
  char     msg[LOG_BUF_MSG_LEN];
};

// Call once in setup().
void logBufferSetup();

// Write an entry directly into the ring buffer.
void logDirect(char level, const char* tag, const char* msg);

// Copy up to maxOut entries with seq > sinceSeq into out[]. Returns count written.
int logBufferSnapshot(LogEntry* out, int maxOut, uint32_t sinceSeq);

// Sequence number of the most recent entry (0 if empty).
uint32_t logBufferLastSeq();
