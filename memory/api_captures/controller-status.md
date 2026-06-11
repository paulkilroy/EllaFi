# `GET /{omadaId}/api/v2/settings/system/status` — controller status

Confirmed path 2026-06-10 (cloud-connector form: `/omadac/{deviceId}/{controllerId}/api/v2/settings/system/status`).
Returns controller info including `timeZone` (IANA) and `systemTime` (UTC epoch ms).

**Firmware uses this for timezone** (`applyControllerTimezone` → `result.timeZone`). The earlier
`/sites/{siteId}/dst` path 404s on the local controller API — use this instead.

## 2026-06-10

```json
{
  "errorCode": 0, "msg": "Success.",
  "result": {
    "name": "Omada Controller_{REDACTED}",
    "macAddress": "{REDACTED-MAC}",
    "timeZone": "Asia/Hong_Kong",
    "systemTime": 1781109165000,
    "upTime": 49235000,
    "controllerVersion": "6.2.10.18",
    "category": "local",
    "model": "OC220 1.0",
    "firmwareVersion": "1.5.18 Build 20260506 Rel.79588",
    "hwcStorage": [ { "name": "Disk", "totalStorage": 5.97, "usedStorage": 1.89, "ocStorage": true } ],
    "deviceCapacity": { "adoptedApNum": 7, "apCapacity": 100, "adoptedOswNum": 2, "oswCapacity": 20,
                        "adoptedOsgNum": 1, "osgCapacity": 10, "adoptedOltNum": 0, "oltCapacity": 2,
                        "shareApAndSwitchCapacity": false, "adoptedApAndSwitchNum": 9, "apAndSwitchCapacity": 120 },
    "sn": "{REDACTED-SN}",
    "ip": "{REDACTED-IP}"
  }
}
```
Note: `systemTime` is UTC epoch ms (timezone-agnostic) — can't derive the local offset from it;
only `timeZone` (IANA name) is meaningful, and the ESP32 needs a name→POSIX table to use it.
