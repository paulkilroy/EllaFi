#include "files.h"
#include "log_buffer.h"
#include "network.h"

#include <ArduinoJson.h>
#include <esp_log.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "logger.h"

static const char* TAG = "files";

// LittleFS.exists() opens the file in read mode internally, which triggers a
// vfs_api error log if the file doesn't exist. Use stat() instead.
bool setupFilesystem() {
  if (!LittleFS.begin(true)) {
    ESP_LOGE(TAG, "LittleFS mount failed");
    halt();
  }
  ESP_LOGI(TAG, "LittleFS mounted");
  return true;
}

static void runProvisioningMode() {
  ESP_LOGI(TAG, "No config.json — entering provisioning mode");
  unsigned long lastPrint = 0;
  String line;
  for (;;) {
    if (millis() - lastPrint >= 1000) {
      lastPrint = millis();
      Serial.println("PROVISIONING_READY");
    }
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\r') continue;
      if (c == '\n') {
        line.trim();
        if (line.startsWith("CONFIG:")) {
          String json = line.substring(7);
          File f = LittleFS.open("/config.json", FILE_WRITE);
          if (!f) {
            Serial.println("CONFIG_ERROR: cannot write file");
          } else {
            f.print(json);
            f.close();
            Serial.println("CONFIG_OK");
            delay(200);
            esp_restart();
          }
        }
        line = "";
      } else {
        line += c;
      }
    }
    delay(10);
  }
}

bool setupConfig() {
  if (!loadConfig()) {
    runProvisioningMode();  // never returns — restarts after writing config
  }
  return true;
}

// Call from loop() — handles FACTORY_RESET command from the provisioner web page.
// Deletes config.json and restarts into provisioning mode.
void checkSerialCommands() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line.trim();
      if (line == "FACTORY_RESET") {
        LittleFS.remove("/config.json");
        Serial.println("FACTORY_RESET_OK");
        delay(200);
        esp_restart();
      }
      line = "";
    } else {
      line += c;
    }
  }
}

void setupLogs() {
  purgeOldLogEntries("/errors.log");
  purgeOldLogEntries("/refunds.log");
  purgeOldLogEntries("/vendo_history.json",   365 * 86400);
  purgeOldLogEntries("/voucher_history.json", 365 * 86400);
}

bool fsExists(const char* path) {
  struct stat st;
  String fullPath = String("/littlefs") + path;
  return stat(fullPath.c_str(), &st) == 0;
}


bool loadConfig() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) {
    ESP_LOGE(TAG, "config.json not found");
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    ESP_LOGE(TAG, "Failed to parse config.json: %s", err.c_str());
    return false;
  }
  AP_SSID               = doc["wifi_ssid"]               | "";
  AP_PASSWORD           = doc["wifi_password"]            | "";
  CONTROLLER_BASE_URL   = doc["omada_url"]                | "";
  CONTROLLER_ID         = doc["omada_controller_id"]      | "";
  ADMIN_USERNAME        = doc["omada_username"]           | "";
  ADMIN_PASSWORD        = doc["omada_password"]           | "";
  MASTER_MAC            = doc["master_mac"]               | "";
  NET_PSK               = doc["network_key"]              | 0ULL;
  MINUTES_PER_COIN      = doc["minutes_per_coin"]         | 24;
  PRICE_PER_COIN        = doc["price_per_coin"]           | 5;
  if (AP_SSID.isEmpty())             { ESP_LOGE(TAG, "config.json missing: wifi_ssid");           return false; }
  if (AP_PASSWORD.isEmpty())         { ESP_LOGE(TAG, "config.json missing: wifi_password");       return false; }
  if (CONTROLLER_BASE_URL.isEmpty()) { ESP_LOGE(TAG, "config.json missing: omada_url");           return false; }
  if (CONTROLLER_ID.isEmpty())       { ESP_LOGE(TAG, "config.json missing: omada_controller_id"); return false; }
  if (ADMIN_USERNAME.isEmpty())      { ESP_LOGE(TAG, "config.json missing: omada_username");      return false; }
  if (ADMIN_PASSWORD.isEmpty())      { ESP_LOGE(TAG, "config.json missing: omada_password");      return false; }
  if (NET_PSK == 0)                  { ESP_LOGE(TAG, "config.json missing: network_key");         return false; }
  ESP_LOGI(TAG, "Config loaded: ssid=%s url=%s", AP_SSID.c_str(), CONTROLLER_BASE_URL.c_str());
  return true;
}

// ── Paused session file helpers ───────────────────────────────────────────────

String macToFilename(const String& mac) {
  String safe = mac;
  safe.replace(":", "");
  return "/pause_" + safe + ".dat";
}

