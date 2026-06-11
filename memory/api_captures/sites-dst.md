# `GET /{omadaId}/api/v2/sites/{siteId}/dst`

Site timezone / DST settings. Admin session (cookie + csrf-token).
Cloud-connector request path form: `GET /omadac/{deviceId}/{controllerId}/api/v2/sites/{siteId}/dst`

**NOT used by firmware** — this path 404s on the local controller API (capture below was via the
cloud connector). The firmware reads `timeZone` from `settings/system/status` instead — see
[controller-status.md](controller-status.md).

## 2026-06-10

```json
{
  "errorCode": 0, "msg": "Success.",
  "result": {
    "site":   { "enable": false, "mode": 0, "status": false, "startTime": 0, "endTime": 0,
                "offset": 0, "nextStart": 0, "nextEnd": 0, "timeZone": "Asia/Hong_Kong",
                "lastStart": 0, "lastEnd": 0 },
    "omadac": { "enable": false, "mode": 0, "status": false, "startTime": 0, "endTime": 0,
                "offset": 0, "nextStart": 0, "nextEnd": 0, "timeZone": "Asia/Hong_Kong",
                "lastStart": 0, "lastEnd": 0 }
  }
}
```
- `timeZone` = IANA name (used by firmware → POSIX TZ table at startup).
- `offset` = DST adjustment (0 when DST disabled), NOT the base UTC offset.
- `enable`/`status` = DST on/off (both false; HK/PH don't observe DST).
