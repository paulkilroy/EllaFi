# sites/{siteId}/clients — all connected clients (admin/cloud)

`POST /omadac/{controllerId}/openapi/v2/{id2}/sites/{siteId}/clients`
Body: `{"filters":{"active":true},"sorts":{},"hideHealthUnsupported":true,"page":1,"pageSize":50,"scope":1}`

> The admin "all clients" list (distinct from the operator `hotspot/sites/{siteId}/clients`). The
> firmware hits the v2 form `GET .../sites/{siteId}/clients?filters.active=true`; the cloud web UI
> uses this POST form. All identifiers below are **redacted** — types/shapes only.

## Key finding: `authStatus` and `authInfo` (reverse-engineered from a live capture, 2026-06-13)

Each client row carries an explicit auth state — **no need to derive "pending" by diffing against
the hotspot list**:

| `authStatus` | meaning | `authInfo` |
| --- | --- | --- |
| `0` | not a portal client — wired / infra / on the management SSID (no auth required) | `[]` |
| `1` | **PENDING** — connected to the portal SSID, awaiting auth | `[]` (empty) |
| `2` | **AUTHORIZED** | `[{authType, info}]` |

`authInfo[].authType` seen live: **`3` = voucher** (`info` = the voucher code). The firmware's
coin/vendo path uses **`4` = external portal** (`OMADA_AUTH_TYPE_EXTERNAL_PORTAL`). So a voucher
customer (enters a code on Omada's own portal) shows `authType:3`; a vendo/coin client authed by
the firmware would show `authType:4`.

→ **Pending-client detector for the auth-field probe: `authStatus == 1`.**

## Response schema (types only)

```
result:
  totalRows: int
  currentPage / currentSize: int
  data: [
    mac, name, hostName, vendor, deviceType, deviceCategory, osName, model: string
    ip: string;  ipv6List: [string]
    connectType: int;  connectDevType: "ap" | "switch";  wireless: bool
    ssid: string;  apName, apMac: string;  radioId: int;  channel: int          # wireless rows
    switchMac, switchName: string;  port: int                                   # wired rows
    signalLevel, healthScore, signalRank, wifiMode, rssi, snr: int
    rxRate, txRate, trafficDown, trafficUp, downPacket, upPacket, activity, uptime: int
    lastSeen: int (epoch ms)
    vid: int;  guest, blocked, active, manager, powerSave: bool
    authStatus: int            # 0 / 1 / 2  (see table above)
    authInfo: [ { authType: int, info: string } ]   # info = voucher code for authType 3
  ]
  clientStat / clientTypeStat / detailedClientTypeStat: { ... aggregate counts ... }
```

## One redacted example row (wireless, authorized voucher client)

```json
{
  "mac": "{mac}", "name": "{name}", "vendor": "{vendor}", "deviceType": "Mobile",
  "ip": "{ip}", "ipv6List": ["{ipv6}"], "wireless": true, "ssid": "{ssid}",
  "apName": "{apName}", "apMac": "{apMac}", "radioId": 1, "channel": 36,
  "rssi": -70, "authStatus": 2, "guest": true, "active": true,
  "authInfo": [ { "authType": 3, "info": "{voucherCode}" } ]
}
```

---

## PATCH — rename a client / set its alias  (captured 2026-06-14)

The "edit client name" call. **EllaFi uses this to stash a node's nickname+commission on the
controller** (the node is itself a client; its `name` is the alias), so it survives a device wipe and
syncs across nodes. Read it back for free from the all-clients list (`name` field).

`PATCH /omadac/{controllerId}/api/v2/sites/{siteId}/clients/{mac}`  — mac is **dash-separated**, in the path.

**Request body** (only `name` matters; `clientInfoCorrection` is optional UI metadata):
```json
{ "name": "{alias}", "clientInfoCorrection": { "type": "string", "vendor": "string", "model": "string" } }
```

**Response 200** — `{ "errorCode": 0, "msg": "Success.", "result": { …the full client record… } }`,
same schema as the GET list above, with the updated `name`. Also seen on the record: `rateLimit{}`,
`ipSetting{useFixedAddr}`, `clientLockToApSetting{enable}`.

**Firmware wiring:** URL mirrors the other calls —
`CONTROLLER_BASE_URL + "/" + CONTROLLER_ID + "/api/v2/sites/" + SITE_ID + "/clients/" + <mac-dashes>`,
method `PATCH`, `Content-Type: application/json`, body `{"name": "<nickname>|<commission>"}`.
HTTPClient: use `h.sendRequest("PATCH", body)`.
