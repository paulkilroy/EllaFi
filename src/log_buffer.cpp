#include "log_buffer.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

static LogEntry          logRing[LOG_BUF_ENTRIES];
static int               logHead  = 0;   // next write slot
static int               logCount = 0;
static uint32_t          logSeq   = 0;
static SemaphoreHandle_t logMux   = nullptr;

void logBufferSetup() {
  logMux = xSemaphoreCreateMutex();
}

void logDirect(char level, const char* tag, const char* msg) {
  if (!logMux || xSemaphoreTake(logMux, pdMS_TO_TICKS(10)) != pdTRUE) return;
  LogEntry& e = logRing[logHead];
  e.seq      = ++logSeq;
  e.uptimeMs = (uint32_t)millis();
  e.ts       = time(NULL);
  e.level    = level;
  strncpy(e.tag, tag, LOG_BUF_TAG_LEN - 1); e.tag[LOG_BUF_TAG_LEN - 1] = '\0';
  strncpy(e.msg, msg, LOG_BUF_MSG_LEN - 1); e.msg[LOG_BUF_MSG_LEN - 1] = '\0';
  logHead = (logHead + 1) % LOG_BUF_ENTRIES;
  if (logCount < LOG_BUF_ENTRIES) logCount++;
  xSemaphoreGive(logMux);
}

int logBufferSnapshot(LogEntry* out, int maxOut, uint32_t sinceSeq) {
  if (!logMux || xSemaphoreTake(logMux, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
  int start = (logHead - logCount + LOG_BUF_ENTRIES) % LOG_BUF_ENTRIES;
  int count = 0;
  for (int i = 0; i < logCount && count < maxOut; i++) {
    int idx = (start + i) % LOG_BUF_ENTRIES;
    if (logRing[idx].seq > sinceSeq) out[count++] = logRing[idx];
  }
  xSemaphoreGive(logMux);
  return count;
}

uint32_t logBufferLastSeq() {
  if (!logMux || xSemaphoreTake(logMux, pdMS_TO_TICKS(10)) != pdTRUE) return 0;
  uint32_t s = logSeq;
  xSemaphoreGive(logMux);
  return s;
}
