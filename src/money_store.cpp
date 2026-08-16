#include "money_store.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_log.h>
#include <memory>
#include "logger.h"   // must be included LAST — see logger.h

static const char* TAG = "money";
static const char* NS  = "money";     // NVS namespace (survives LittleFS/OTA writes; only a full erase clears it)
static const char* KEY = "rec";       // single blob holding the whole record

static Preferences   moneyPrefs;
static JsonDocument  MONEY;           // in-RAM record — small (a few dozen groups); default heap allocator
static bool          moneyReady = false;

// Guarantee the top-level shape so callers can index groups/income without null checks.
static void ensureShape() {
  if (MONEY["v"].isNull())               MONEY["v"] = 1;
  if (!MONEY["groups"].is<JsonObject>()) MONEY["groups"].to<JsonObject>();
  if (!MONEY["income"].is<JsonObject>()) MONEY["income"].to<JsonObject>();
}

bool moneyStoreBegin() {
  moneyReady = moneyPrefs.begin(NS, /*readOnly=*/false);
  if (!moneyReady) { ESP_LOGE(TAG, "NVS begin failed for '%s' — record will be RAM-only", NS); ensureShape(); return false; }

  size_t len = moneyPrefs.getBytesLength(KEY);
  if (len) {
    std::unique_ptr<uint8_t[]> buf(new uint8_t[len]);
    if (moneyPrefs.getBytes(KEY, buf.get(), len) == len) {
      DeserializationError e = deserializeJson(MONEY, buf.get(), len);
      if (e) { ESP_LOGE(TAG, "record corrupt (%s) — starting empty", e.c_str()); MONEY.clear(); }
    }
  }
  ensureShape();
  ESP_LOGI(TAG, "loaded: %u groups, %u income months (%u bytes)",
           (unsigned)MONEY["groups"].as<JsonObject>().size(),
           (unsigned)MONEY["income"].as<JsonObject>().size(), (unsigned)len);
  return true;
}

bool moneyStoreSave() {
  if (!moneyReady) return false;
  MONEY["updated"] = (long)time(NULL);
  String out;
  serializeJson(MONEY, out);
  size_t n = moneyPrefs.putBytes(KEY, out.c_str(), out.length());
  if (n != out.length()) { ESP_LOGE(TAG, "save failed: wrote %u/%u bytes", (unsigned)n, (unsigned)out.length()); return false; }
  ESP_LOGI(TAG, "saved %u bytes", (unsigned)out.length());
  return true;
}

void moneyUpsertGroup(const String& id, const String& name, const String& seller,
                      const String& month, int used, int price, int rate, time_t seen) {
  JsonObject g = MONEY["groups"][id.c_str()].to<JsonObject>();   // reset+recreate; every field set below
  g["name"]       = name;
  g["seller"]     = seller;
  g["month"]      = month;
  g["used"]       = used;
  g["price"]      = price;
  g["rate"]       = rate;
  g["revenue"]    = (long)used * price;
  g["commission"] = (long)used * price * rate / 100;
  g["deleted"]    = false;
  g["seen"]       = (long)seen;
}

void moneyMarkAbsent(const std::set<String>& liveIds) {
  for (JsonPair kv : MONEY["groups"].as<JsonObject>())
    if (liveIds.count(String(kv.key().c_str())) == 0)
      kv.value()["deleted"] = true;   // frozen at its last-known used/commission
}

void moneySetIncome(const String& month, long vendo, long voucher) {
  JsonObject m = MONEY["income"][month.c_str()].to<JsonObject>();
  m["vendo"]   = vendo;
  m["voucher"] = voucher;
}

long moneyGroupUsed(const String& id) {
  return MONEY["groups"][id]["used"] | 0L;
}

String moneyRecordJson() {
  String out;
  serializeJson(MONEY, out);
  return out;
}

int moneyImport(const String& json, String& err) {
  JsonDocument in;
  DeserializationError e = deserializeJson(in, json);
  if (e) { err = e.c_str(); return -1; }

  int touched = 0;
  for (JsonPair kv : in["groups"].as<JsonObject>()) {
    JsonObject s = kv.value().as<JsonObject>();
    const char* id = kv.key().c_str();
    JsonObject g = MONEY["groups"][id];
    // Higher usedCount wins: an import must never lower a count the live device already accrued.
    if (!g.isNull() && (int)(g["used"] | 0) >= (int)(s["used"] | 0)) continue;
    moneyUpsertGroup(id, s["name"] | "", s["seller"] | "", s["month"] | "",
                     s["used"] | 0, s["price"] | 0, s["rate"] | 0, (time_t)(s["seen"] | 0L));
    if (s["deleted"] | false) MONEY["groups"][id]["deleted"] = true;
    touched++;
  }
  for (JsonPair kv : in["income"].as<JsonObject>()) {
    JsonObject s   = kv.value().as<JsonObject>();
    JsonObject cur = MONEY["income"][kv.key().c_str()];
    // A side the import omits keeps its stored value — a voucher-only rollup must not zero vendo.
    long vendo   = s["vendo"].isNull()   ? (long)(cur["vendo"]   | 0L) : (long)s["vendo"];
    long voucher = s["voucher"].isNull() ? (long)(cur["voucher"] | 0L) : (long)s["voucher"];
    moneySetIncome(kv.key().c_str(), vendo, voucher);
    touched++;
  }
  moneyStoreSave();
  return touched;
}

