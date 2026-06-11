#include "log_buffer.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

static LogEntry          LOG_RING[LOG_BUF_ENTRIES];
static int               LOG_HEAD  = 0;   // next write slot
static int               LOG_COUNT = 0;
static uint32_t          LOG_SEQUENCE   = 0;
static SemaphoreHandle_t LOG_MUTEX   = nullptr;

void logBufferSetup() {
  LOG_MUTEX = xSemaphoreCreateMutex();
}

void logDirect(char level, const char* tag, const char* msg) {
  if (!LOG_MUTEX || xSemaphoreTake(LOG_MUTEX, pdMS_TO_TICKS(10)) != pdTRUE) return;
  LogEntry& e = LOG_RING[LOG_HEAD];
  e.seq      = ++LOG_SEQUENCE;
  e.uptimeMs = (uint32_t)millis();
  e.ts       = time(NULL);
  e.level    = level;
  strncpy(e.tag, tag, LOG_BUF_TAG_LEN - 1); e.tag[LOG_BUF_TAG_LEN - 1] = '\0';
  strncpy(e.msg, msg, LOG_BUF_MSG_LEN - 1); e.msg[LOG_BUF_MSG_LEN - 1] = '\0';
  LOG_HEAD = (LOG_HEAD + 1) % LOG_BUF_ENTRIES;
  if (LOG_COUNT < LOG_BUF_ENTRIES) LOG_COUNT++;
  xSemaphoreGive(LOG_MUTEX);
}

int logBufferSnapshot(LogEntry* out, int maxOut, uint32_t sinceSeq) {
  if (!LOG_MUTEX || xSemaphoreTake(LOG_MUTEX, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
  int start = (LOG_HEAD - LOG_COUNT + LOG_BUF_ENTRIES) % LOG_BUF_ENTRIES;
  int count = 0;
  for (int i = 0; i < LOG_COUNT && count < maxOut; i++) {
    int idx = (start + i) % LOG_BUF_ENTRIES;
    if (LOG_RING[idx].seq > sinceSeq) out[count++] = LOG_RING[idx];
  }
  xSemaphoreGive(LOG_MUTEX);
  return count;
}

uint32_t logBufferLastSeq() {
  if (!LOG_MUTEX || xSemaphoreTake(LOG_MUTEX, pdMS_TO_TICKS(10)) != pdTRUE) return 0;
  uint32_t s = LOG_SEQUENCE;
  xSemaphoreGive(LOG_MUTEX);
  return s;
}
