#pragma once
// Must be included LAST in each .cpp that uses ESP_LOGE — after esp_log.h.
// Redefines ESP_LOGE to also persist errors to /errors.log via appendFormattedErrorLog.
// appendFormattedErrorLog must NOT call ESP_LOGE (would recurse).

void appendFormattedErrorLog(const char* tag, const char* fmt, ...);

#ifdef ESP_LOGE
#undef ESP_LOGE
#endif
#define ESP_LOGE(tag, fmt, ...) do { \
    esp_log_write(ESP_LOG_ERROR, tag, LOG_FORMAT(E, fmt), esp_log_timestamp(), tag, ##__VA_ARGS__); \
    appendFormattedErrorLog(tag, fmt, ##__VA_ARGS__); \
} while(0)