bool savePausedSessionFile(const String& mac, unsigned long remainingMillis) {
  String path = macToFilename(mac);
  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) {
    ESP_LOGE(TAG, "Failed to save paused session to %s", path.c_str());
    return false;
  }
  f.write((uint8_t*)&remainingMillis, sizeof(remainingMillis));
  f.close();
  ESP_LOGI(TAG, "Saved paused session for %s: %lu ms", mac.c_str(), remainingMillis);
  return true;
}

unsigned long loadPausedSessionFile(const String& mac) {
  String path = macToFilename(mac);
  if (!fsExists(path.c_str())) return 0;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return 0;
  unsigned long remainingMillis = 0;
  f.read((uint8_t*)&remainingMillis, sizeof(remainingMillis));
  f.close();
  ESP_LOGI(TAG, "Loaded paused session for %s: %lu ms", mac.c_str(), remainingMillis);
  return remainingMillis;
}

bool hasPausedSessionFile(const String& mac) {
  return fsExists(macToFilename(mac).c_str());
}

void deletePausedSessionFile(const String& mac) {
  String path = macToFilename(mac);
  if (fsExists(path.c_str())) {
    LittleFS.remove(path);
    ESP_LOGI(TAG, "Deleted paused session file: %s", path.c_str());
  }
}

// ── Logging ───────────────────────────────────────────────────────────────────

void purgeOldLogEntries(const char* path, time_t maxAgeSeconds) {
  if (!fsExists(path)) return;
  const char* tmpPath = "/log_tmp.dat";
  File src = LittleFS.open(path, FILE_READ);
  if (!src) return;
  File dst = LittleFS.open(tmpPath, FILE_WRITE);
  if (!dst) { src.close(); return; }

  time_t cutoff = time(NULL) - maxAgeSeconds;
  int kept = 0, removed = 0;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;
    int tsIdx = line.indexOf("\"ts\":");
    if (tsIdx >= 0) {
      time_t ts = (time_t)atol(line.c_str() + tsIdx + 5);
      if (ts < cutoff) { removed++; continue; }
    }
    dst.println(line);
    kept++;
  }
  src.close();
  dst.close();
  LittleFS.remove(path);
  LittleFS.rename(tmpPath, path);
  ESP_LOGI(TAG, "Purged %s: kept=%d removed=%d", path, kept, removed);
}

// appendErrorLog must NOT call ESP_LOGE — would recurse via logger.h macro.
void appendErrorLog(const char* tag, const char* msg) {
  File f = LittleFS.open("/errors.log", FILE_APPEND);
  if (!f) return;
  JsonDocument doc;
  doc["ts"]  = (long)time(NULL);
  doc["tag"] = tag;
  doc["msg"] = msg;
  serializeJson(doc, f);
  f.print('\n');
  f.close();
}

static LogForwardFn LOG_FORWARD_FN = nullptr;

void setLogForwardCallback(LogForwardFn fn) {
  LOG_FORWARD_FN = fn;
}

static void logCore(char level, const char* tag, const char* buf) {
  Serial.printf("%c (%lu) %s: %s\r\n", level, (unsigned long)millis(), tag, buf);
  logDirect(level, tag, buf);
  if (level == 'W' || level == 'E') {
    appendErrorLog(tag, buf);
    if (LOG_FORWARD_FN) LOG_FORWARD_FN(level, tag, buf);
  }
}

void _logToBuffer(char level, const char* tag, const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  logCore(level, tag, buf);
}

void logAppend(char level, const char* tag, const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  logCore(level, tag, buf);
}

void appendRefundLog(const String& mac, int coins, int minutes, const String& code) {
  File f = LittleFS.open("/refunds.log", FILE_APPEND);
  if (!f) return;
  JsonDocument doc;
  doc["ts"]      = (long)time(NULL);
  doc["mac"]     = mac;
  doc["coins"]   = coins;
  doc["minutes"] = minutes;
  doc["code"]    = code;
  serializeJson(doc, f);
  f.print('\n');
  f.close();
}

void appendSaleLog(int coins, int minutes, const String& nodeMac) {
  File f = LittleFS.open("/vendo_history.json", FILE_APPEND);
  if (!f) return;
  JsonDocument doc;
  doc["ts"]    = (long)time(NULL);
  doc["coins"] = coins;
  doc["min"]   = minutes;
  if (!nodeMac.isEmpty()) doc["node"] = nodeMac;
  serializeJson(doc, f);
  f.print('\n');
  f.close();
}

String generateRefundCode() {
  static const char chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O/I/1
  String code = "";
  for (int i = 0; i < 6; i++) code += chars[esp_random() % (sizeof(chars) - 1)];
  return code;
}
