# `/{omadaId}/api/v2/sites/{siteId}/setting/profiles/groups`

Firewall/ACL **profile groups** (IP / IPv6 / MAC / port / domain / **country‑location**). Permanent,
site‑level config objects — NOT voucher groups, NOT shown to portal customers. Admin session.
Source: `group.har` (self‑capture, 2026‑07‑24), values redacted.

## Group `type` enum (from a live GET)
| type | kind | key list field | has `description`? |
|---|---|---|---|
| 0 | IP group | `ipList[{ip,mask,description}]` | per‑entry only |
| 3 | IPv6 group | `ipv6List[{ip,prefix}]` | — |
| 5 | **Country / location group** | `countryList[]` | **YES — top‑level free‑text** |
| 7 | Domain group | `domainName[]` / `domainNamePort[]` | — |
_(MAC and port group types also exist — not captured here.)_

## ⭐ Type‑5 `description` = usable data store
- **Max length: exactly 512 characters** (binary‑searched: 511 ✓, 512 ✓, 513 ✗). The API's
  rejection text `Parameter [description] should be 1 - 256 characters` is **canned/wrong** — the real
  cap is 512. `errorCode -1001` on overflow.
- **Supports in‑place update (PATCH)** — unlike `voucherGroups` (create/delete only). So the blob can be
  rewritten without churning ids.
- Permanent, invisible to customers, survives an ESP wipe, and readable from any node → good durable,
  cross‑node store. 512 chars/group; **chunk across several named groups for more** (base64 512 ≈ 384 B;
  ~a year of weekly per‑seller data ≈ 3 groups).

## POST — create
`POST /{omadaId}/api/v2/sites/{siteId}/setting/profiles/groups`
```json
{"name":"ELLAFI_DATA_0","type":5,"ipList":null,"ipv6List":null,"macAddressList":null,"portList":null,
 "countryList":["Philippines"],"description":"<up to 512 chars>","portType":null,"portMaskList":null,
 "domainNamePort":null,"ouiList":null,"count":0}
```
Response: `{"errorCode":0,"msg":"Success.","result":"{groupId}"}`  ← result is the new groupId string.

## GET — list all groups
`GET /{omadaId}/api/v2/sites/{siteId}/setting/profiles/groups`
```json
{"errorCode":0,"msg":"Success.","result":{"data":[
  {"groupId":"DomainGroup_Any","name":"DomainGroup_Any","buildIn":true,"type":7,"domainName":["*"],"resource":1},
  {"groupId":"{REDACTED}","name":"IPGroup_Any","ipList":[{"ip":"0.0.0.0","mask":0}],"type":0,"resource":0},
  {"groupId":"BI-IPv6Group_Any","name":"IPv6Group_Any","buildIn":true,"ipv6List":[{"ip":"::","prefix":0}],"type":3,"resource":1},
  {"groupId":"{REDACTED}","name":"ESP32 HotSpot","ipList":[{"ip":"192.168.0.3","mask":32,"description":""}],"type":0,"resource":0},
  {"groupId":"{REDACTED}","name":"ELLAFI_DATA_0","type":5,"countryList":["Philippines"],"description":"...","resource":0}
], "totalOuiCount":0}}
```
Note: identifier is **`groupId`** here (not `id`). `buildIn:true` = system groups (don't touch).

## PATCH — update in place
`PATCH /{omadaId}/api/v2/sites/{siteId}/setting/profiles/groups/{type}/{groupId}`  ← type in the path (e.g. `/5/`)
```json
{"name":"ELLAFI_DATA_0","type":5,"resource":0,"ipList":null,"ipv6List":null,"macAddressList":null,
 "portList":null,"countryList":["Philippines"],"description":"<new value>","portType":null,
 "portMaskList":null,"domainNamePort":null,"ouiList":null,"count":1}
```
Response: `{"errorCode":0,"msg":"Success."}`

## DELETE
`DELETE /{omadaId}/api/v2/sites/{siteId}/setting/profiles/groups/{type}/{groupId}` → `{"errorCode":0,"msg":"Success."}`

## Gotchas
- Type is required in the PATCH/DELETE **path** as well as the body.
- Send the full field set (the `null` siblings) on create/patch — partial bodies are rejected.
- A type‑5 group needs a non‑empty `countryList` (e.g. `["Philippines"]`); `description` alone isn't enough.
- These bind to firewall/ACL rules — a group used only as a data store simply stays unbound (harmless).
