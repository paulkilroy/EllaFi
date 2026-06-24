#include "files.h"
#include "log_buffer.h"
#include "network.h"

#include <ArduinoJson.h>
#include <esp_log.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>  // strerror
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
  purgeOldLogEntries("/errors.log", 86400, 500);  // cap to last 500 — a controller-down flood can blow past the age window
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
  MANAGEMENT_SSID       = doc["wifi_ssid"]               | "";
  MANAGEMENT_PASSWORD   = doc["wifi_password"]            | "";
  CONTROLLER_BASE_URL   = doc["omada_url"]                | "";
  CONTROLLER_ID         = doc["omada_controller_id"]      | "";
  ADMIN_USERNAME        = doc["omada_username"]           | "";
  ADMIN_PASSWORD        = doc["omada_password"]           | "";
  MASTER_MAC            = doc["master_mac"]               | "";
  NET_KEY               = doc["network_key"]              | 0ULL;
  MINUTES_PER_COIN      = doc["minutes_per_coin"]         | 24;
  PRICE_PER_COIN        = doc["price_per_coin"]           | 5;
  HEALTHCHECK_URL       = doc["healthchecks_url"]         | "";   // optional; empty = disabled
  if (MANAGEMENT_SSID.isEmpty())     { ESP_LOGE(TAG, "config.json missing: wifi_ssid");           return false; }
  if (MANAGEMENT_PASSWORD.isEmpty()) { ESP_LOGE(TAG, "config.json missing: wifi_password");       return false; }
  if (CONTROLLER_BASE_URL.isEmpty()) { ESP_LOGE(TAG, "config.json missing: omada_url");           return false; }
  if (CONTROLLER_ID.isEmpty())       { ESP_LOGE(TAG, "config.json missing: omada_controller_id"); return false; }
  if (ADMIN_USERNAME.isEmpty())      { ESP_LOGE(TAG, "config.json missing: omada_username");      return false; }
  if (ADMIN_PASSWORD.isEmpty())      { ESP_LOGE(TAG, "config.json missing: omada_password");      return false; }
  if (NET_KEY == 0)                  { ESP_LOGE(TAG, "config.json missing: network_key");         return false; }
  ESP_LOGI(TAG, "Config loaded: ssid=%s url=%s", MANAGEMENT_SSID.c_str(), CONTROLLER_BASE_URL.c_str());
  ESP_LOGI(TAG, "Healthcheck: %s", HEALTHCHECK_URL.isEmpty() ? "disabled (no healthchecks_url in config)" : "enabled");
  return true;
}

String maskedConfigJson() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) return "";
  JsonDocument doc;
  if (deserializeJson(doc, f)) { f.close(); return ""; }
  f.close();
  for (const char* k : {"wifi_password", "omada_password", "network_key"})
    if (!doc[k].isNull()) doc[k] = "••••••••";
  String json; serializeJson(doc, json);
  return json;
}

bool mergeAndSaveConfig(const String& body, String& err) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) { err = "Bad JSON"; return false; }

  // Start from the current config so omitted/blank fields (e.g. unchanged passwords) survive.
  JsonDocument merged;
  { File rf = LittleFS.open("/config.json", "r"); if (rf) { deserializeJson(merged, rf); rf.close(); } }
  for (JsonPair kv : doc.as<JsonObject>()) {
    // Never overwrite a secret with the masked placeholder posted back unchanged.
    if (kv.value().is<const char*>() && String(kv.value().as<const char*>()) == "••••••••") continue;
    // HTML form fields arrive as strings; preserve the type of an existing numeric field (e.g.
    // network_key) so a digit-string doesn't turn it into a string and brick loadConfig().
    JsonVariant cur = merged[kv.key()];
    if (kv.value().is<const char*>() && (cur.is<long long>() || cur.is<unsigned long long>())) {
      String s = kv.value().as<String>();
      bool numeric = s.length() > 0;
      for (size_t i = 0; i < s.length(); i++) if (!isdigit((unsigned char)s[i])) { numeric = false; break; }
      if (numeric) { merged[kv.key()] = (uint64_t)strtoull(s.c_str(), nullptr, 10); continue; }
    }
    merged[kv.key()] = kv.value();
  }

  // Refuse to write a config that would brick the device (can't join WiFi / reach Omada).
  for (const char* k : {"wifi_ssid", "wifi_password", "omada_url", "omada_controller_id",
                        "omada_username", "omada_password", "network_key"}) {
    bool emptyStr = merged[k].is<const char*>() && String(merged[k].as<const char*>()).isEmpty();
    if (merged[k].isNull() || emptyStr) { err = String("Missing required field: ") + k; return false; }
  }

  File f = LittleFS.open("/config.json", "w");
  if (!f) { err = "Failed to write"; return false; }
  serializeJson(merged, f); f.close();
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

void purgeOldLogEntries(const char* path, time_t maxAgeSeconds, int maxEntries) {
  if (!fsExists(path)) return;
  const char* tmpPath = "/log_tmp.dat";
  time_t cutoff = time(NULL) - maxAgeSeconds;

  // First pass: count entries that survive the age filter. A flood can blow past the age window
  // (e.g. the controller down spamming errors), so we also drop the oldest beyond maxEntries.
  int surviving = 0;
  {
    File src = LittleFS.open(path, FILE_READ);
    if (!src) return;
    while (src.available()) {
      String line = src.readStringUntil('\n');
      line.trim();
      if (line.isEmpty()) continue;
      int tsIdx = line.indexOf("\"ts\":");
      if (tsIdx >= 0 && (time_t)atol(line.c_str() + tsIdx + 5) < cutoff) continue;
      surviving++;
    }
    src.close();
  }
  int skip = (maxEntries > 0 && surviving > maxEntries) ? surviving - maxEntries : 0;

  File src = LittleFS.open(path, FILE_READ);
  if (!src) return;
  File dst = LittleFS.open(tmpPath, FILE_WRITE);
  if (!dst) { src.close(); return; }
  int kept = 0, removed = 0, idx = 0;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;
    int tsIdx = line.indexOf("\"ts\":");
    if (tsIdx >= 0 && (time_t)atol(line.c_str() + tsIdx + 5) < cutoff) { removed++; continue; }
    if (idx++ < skip) { removed++; continue; }   // oldest survivors beyond the count cap
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
  if (!f) {
    // DIAGNOSTIC: errno tells us why (EMFILE/ENFILE = handle exhaustion, ENOSPC = full, etc.).
    // Serial (not ESP_LOGE) to avoid recursing back into here.
    Serial.printf("E (%lu) files: appendErrorLog open FAILED errno=%d (%s) — dropped [%s] %s\r\n",
                  (unsigned long)millis(), errno, strerror(errno), tag, msg);
    return;
  }
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

void appendRefundLog(const String& mac, int coins, int minutes, const String& code) {
  File f = LittleFS.open("/refunds.log", FILE_APPEND);
  if (!f) {
    ESP_LOGE(TAG, "appendRefundLog: can't open /refunds.log — refund code %s (%d coins) NOT persisted", code.c_str(), coins);
    return;
  }
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
  if (!f) {
    ESP_LOGE(TAG, "appendSaleLog: can't open /vendo_history.json — sale (%d coins, node '%s') NOT persisted", coins, nodeMac.c_str());
    return;
  }
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