// ── Vendo history aggregation ─────────────────────────────────────────────────

static String monthOf(time_t t) {
  struct tm* m = localtime(&t);
  char buf[8];
  strftime(buf, sizeof(buf), "%Y-%m", m);
  return String(buf);
}

void moneyVendoMonths(std::map<String, long>& out) {
  File f = LittleFS.open("/vendo_history.json", FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() < 5) continue;
    JsonDocument d;
    if (deserializeJson(d, line)) continue;
    long ts = d["ts"] | 0L;
    int coins = d["coins"] | 0;
    if (ts <= 0 || coins <= 0) continue;
    out[monthOf((time_t)ts)] += (long)coins * PRICE_PER_COIN;
  }
  f.close();
}

bool moneyVendoMigrated()     { return !MONEY["vendoMigrated"].isNull(); }
void moneyMarkVendoMigrated() { MONEY["vendoMigrated"] = (long)time(NULL); }

// ── Nightly rollup ────────────────────────────────────────────────────────────

void moneyNightlyRollup() {
  // 1. Live voucher groups from the controller (same AUTOGEN parsing as the admin voucher list)
  JsonDocument raw = getVoucherGroupsJson();
  bool fetched = !raw.isNull() && raw["errorCode"].as<int>() == 0;
  if (fetched) {
    std::set<String> live;
    std::map<String, long> sellerDay;   // redeemed-as-sold: pesos accrued per seller since last rollup
    for (JsonObject g : raw["result"]["data"].as<JsonArray>()) {
      String name = g["name"].as<String>();
      if (!name.startsWith("AUTOGEN: ")) continue;
      String rest   = name.substring(9);
      int spaceIdx  = rest.indexOf(' ');
      String seller = spaceIdx >= 0 ? rest.substring(spaceIdx + 1) : "";
      int price = 0;
      {
        JsonDocument descDoc;
        if (deserializeJson(descDoc, g["description"].as<String>()) == DeserializationError::Ok)
          price = descDoc["price"] | 0;
      }
      String id      = g["id"].as<String>();
      int    used    = g["usedCount"] | 0;
      // The ledger still holds the PREVIOUS rollup's count — the delta is what this seller's
      // customers redeemed since then, i.e. the day that just ended (rollup runs 2:45am).
      long prevUsed = MONEY["groups"][id]["used"] | 0L;
      if (used > prevUsed && !seller.isEmpty() && price > 0)
        sellerDay[seller] += (long)(used - prevUsed) * price;
      time_t created = (time_t)(g["createdTime"].as<uint64_t>() / 1000ULL);
      moneyUpsertGroup(id, name, seller, monthOf(created),
                       used, price, COMMISSION_RATE_PERCENT, time(NULL));
      live.insert(id);
    }
    moneyMarkAbsent(live);   // only on a successful fetch — a controller outage must not freeze the ledger
    // Daily per-seller journal for the sales report's Leader column (cosmetic → LittleFS is fine;
    // the money source-of-truth stays in NVS above). Reader derives the day from ts.
    if (!sellerDay.empty()) {
      File f = LittleFS.open("/seller_days.json", FILE_APPEND);
      if (f) {
        JsonDocument d;
        d["ts"] = (long)time(NULL);
        JsonObject sm = d["sellers"].to<JsonObject>();
        for (auto& kv : sellerDay) sm[kv.first.c_str()] = kv.second;
        serializeJson(d, f);
        f.print('\n');
        f.close();
      }
    }
  } else {
    ESP_LOGW(TAG, "rollup: voucher fetch failed — ledger groups left as-is");
  }

  // 2. Income refresh: vendo from the sale log, voucher aggregated from the ledger by month
  std::map<String, long> vendo;
  moneyVendoMonths(vendo);
  std::map<String, long> voucher;
  for (JsonPair kv : MONEY["groups"].as<JsonObject>())
    voucher[String(kv.value()["month"] | "")] += (long)(kv.value()["revenue"] | 0L);

  std::set<String> months;
  for (auto& kv : vendo)   months.insert(kv.first);
  for (auto& kv : voucher) months.insert(kv.first);
  for (JsonPair kv : MONEY["income"].as<JsonObject>()) months.insert(String(kv.key().c_str()));
  for (const String& m : months) {
    JsonObject cur = MONEY["income"][m.c_str()];
    long v = vendo.count(m) ? vendo[m] : (long)(cur["vendo"] | 0L);   // months not in the log keep their (migrated) value
    moneySetIncome(m, v, voucher.count(m) ? voucher[m] : 0L);
  }
  moneyStoreSave();
  ESP_LOGI(TAG, "rollup done: fetch=%d, %u groups, %u income months",
           (int)fetched, (unsigned)MONEY["groups"].as<JsonObject>().size(), (unsigned)months.size());
}
