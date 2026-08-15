# Portal customized-page upload (Import Customized Page)

Captured 2026-08-15 from the controller web UI (HAR, cloud-connector path; verified working
against the local v2 API by `scripts/upload_portal.py`). Auth: normal v2 login → `Csrf-Token`
header + session cookie.

## 1+2 · Upload the zip (two-step, same endpoint)

`POST {cid}/api/v2/files/sites/{siteId}/portal/page` — multipart/form-data

Step 1 (pre-check), field `data` only:
```json
{"md5":"<md5-of-zip>","fileName":"ellafi-poc.zip"}
```
→ `{"errorCode":0}`

Step 2, fields `data` (same JSON) + `file` (the zip bytes):
→ `{"errorCode":0,"result":{"id":"<fileId>","fileName":"ellafi-poc.zip"}}`

## 3 · Bind to the portal

`PATCH {cid}/api/v2/sites/{siteId}/setting/portals/{portalId}` — the UI sends a TRIMMED portal
object, not the full GET result (the full object fails validation: its half-populated
`portalCustomize` demands `defaultLanguage`/`copyrightEnable`). Field set that works:

```json
{"id","name","enable","httpsRedirectEnable","landingPage","landingUrlScheme","portalUrlAuto",
 "portalUrl","authType","hotspot","resource","modifyTimeMs","ssidList","networkList",
 "importedPortalPage":{"id":"<fileId>"},"pageType":2}
```

`pageType: 2` = imported custom page. Values sourced from `GET …/setting/portals` (lists all
portals with names — find by `name`).

## Quirks

- Sites listing for this flow: `GET {cid}/api/v2/user/sites?currentPage=1&currentPageSize=100`
  (plain `/api/v2/sites` with `currentSize` returns empty — the usual per-endpoint param naming).
- Served portal URL shape: `http://<controller>:8088/portal/entry/{cid}/{siteId}/{portalId}/index.html`
  with redirect params appended (`clientMac`, `apMac`, `ssidName`, `radioId`, `clientIp`,
  `originUrl`; no `gatewayMac`/`vid`/`t` seen on this controller).
- Portal config of record (EllaFi Test Portal): `authType 11` (hotspot), `hotspot.enabledTypes [3]`
  (voucher only), `httpsRedirectEnable false` — keep false: the page calls the ESP over plain http.

Tool: `scripts/upload_portal.py --portal "<name>"` (uses `data/config.json` active env).
