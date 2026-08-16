#pragma once

// ── Durable money record (NVS / "NVRAM") ──────────────────────────────────────
// The financial source-of-truth lives here, NOT on LittleFS — because a web filesystem
// upload wipes LittleFS but leaves the NVS partition untouched. It is one JSON blob in
// the "money" namespace (see the schema in money_store.cpp).
//
// Why a per-GROUP ledger (keyed by Omada groupId) instead of a per-month rollup:
//   • idempotent — the nightly job upserts each live group by id, so re-running never
//     double-counts;
//   • deletion-safe — a group that disappears from the controller is marked `deleted`
//     and frozen at its last-known usedCount, never dropped. That is the whole point:
//     the monthly report reads THIS, so deleting an Omada group can't rewrite history.
//
// Reports aggregate the ledger by month/seller at read time; the ledger itself is small
// (a few dozen groups + one income row per month) and stays well under the 20 KB NVS budget.

#include "globals.h"
#include <map>
#include <set>

// Load the record from NVS into RAM. Call once at boot, before any reader/writer. Returns
// false only on a hard NVS failure (record still usable — starts empty).
bool moneyStoreBegin();

// Persist the in-RAM record back to NVS. Call after a batch of upserts (e.g. end of a rollup).
bool moneyStoreSave();

// Upsert one group's frozen contribution. revenue/commission are (re)derived from used×price×rate,
// so calling this again for the same id with a newer usedCount just refreshes it. Sets deleted=false.
void moneyUpsertGroup(const String& id, const String& name, const String& seller,
                      const String& month, int used, int price, int rate, time_t seen);

// Mark every ledger group NOT in `liveIds` as deleted (freeze it). Call after a rollup that
// enumerated the live groups, so vanished groups are preserved rather than silently lost.
void moneyMarkAbsent(const std::set<String>& liveIds);

// Set the monthly income totals (pesos): vendo = coin income, voucher = redemption income.
void moneySetIncome(const String& month, long vendo, long voucher);

// The whole record as JSON — for the report handler and for off-device backup export.
String moneyRecordJson();

// A group's usedCount as of the LAST nightly rollup (0 if unknown). Live count minus this =
// redemptions so far today — the sales report's per-seller "today" attribution.
long moneyGroupUsed(const String& id);

// Merge an imported record (Mac-side rollup of the voucher backups, incl. already-deleted groups)
// into the store. Upserts groups by id (higher `used` wins) and income by month — a month field the
// import omits keeps its stored value (so a voucher-only rollup can't zero vendo income). Persists.
// Returns groups touched, or -1 on parse error (sets err).
int moneyImport(const String& json, String& err);

// Aggregate /vendo_history.json into per-month peso totals (coins × PRICE_PER_COIN), line by line.
// Used by the migrate dry-run and the nightly rollup.
void moneyVendoMonths(std::map<String, long>& out);

// Nightly rollup (master, 2:45am — before the 3am reboot): upsert live AUTOGEN voucher groups at
// COMMISSION_RATE_PERCENT, freeze groups that vanished from the controller (only when the fetch
// succeeded), refresh income months (vendo from the sale log, voucher from the ledger), persist.
void moneyNightlyRollup();

// One-time vendo-history migration guard (stored inside the record).
bool moneyVendoMigrated();
void moneyMarkVendoMigrated();
