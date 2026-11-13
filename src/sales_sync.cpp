#include "sales_sync.h"
#include "globals.h"
#include "omada.h"            // ADMIN_PASSWORD
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <mbedtls/md.h>

static const char* TAG = "SalesSync";
static const int   BATCH_LINES = 200;                     // ~9 KB of JSON per POST
static const unsigned long SYNC_EVERY_MILLIS = 10 * MILLIS_PER_MINUTE;
static const unsigned long CATCHUP_EVERY_MILLIS = MILLIS_PER_MINUTE;
static const size_t TLS_MIN_BLOCK = 45000;               // same line as the TIGHT resource warning in loop()

static String hmacHex(const String& key, const String& msg) {
  uint8_t out[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                  (const uint8_t*)key.c_str(), key.length(), (const uint8_t*)msg.c_str(), msg.length(), out);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", out[i]);
  return String(hex);
}

void salesSyncTick() {
  static unsigned long lastRun = 0, every = 0;
  if (!IS_MASTER || TRACKER_URL.isEmpty() || WiFi.status() != WL_CONNECTED || time(NULL) < NTP_EPOCH_MIN) return;
  if (lastRun && millis() - lastRun < every) return;
  lastRun = millis(); every = SYNC_EVERY_MILLIS;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < TLS_MIN_BLOCK) {
    ESP_LOGD(TAG, "skipped: DRAM too tight for TLS");
    return;
  }

  Preferences p;
  p.begin("salesync", true);
  long acked = p.getLong("acked", 0);
  p.end();

  File f = LittleFS.open("/vendo_history.json", FILE_READ);
  if (!f) return;
  String body = "{\"master\":\"" + WiFi.macAddress() + "\",\"sales\":[";
  int n = 0; long maxTs = acked;
  while (f.available() && n < BATCH_LINES) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;
    JsonDocument d;
    if (deserializeJson(d, line)) continue;
    long ts = d["ts"] | 0L;
    if (ts <= acked) continue;
    if (n++) body += ',';
    body += line;
    if (ts > maxTs) maxTs = ts;
  }
  bool more = f.available();
  f.close();
  if (!n) return;
  body += "]}";
  // a full batch may end mid-second: re-send that second next time (the tracker dedupes), and catch up fast
  if (n == BATCH_LINES && more) { maxTs -= 1; every = CATCHUP_EVERY_MILLIS; }

  WiFiClientSecure wc; wc.setInsecure();
  HTTPClient h;
  h.setConnectTimeout(10000); h.setTimeout(15000);
  if (!h.begin(wc, TRACKER_URL)) return;
  h.addHeader("Content-Type", "application/json");
  h.addHeader("X-EllaFi-Sig", hmacHex(ADMIN_PASSWORD, body));
  int code = h.POST(body);
  String resp = code > 0 ? h.getString() : h.errorToString(code);
  h.end();
  if (code == 200) {
    p.begin("salesync", false);
    p.putLong("acked", maxTs);
    p.end();
    ESP_LOGI(TAG, "sent %d sale(s), acked through %ld%s", n, maxTs, every == CATCHUP_EVERY_MILLIS ? " (catching up)" : "");
  } else {
    ESP_LOGW(TAG, "tracker refused %d sale(s): HTTP %d %.120s", n, code, resp.c_str());
  }
}
