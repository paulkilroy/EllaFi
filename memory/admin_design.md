# Admin Page Design

**Status: DESIGN COMPLETE — not yet implemented**

---

## Architecture

- Admin page (`data/admin.html`) is served by the ESP32 master
- Browser calls ESP32 endpoints only — ESP32 proxies all Omada calls server-side
- No CORS issues (same origin)
- No new servers

---

## Data Sources

| Data | Source | Speed |
|------|--------|-------|
| Active sessions count | `HOTSPOT_SESSION_CACHE` (RAM) | Instant |
| 24h hotspot sessions | Omada `/hotspot/sites/{siteId}/clients` with time filters | ~1-2 pages |
| Voucher groups | Omada `/hotspot/sites/{siteId}/voucherGroups` | Fast |
| Daily sales history | `/daily_sales.jsonl` on LittleFS | Instant |
| Coin-per-node today | `/coins_today.jsonl` on LittleFS | Instant |
| Errors / refunds | `/errors`, `/refunds` on LittleFS | Already working |
| Reseller list | `/sellers.json` on LittleFS | Instant |
| Config | `config.json` on LittleFS | Already exists |

---

## ESP32 Endpoints Needed

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/admin/sales` | GET | Last 30 days from `daily_sales.jsonl` |
| `/admin/vouchers` | GET | Live voucher groups from Omada (filtered by name convention) |
| `/admin/vouchers` | POST | Create new voucher group in Omada; body: `{pages, qty, duration_min, price, seller}` where `qty = pages × 130` (5 cols × 26 rows per A4 page) |
| `/admin/sellers` | GET/POST | Read/write `sellers.json` |
| `/admin/config` | GET | Serve config.json (mask passwords) |
| `/admin/config` | POST | Write config.json + esp_restart() |
| `/admin/nodes` | GET | Node health from UDP gossip state + system metrics; includes `vendoActive` and `voucherActive` counts ✅ IMPLEMENTED |
| `/admin/reboot` | POST | esp_restart() |
| `/errors` | GET | Already exists |
| `/refunds` | GET | Already exists |
| `/update` | POST | OTA: flashes master + saves /fw.bin to LittleFS + broadcasts NET_MSG_OTA_AVAILABLE to slaves ✅ IMPLEMENTED |

---

## LittleFS Files

### `/daily_sales.jsonl`
One JSON line appended per day at 2am. Stores daily totals + per-node + per-voucher-group breakdown:
```json
{"date":"2026-05-22","coins":42,"coin_min":1008,
 "by_node":{"AC:A7:04:26:4A:D4":20,"AC:A7:04:26:4B:12":22},
 "vouchers":[
   {"group_id":"69f49009...","name":"5P2H BELEN","used_today":13,"price":5,"commission":30},
   {"group_id":"6a026e1b...","name":"5P2H KANO","used_today":8,"price":5,"commission":30}
   // price = parsed from Omada group description JSON; commission = parsed from same description field
 ],
 "snapshots":{"69f49009...":970,"6a026e1b...":1547}}
