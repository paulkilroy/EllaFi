# `/{omadaId}/api/v2/hotspot/sites/{siteId}/voucherGroups`

Voucher group create / list / detail / delete. Admin or operator session.

---

## POST (create) — request bodies, 2026-06-09

The **only** difference between "By Time" and "By Usage" timing is `upTimeLimitEnable`:
- `false` → **By Time** (countdown from first use, even while disconnected)
- `true`  → **By Usage** (countdown only while client is online)

`timingByClientUsage` (seen in GET responses on individual codes) is a runtime state flag, NOT
a create field — don't confuse them. The timing mode is NOT echoed back in the group GET.

**By Time** request body:
```json
{"codeLength":6,"codeForm":[0],"amount":10,"name":"Voucher by Time","type":0,"logout":true,
 "rateLimitId":"{REDACTED}","downLimitUnit":0,"upLimitUnit":0,"trafficLimitEnable":false,
 "trafficLimit":null,"trafficLimitUnit":0,"voucherValidityEnable":false,"upTimeLimitEnable":false,
 "durationType":1,"duration":120,"description":null,"maxUsers":1,"trafficLimitFrequency":1,
 "portalIds":["{REDACTED}"],"applyToAllPortals":true,"validityType":0,"startTime":"00:00",
 "endTime":"23:59","scheduleTime":0,
 "weeklyEnableDays":{"1":true,"2":true,"3":true,"4":true,"5":true,"6":true,"7":true},
 "pattern":{"patternType":0,"position":0,"ssidNetworkEnable":false,"durationEnable":false,
            "limitEnable":false,"logoPictureId":null,"logoSize":62}}
```

**By Usage** request body — identical except:
```json
"upTimeLimitEnable":true
```

---

## GET (detail) — `?currentPage=N&currentPageSize=130`, 2026-06-09

Daily/per-code data. `durationType:1`, `duration` in minutes. Codes are 6–8 digit numeric strings.

```json
{
  "errorCode": 0, "msg": "Success.",
  "result": {
    "id": "{REDACTED-groupId}", "omadacId": "{REDACTED}", "siteId": "{REDACTED-siteId}",
    "name": "Test Logout Voucher", "createdTime": 1781042723801,
    "creatorName": "{REDACTED-email}", "creatorRole": "Owner",
    "type": 0, "maxUsers": 1, "durationType": 1, "duration": 120,
    "rateLimitId": "{REDACTED}", "trafficLimitFrequency": 1,
    "unusedCount": 4, "inUseCount": 1, "expiredCount": 0, "totalCount": 5,
    "portalNames": [], "portalIds": [], "applyToAllPortals": true,
    "statisticsCount": { "totalUnusedCount": 4, "totalInUseCount": 1, "totalExpiredCount": 0, "totalStatisticsCount": 5 },
    "logout": true, "validityType": 0,
    "pattern": { "patternType": 0, "patternCode": "{REDACTED}", "position": 0, "logoSize": 62,
                 "ssidNetworkEnable": false, "ssidNetworkNameList": [], "durationEnable": false,
                 "limitEnable": false, "validity": "Permanent" },
    "totalRows": 5, "currentPage": 1, "currentSize": 10,
    "data": [
      { "id": "{REDACTED}", "code": "{REDACTED}", "status": 1, "startTime": 1781044739600,
        "endTime": 9223372036854775807, "logout": true, "timeUsedSec": 49, "timeLeftSec": 7151,
        "timingByClientUsage": false },
      { "id": "{REDACTED}", "code": "{REDACTED}", "status": 0, "startTime": 0,
        "endTime": 9223372036854775807, "logout": true, "timeUsedSec": 0, "timeLeftSec": 7200,
        "timingByClientUsage": false }
      // ... remaining unused codes identical shape ...
    ]
  }
}
```
- `status`: 0 = unused, 1 = in use. `endTime` 9223372036854775807 = Int64 max (no hard expiry).
- `timeUsedSec` / `timeLeftSec` track the duration countdown per code.

---

## DELETE — `DELETE /{omadaId}/api/v2/hotspot/sites/{siteId}/voucherGroups/{groupId}`, 2026-06-09

Response (also `200 OK`):
```json
{"errorCode":0,"msg":"Success."}
```

---

## Earlier captures (migrated from admin_design.md)

### GET detail — group WITH `unitPrice` (non-Essential controller)

`unitPrice` appears in DETAIL only, absent from LIST; **not supported on Omada Essential**.

```json
{
  "name": "5P 2H Kano -- TESTING", "createdTime": 1779432311310,
  "creatorName": "{REDACTED-email}", "duration": 120, "durationType": 1,
  "unusedCount": 10, "inUseCount": 0, "expiredCount": 0, "totalCount": 10,
  "unitPrice": "5", "unusedAmount": "50", "usedAmount": "0", "totalAmount": "50", "currency": "PHP",
  "description": "commission 10%",
  "statisticsCount": { "totalUnusedCount": 10, "totalInUseCount": 0, "totalExpiredCount": 0, "totalStatisticsCount": 10 },
  "totalRows": 10, "currentPage": 1, "currentSize": 10,
  "data": [ { "id": "{REDACTED}", "code": "{REDACTED}", "status": 0, "startTime": 0,
              "endTime": 9223372036854775807, "timeLeftSec": 7200, "timingByClientUsage": false } ]
}
```
- `createdTime` epoch ms; `duration` minutes; `code` is 6–8 digit numeric string.
- `unusedAmount`/`totalAmount` = count × unitPrice (PHP value), not counts.

### GET list — `?currentPage=1&currentPageSize=100`

```json
{
  "errorCode": 0,
  "result": {
    "currentPage": 1, "currentSize": 10, "totalRows": 3, "groupAmount": 5000,
    "data": [ { "id": "{REDACTED-groupId}", "name": "AUTOGEN: 5P2H Kano", "description": "commission 10%",
                "duration": 120, "totalCount": 130, "unusedCount": 130, "usedCount": 0,
                "createdTime": 1779682851340, "applyToAllPortals": true } ]
  }
}
```
- `id`, `totalCount`, `usedCount`, `unusedCount` top-level per group; NO `unitPrice`/`statisticsCount` in list.
- `totalRows` at result level = number of GROUPS (not vouchers). Default page size 10 → pass `currentPageSize=100`.

### POST create — original confirmed body (`name:"test2"`)

```json
{ "codeLength": 8, "codeForm": [0], "amount": 100, "name": "test2", "type": 0, "logout": true,
  "downLimitUnit": 0, "upLimitUnit": 0, "trafficLimitEnable": false, "trafficLimit": null,
  "trafficLimitUnit": 0, "voucherValidityEnable": false, "upTimeLimitEnable": false,
  "durationType": 1, "duration": 120, "description": "desfcripion here", "maxUsers": 1,
  "trafficLimitFrequency": 1, "portalIds": [], "applyToAllPortals": true, "validityType": 0,
  "startTime": "00:00", "endTime": "23:59", "scheduleTime": 0,
  "weeklyEnableDays": {"1":true,"2":true,"3":true,"4":true,"5":true,"6":true,"7":true},
  "pattern": {"patternType":0,"position":0,"ssidNetworkEnable":false,"durationEnable":false,"limitEnable":false,"logoPictureId":null,"logoSize":62} }
```
Response: `{"errorCode":0,"msg":"Success.","result":{"id":"{REDACTED-groupId}"}}`.
- `amount` = voucher count (NOT `totalCount`). `codeLength:8` + `codeForm:[0]` (digits) required. Omada strict about required fields.
