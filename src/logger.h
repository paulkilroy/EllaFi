#pragma once
// Must be included LAST in each .cpp — after esp_log.h.
// When RING_BUFFER_LOG_CAPTURE is defined (via build flag), routes all ESP_LOG*
// calls into the in-RAM ring buffer in addition to serial output.
// W/E entries also persist to errors.log and forward to master from slaves.

void _logToBuffer(char level, const char* tag, const char* fmt, ...);

#ifdef RING_BUFFER_LOG_CAPTURE
  #ifdef ESP_LOGE
  #undef ESP_LOGE
  #endif
  #ifdef ESP_LOGW
  #undef ESP_LOGW
  #endif
  #ifdef ESP_LOGI
  #undef ESP_LOGI
  #endif
  #ifdef ESP_LOGD
  #undef ESP_LOGD
  #endif

  #define ESP_LOGE(tag, fmt, ...) do { _logToBuffer('E', tag, fmt, ##__VA_ARGS__); } while(0)
  #define ESP_LOGW(tag, fmt, ...) do { _logToBuffer('W', tag, fmt, ##__VA_ARGS__); } while(0)
  #define ESP_LOGI(tag, fmt, ...) do { _logToBuffer('I', tag, fmt, ##__VA_ARGS__); } while(0)
  #define ESP_LOGD(tag, fmt, ...) do { _logToBuffer('D', tag, fmt, ##__VA_ARGS__); } while(0)
#endif
