# `GET /{omadaId}/api/v2/hotspot/sites/{siteId}/vouchers/statistics/history`

Daily voucher redemption buckets. Query: `?filters.timeStart={sec}&filters.timeEnd={sec}`.

**Key finding (2026-06-10):** buckets are **END-stamped** — `time` is the NEXT local midnight,
so a bucket holds the *prior* day's redemptions. Attribute via `localDayNum(time/1000 - 1)`.
All `time` values land on local midnight (16:00:00 UTC for UTC+8). `timeInterval:24` = daily.

Captured ~Jun 11 00:32 PHT (current partial day = count 1, stamped Jun 12 00:00 PHT):

```json
{
  "errorCode": 0, "msg": "Success.",
  "result": {
    "summary": { "count": 5916, "amount": "0", "duration": 29510040, "currency": "PHP" },
    "usage": [
      { "count": 0,   "timeInterval": 24, "time": 1778428800000, "amount": "0", "currency": "PHP" },
      { "count": 300, "timeInterval": 24, "time": 1778515200000 },
      { "count": 258, "timeInterval": 24, "time": 1778601600000 },
      { "count": 244, "timeInterval": 24, "time": 1778688000000 },
      { "count": 241, "timeInterval": 24, "time": 1778774400000 },
      { "count": 181, "timeInterval": 24, "time": 1778860800000 },
      { "count": 158, "timeInterval": 24, "time": 1778947200000 },
      { "count": 218, "timeInterval": 24, "time": 1779033600000 },
      { "count": 194, "timeInterval": 24, "time": 1779120000000 },
      { "count": 178, "timeInterval": 24, "time": 1779206400000 },
      { "count": 170, "timeInterval": 24, "time": 1779292800000 },
      { "count": 162, "timeInterval": 24, "time": 1779379200000 },
      { "count": 218, "timeInterval": 24, "time": 1779465600000 },
      { "count": 203, "timeInterval": 24, "time": 1779552000000 },
      { "count": 227, "timeInterval": 24, "time": 1779638400000 },
      { "count": 177, "timeInterval": 24, "time": 1779724800000 },
      { "count": 221, "timeInterval": 24, "time": 1779811200000 },
      { "count": 118, "timeInterval": 24, "time": 1779897600000 },
      { "count": 184, "timeInterval": 24, "time": 1779984000000 },
      { "count": 190, "timeInterval": 24, "time": 1780070400000 },
      { "count": 233, "timeInterval": 24, "time": 1780156800000 },
      { "count": 207, "timeInterval": 24, "time": 1780243200000 },
      { "count": 169, "timeInterval": 24, "time": 1780329600000 },
      { "count": 209, "timeInterval": 24, "time": 1780416000000 },
      { "count": 190, "timeInterval": 24, "time": 1780502400000 },
      { "count": 150, "timeInterval": 24, "time": 1780588800000 },
      { "count": 128, "timeInterval": 24, "time": 1780675200000 },
      { "count": 144, "timeInterval": 24, "time": 1780761600000 },
      { "count": 137, "timeInterval": 24, "time": 1780848000000 },
      { "count": 163, "timeInterval": 24, "time": 1780934400000 },
      { "count": 118, "timeInterval": 24, "time": 1781020800000 },
      { "count": 225, "timeInterval": 24, "time": 1781107200000 },
      { "count": 1,   "timeInterval": 24, "time": 1781193600000 }
    ]
  }
}
```
Decode: `1781107200000` = Jun 11 00:00 PHT → **June 10's** total (225). `1781193600000` =
Jun 12 00:00 PHT → **June 11's** partial total (1).