```
`snapshots` = `expiredCount + inUseCount` per group at time of writing — used to compute next day's delta.

### `/coins_today.jsonl`
Appended in real-time by master on every successful `finalizeCoinSession`. Deleted at 2am after aggregation.
```json
{"ts":1748000000,"node":"AC:A7:04:26:4B:12","mac":"36-39-18-96-8C-D6","min":48}
```
- `node` = MAC of the ESP32 that inserted the coin (master's own MAC for direct inserts, slave MAC from `coin_inserted` UDP message)

### `/sellers.json`
Holds both voucher resellers and vendo node nicknames. Node nicknames stored here (not browser localStorage) so they persist across devices.
```json
{
  "sellers": [
    {"name":"KANO","commission":30},
    {"name":"DARNA","commission":30},
    {"name":"BELEN","commission":30}
  ],
  "nodes": [
    {"mac":"AC:A7:04:26:4A:D4","nickname":"Bakhaw"},
    {"mac":"AC:A7:04:26:4B:12","nickname":"Daram"},
    {"mac":"AC:A7:04:26:4C:20","nickname":"Samar"}
  ]
}
```

---

## Nightly Task (2am)

Fires using NTP time (`time(NULL)`), checked in `loop()` or a FreeRTOS timer.

1. Query Omada `voucherGroups` → get `expiredCount + inUseCount` per group
2. Filter to groups matching name convention (see below)
3. For each group: `used_today = current_count - yesterday_snapshot[group_id]`
4. Read `/coins_today.jsonl` → aggregate by node MAC
5. Append record to `/daily_sales.jsonl`
6. Delete `/coins_today.jsonl`

---

## Voucher Group Naming Convention

**Format: `AUTOGEN: {PRICE}P{DURATION_HOURS}H {SELLER_NAME}`**

Examples: `AUTOGEN: 5P2H KANO`, `AUTOGEN: 5P2H DARNA`, `AUTOGEN: 10P4H BELEN`

Parse regex: `^AUTOGEN: (\d+)P(\d+)H (.+)$` → groups: price hint, duration hint, seller name

- Name is **operator reference only** — price authoritative from `unitPrice`, commission from `description`
- Groups not matching this regex are **ignored** by all reporting and the nightly task
- Existing non-conforming groups ("Family", "Voucher ni Ella", etc.) are silently skipped
- Admin page assembles name automatically from form fields (price, duration, seller)

### Omada Group Fields (confirmed)
- **`unitPrice`** — present in GROUP DETAIL response only, absent from LIST response; **not supported on Omada Essential** — do not rely on it for pricing
- **`description`** — freeform text; we store `{"price":5,"commission":30}` JSON here; parse to get both price and commission rate
- **`unusedCount`**, **`usedCount`**, **`totalCount`** — voucher counts (top-level in LIST response)
- `unusedAmount`, `usedAmount`, `totalAmount`, `currency` — present in DETAIL response only, may be absent in LIST; not used by our reporting logic
- Price stored in `description` JSON field is authoritative (Omada Essential has no native `unitPrice` support); name suffix (e.g. `5P2H`) is human-readable reference only

### Commission Storage
- **Current commission** → `sellers.json` (used when creating new batches)
- **Historical commission** → stored in Omada group `description` field as `"commission 30%"` at batch creation time
- If a seller's commission changes, old batches retain their original rate; new batches use the updated rate

---

## Omada API Endpoints Used

| Endpoint | Purpose |
|----------|---------|
| `GET /api/v2/hotspot/sites/{siteId}/clients?sorts.end=desc&currentPageSize=100&currentPage=N` | Active + recent sessions (paginated, stop when all expired) |
| `GET /api/v2/hotspot/sites/{siteId}/clients?filters.timeStart=X&filters.timeEnd=Y` | Day-range query — used for dashboard "Revenue Today" (last 24h) |
| `GET /api/v2/hotspot/sites/{siteId}/voucherGroups` | List all voucher groups — returns group metadata |
| `GET /api/v2/hotspot/sites/{siteId}/voucherGroups/{groupId}?currentPage=1&currentPageSize=100` | Group detail + paginated voucher codes |
| `POST /api/v2/hotspot/sites/{siteId}/voucherGroups` | Create new voucher group (TBD — endpoint not yet confirmed) |

### Session authTypes
- `authType: 4` = External Portal Server (vendo / our coin sessions)
- `authType: 3` = Voucher

### Confirmed VoucherGroup Fields (confirmed from user-pasted response)
```json
{
  "name": "5P 2H Kano -- TESTING",
  "createdTime": 1779432311310,
  "creatorName": "paulkilroy@gmail.com",
  "duration": 120,
  "durationType": 1,
  "unusedCount": 10,
  "inUseCount": 0,
  "expiredCount": 0,
  "totalCount": 10,
  "unitPrice": "5",
  "unusedAmount": "50",
  "usedAmount": "0",
  "totalAmount": "50",
  "currency": "PHP",
  "description": "commission 10%",
  "statisticsCount": {
    "totalUnusedCount": 10,
    "totalInUseCount": 0,
    "totalExpiredCount": 0,
    "totalStatisticsCount": 10
  },
  "totalRows": 10,
  "currentPage": 1,
  "currentSize": 10,
  "data": [
    { "id": "6a0ffb77...", "code": "98267625", "status": 0,
      "startTime": 0, "endTime": 9223372036854775807,
      "timeLeftSec": 7200, "timingByClientUsage": false }
  ]
}
```
- `createdTime` — Unix epoch **ms** ✅ use for "Generated" column, no need to store in description
- `duration` — **minutes** (120 = 2h) ✅ use directly, no need to parse from name
- `unitPrice` — price per voucher (string, PHP)
- `unusedAmount` / `totalAmount` — PHP value (count × unitPrice), not count
- `statisticsCount.totalStatisticsCount` — total voucher code count
- `data[]` — paginated voucher codes; `code` is an 8-digit **numeric** string (e.g. `"56589058"`) — NOT alphanumeric; `status: 0` = unused, other = used
- `totalRows` — total voucher count in group (top-level in result); use for pagination; same value as `totalCount` (also top-level) and `statisticsCount.totalStatisticsCount` (nested)
- `currentPage`, `currentSize` — pagination info (top-level in result)
- To fetch all codes for printing: paginate with `currentPageSize=130` (one page = one print page)

### Confirmed VoucherGroup LIST (GET)

**URL:** `GET /api/v2/hotspot/sites/{siteId}/voucherGroups?currentPage=1&currentPageSize=100`

**Response structure:**
```json
{
  "errorCode": 0,
  "result": {
    "currentPage": 1,
    "currentSize": 10,
    "totalRows": 3,
    "groupAmount": 5000,
    "data": [
      {
        "id": "6a13ce23c7f118353f359694",
        "name": "AUTOGEN: 5P2H Kano",
        "description": "commission 10%",
        "duration": 120,
        "totalCount": 130,
        "unusedCount": 130,
        "usedCount": 0,
        "createdTime": 1779682851340,
        "applyToAllPortals": true
      }
    ]
  }
}
```

Key confirmed facts:
- `id` is top-level per group (use for detail/codes fetch)
- `totalCount` is top-level per group (total voucher codes in the group)
- No `statisticsCount` or `unitPrice` in list response — those are detail-only (or absent entirely; see below)
- `totalRows` at result level = number of groups (for group list pagination), NOT voucher count
- Default page size is 10 — must pass `currentPageSize=100` to get more than 10 groups
- `description` is freeform — we store commission as `{"price":5,"commission":30}` JSON in this field
- **No `unitPrice` field in either list or create payload** — Omada Essential does not support pricing; price must come from `description` or name parsing

### Confirmed VoucherGroup CREATE (POST)

**URL:** `POST /api/v2/hotspot/sites/{siteId}/voucherGroups`

**Response:** `{"errorCode":0,"msg":"Success.","result":{"id":"6a13ccbe386a14129cd00b20"}}` — just the new group id.

**Required request body (confirmed from browser capture):**
```json
{
  "codeLength": 8,
  "codeForm": [0],
  "amount": 100,
  "name": "test2",
  "type": 0,
  "logout": true,
  "downLimitUnit": 0,
  "upLimitUnit": 0,
  "trafficLimitEnable": false,
  "trafficLimit": null,
  "trafficLimitUnit": 0,
  "voucherValidityEnable": false,
  "upTimeLimitEnable": false,
  "durationType": 1,
  "duration": 120,
  "description": "desfcripion here",
  "maxUsers": 1,
  "trafficLimitFrequency": 1,
  "portalIds": [],
  "applyToAllPortals": true,
  "validityType": 0,
  "startTime": "00:00",
  "endTime": "23:59",
  "scheduleTime": 0,
  "weeklyEnableDays": {"1":true,"2":true,"3":true,"4":true,"5":true,"6":true,"7":true},
  "pattern": {"patternType":0,"position":0,"ssidNetworkEnable":false,"durationEnable":false,"limitEnable":false,"logoPictureId":null,"logoSize":62}
}
```

- `amount` = number of vouchers (NOT `totalCount`)
- `duration` = minutes
- No `unitPrice` field in create payload — Omada Essential does not support pricing natively
- `description` is freeform — we store `{"price":5,"commission":30}` JSON here (confirmed approach)
- `codeLength:8` and `codeForm:[0]` (digits only) are required
- All other fields should be sent as shown — Omada is strict about required fields

---

## Dashboard Data Flow

Dashboard makes **no Omada calls** — all data from memory or fast file reads.

- **Active sessions**: `HOTSPOT_SESSION_CACHE` (vendo) + Omada client cache already in RAM (voucher)
- **Vendo today**: in-memory coin session counter incremented on each `finalizeCoinSession`, reset at 2am
- **Errors / Refunds (24h)**: count lines from LittleFS `/errors` and `/refunds`
- **Node health + system health**: from `/admin/nodes` — UDP gossip state + master ESP32 API calls

---

## Voucher Page Data Flow

- **Reseller list / seller dropdown**: `sellers.json` → `sellers[]`
- **Voucher batch list**: live from Omada `voucherGroups`, filtered by `AUTOGEN:` prefix; price and commission both parsed from `description` JSON field, seller from name regex, pages = `totalCount / 130`
- **Create batch**: form → ESP32 → Omada POST → return codes for printing; body `{pages, qty=pages×130, duration_min, price, seller}`
- **Print**: 5 columns × 26 rows = 130 cards per A4 page (`CARDS_PER_PAGE = 130`)

---

## UI Field → Data Source Mapping

### Dashboard Tab

**No Omada calls on dashboard load — everything from memory or fast file reads.**

| Field | Element | Endpoint | Source detail |
|-------|---------|----------|---------------|
| Vendo Active | `d-sessions-vendo` | `GET /admin/nodes` | `HOTSPOT_SESSION_CACHE` count |
| Voucher Active | `d-sessions-voucher` | `GET /admin/nodes` | `VOUCHER_ACTIVE_COUNT` global (see Firmware Change below) |
| Errors (24h) | `d-errors` | `GET /errors` | ✅ wired — line count |
| Refunds (24h) | `d-refunds` | `GET /refunds` | ✅ wired — line count |

**Node Health table** — `GET /admin/nodes`

| Column | Source |
|--------|--------|
| Status dot | UDP gossip: last seen < 30s |
| Role | Matches `MASTER_MAC` from config |
| Nickname | `sellers.json` → `nodes[].nickname` for matching MAC |
| IP | UDP gossip state |
| Uptime | UDP gossip state |
| Free Heap | `ESP.getFreeHeap()` per node via gossip |
| RSSI | UDP gossip state |

**System Health table** — `GET /admin/nodes` (master fields only)

| Metric | Source |
|--------|--------|
| Free Heap | `ESP.getFreeHeap()` |
| Min Free Heap (boot) | `ESP.getMinFreeHeap()` — worst case since last restart |
| Active Sockets | firmware counter tracking open HTTP connections |
| Omada Session Age | `millis() - lastOperatorLoginMs` |
| Last Cache Refresh | `millis() - lastCacheRefreshMs` |
| LittleFS | `LittleFS.usedBytes()` / `LittleFS.totalBytes()` |
| NTP | `time(NULL) > 1000000000` → "Synced" / "Not synced" |

---

### Sales Tab — `GET /admin/sales` → `daily_sales.jsonl`

**This Week — Nodes & Sellers table** (moved from Dashboard)

`/admin/sales` returns 30 days. JS slices: days 1–7 = this week, days 8–14 = last week. All client-side — no extra endpoint.

| Column | Node rows | Seller rows |
|--------|-----------|-------------|
| Name | `sellers.json` nodes nickname / MAC | `sellers.json` sellers name |
| Type | "Vendo" | "Voucher" |
| Activity | sum `by_node[mac]` over days 1–7 | sum `vouchers[].used_today` where seller name matches, days 1–7 |
| Revenue | activity × ₱1 | `used_today × price` summed |
| vs Last Week | same fields summed over days 8–14, compare to days 1–7 | same |

---

### Sales Tab — `GET /admin/sales` → `daily_sales.jsonl`

- **Today not shown** — completed days only (2am granularity)
- Always 30 days — no period selector

| Field | Source |
|-------|--------|
| Chart bar — Vendo (blue) | `coins` per day (₱) |
| Chart bar — Vouchers (green) | `sum(vouchers[].used_today × price)` per day |
| Table: Date | `date` |
| Table: Vendo | `coins` |
| Table: Vouchers | `sum(vouchers[].used_today)` (count) |
| Table: Total Minutes | `coin_min + sum(vouchers[].used_today × duration_min)` where `duration_min` = `duration` from Omada (minutes), stored in `daily_sales.jsonl` voucher entry |
| Table: Total Revenue | `coins + sum(vouchers[].used_today × price)` |
| Table: Leader | top performer that day — `max(by_node values, vouchers[].used_today × price)` → show name + ₱ |

---

### Vouchers Tab

**Generate form** — submits to `POST /admin/vouchers`

| Field | Source |
|-------|--------|
| Pages input | User input; `qty = pages × 130` sent to Omada |
| Duration | User input → minutes |
| Price (₱) | User input → `unitPrice` in Omada group |
| Seller dropdown | `GET /admin/sellers` → `sellers.json` sellers[] |
| New seller | User input → appended to `sellers.json` |

**Voucher Batches table** — `GET /admin/vouchers` → Omada voucherGroups filtered by `AUTOGEN:`

| Column | Source |
|--------|--------|
| Generated | `createdTime` (epoch ms) → formatted date |
| Seller | Regex `^AUTOGEN: \d+P\d+H (.+)$` on group name |
| Pages | `totalCount / 130` (from LIST response) |
| Duration | `duration` field (minutes) → `durLabelShort()` |
| Price | parsed from `description` JSON field |
| Print | `GET /admin/vouchers/{groupId}?currentPageSize=130` per page, paginated |

---

### Logs Tab — ✅ fully wired

| Section | Endpoint | Columns |
|---------|----------|---------|
| Error Log | `GET /errors` | Time, Tag, Message |
| Refund Log | `GET /refunds` | Time, MAC, Coins, Minutes, Code |

---

### Settings Tab — `GET /admin/config` / `POST /admin/config`

| Field | config.json key | Notes |
|-------|----------------|-------|
| SSID | `wifi_ssid` | |
| WiFi Password | `wifi_password` | masked on GET |
| Controller URL | `omada_url` | |
| Controller ID | `omada_controller_id` | |
| Operator Username | `omada_operator_username` | |
| Operator Password | `omada_operator_password` | masked on GET |
| Admin Username | `omada_username` | |
| Admin Password | `omada_password` | masked on GET |
| Master MAC | `master_mac` | |
| Network Key | `network_key` | |
| Minutes per Coin | `minutes_per_coin` | |

---

### System Tab — ✅ fully wired

| Action | Endpoint |
|--------|----------|
| OTA Upload | `POST /update` |
| Reboot | `POST /admin/reboot` |
| Program Mode | `GET /program` |

---

## Firmware Changes Required for Admin

### `VOUCHER_ACTIVE_COUNT` — for Dashboard "Voucher Active" card

`refreshHotspotSessionCache()` currently skips authType=3 (voucher) sessions entirely (`omada.cpp:499`). To populate the dashboard counter without a live Omada call, add a second pass in `refreshHotspotSessionCache()` that counts active voucher sessions from the already-fetched hotspot data:

```cpp
// globals.h — add:
extern volatile int VOUCHER_ACTIVE_COUNT;

// omada.cpp — in refreshHotspotSessionCache(), after getHotspotClientsJson():
int voucherCount = 0;
uint64_t now = nowEpochMillis();
for (JsonObject hs : hotspotDoc["result"]["data"].as<JsonArray>()) {
    if (hs["authType"].as<int>() == OMADA_AUTH_TYPE_VOUCHER
        && hs["end"].as<uint64_t>() > now)
        voucherCount++;
}
VOUCHER_ACTIVE_COUNT = voucherCount;  // single 32-bit write, hardware-atomic on ESP32
```

No extra Omada call — reuses the `hotspotDoc` already fetched by the refresh. Define `OMADA_AUTH_TYPE_VOUCHER 3` alongside the existing `OMADA_AUTH_TYPE_EXTERNAL_PORTAL 4`.
