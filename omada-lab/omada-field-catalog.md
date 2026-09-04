# Omada device-plane FIELD CATALOG — every field, both directions

Codifies **all fields exchanged** between an EllaFi fake-AP (the `emulator/adopt_full.py`
reference impl, and the future ESP32 port) and the Omada controller. This is the
data-model companion to:
- **`omada-protocol-reference.md`** — transport/framing/crypto/timing/state-machine (the *how*)
- **`adopt-handshake.md`** — the handshake, empirically confirmed
- **`../memory/omada_app_reference/device-field-table.md`** — inform fields per device type (eap/osg/osw)
- **`../memory/omada_app_reference/field-map.md`** — which app screen each field renders on
- **`../memory/omada_device_log_alerts_design.md`** — the `log` field (Alerts/Events injection)

Directions: **D→C** device→controller (all **plaintext** JSON in TLS), **C→D**
controller→device (**RC4** after DEVICE_NEGOTIATION). Framing = 4-byte BE length +
UTF-8 `{header, body}`. All hex digests UPPERCASE; all timestamps **ms**.

Source: `emulator/adopt_full.py` (live-verified 5.15 + 6.2), decompiled jars in `_jadx*/`.

---

## Header (every message, both directions)

`MessageHeader` — order: `seq, version, verCap, device, mac, type, error(=0),
compress, dest, timestamp(Long ms), ip` (+ `seqNum/last` when fragmenting).
Accept-gate: `mac` non-empty · `device` resolves via `DeviceType` · `type` a known
`MessageType` · body matches the type's class — else **channel closes**.
`getSeq()` = `header.seq` ?? body `sequenceId` ?? `additionalProperties.sequenceId`.

---

## Message flow — who sends what (types from `MessageType.java`)

| # | Dir | Type (id) | Body / purpose | Encrypt |
|---|----|---|---|---|
| 1 | D→C | DISCOVERY (1), UDP 29810 | discovery deviceInfo → Pending list | plain |
| 2 | C→D | PRE_ADOPT_REQUEST (2), UDP | `{adoptPort}` (operator tapped Adopt, or REST adopt) | plain |
| 3 | D→C | PRE_CONNECT_INFO (3), TLS 29814 | identity + caps, opens manage channel | plain |
| 4 | C→D | PRE_CONNECT_INFO_RESPONSE (1048576) | `{randomKeyForDeviceVerify, username, destIp}` | plain |
| 5 | D→C | DEVICE_VERIFY_INFO (1048577) | `{auth, randomKeyForSystemVerify}` (device proves) | plain |
| 6 | C→D | DEVICE_VERIFY_RESPONSE (1048578) | `{error:0, auth}` (controller proves back) | plain |
| 7 | D→C | SYSTEM_VERIFY_RESULT (1048579) | *empty* → ctx ADOPT_SUCCESS | plain |
| 8 | C→D | VERIFY_RESULT_ACK (1048585) | *empty* | plain |
| 9 | D→C | DEVICE_NEGOTIATION (1048580) | RC4-key handoff + caps (frag-able) | plain |
| 10 | C→D | SYSTEM_NEGOTIATION (1048581) | initial config + **userAccount** push | **RC4** |
| 11 | D→C | INIT_SYNC_RESULT (1048582) | *empty, error 0* → ctx **CONNECTED** | plain |
| 12 | C→D | INIT_SYNC_RESULT_ACK (1048586) | *empty* | plain |
| 13 | C→D | SET_REQUEST (4096) | config push `{sequenceId, configVersion, config}` | **RC4** |
| 14 | D→C | SET_RESPONSE (8192) | `{sequenceId, errcode:0, configVersion}` + **echo header.seq** | plain |
| 15 | D→C | **INFORM_REQUEST (256)** | the vitals/clients/logs heartbeat (every cycle) | plain |
| 16 | C→D | INFORM_RESPONSE (512) | *empty* fire-and-forget ack | plain |
| 17 | C→D | GET_REQUEST (24576) | read a config section | RC4 |
| 18 | D→C | GET_RESPONSE (28672) | section value + **echo seq** | plain |
| 19 | C→D | UPGRADE_REQUEST_V2 (69632) | firmware upgrade (url+md5) | RC4 |
| 20 | D→C | UPGRADE_RESPONSE_V2 (73728) | ack + **echo seq** (→ EllaFi GitHub OTA) | plain |
| 21 | C→D | REBUILD_REQUEST (36864) | re-push full config on live channel | RC4 |
| 22 | C→D | FORGET_REQUEST (16384) / _NO_RESET (131072) | factory-reset / unmanage | RC4 |

Reconnect = new TLS + **PRE_CONNECT_INFO** (step 3), verify with the *learned* site
Device Account. Empty-body types: INFORM_RESPONSE, FORGET_*, SYSTEM_VERIFY_RESULT,
VERIFY_RESULT_ACK, INIT_SYNC_RESULT(_ACK).

---

## D→C field tables

### PRE_CONNECT_INFO (3) + DEVICE_NEGOTIATION (9) — identity & capabilities
`{needUsername:true, controllerSetting.controllerId, deviceInfo, deviceMisc, radioCap[], devCap}`

| Field | Example / value | ESP source |
|---|---|---|
| deviceInfo.name | `EllaFi-Negra` | config nickname |
| deviceInfo.model | `EAP225-Outdoor` | **FAKE** (old model = fewer config pushes) |
| deviceInfo.modelVersion | `1.0` | FAKE |
| deviceInfo.firmwareVersion | `5.0.0` (low, to trip the OTA prompt) or real build | **SMUGGLE**/FAKE — see OTA note |
| deviceInfo.hardwareVersion | `1.0` | FAKE |
| deviceInfo.upTime | `3600` (seconds string here, in DISCOVERY/PRE_CONNECT) | REAL |
| deviceInfo.cpuUtil / memUtil | `5 / 37` | REAL-ish / REAL |
| deviceInfo.wirelessLinked | `false` | FAKE |
| deviceInfo.**hwId** / **oemId** | genuine EAP225 IDs (plain keys, controller encrypts) | FAKE — enables cloud OTA match |
| deviceMisc.support{2g,5g,mesh,…} | booleans | FAKE (match the spoofed model) |
| radioCap[] | `{radioId,minPow,maxPow,mimo,supportSsidNum,supportMaxClient,…}` | FAKE per model |
| devCap (negotiation) | `{supportRadioMode2G:1, logNotification:{supportLogKey:[]}, …}` | FAKE — **controller NPEs if `devCap`/`logNotification` missing** |
| components_v2 | `{wlanBasic, ssidInform, portal, led, upgrade, abnormalDetect(→health graph), …}` | FAKE version map |
| key (negotiation) | `b64(RSA_pub(RC4sessionKey))` | crypto |
| configVersion | `0` fresh, `N` after applying SET vN | state |

### DEVICE_VERIFY_INFO (5)
| Field | Value |
|---|---|
| auth | `ecsp2_auth(user, MD5_UPPER(pass), randomKeyForDeviceVerify)` = `SHA256_UPPER(SHA256_UPPER(user+md5pw)+rndKey)` |
| randomKeyForSystemVerify | random **≥36 chars** (`uuid4`) — 6.2 rejects shorter with `INVALID_DEVICE_RANDOMKEY` |

### INFORM_REQUEST (15) — the heartbeat; **the field set that matters most**
Top level: `{sequenceId (↑ every inform), timeStamp (ms), deviceInfo, wSettings_2G/5G,
ssidStats_2G/5G[], radioTraffic_2G/5G, clients[], log, mesh}`

**deviceInfo (vitals):**
| Field | EllaFi source | Trap |
|---|---|---|
| name, model, firmwareVersion, hardwareVersion | identity (fw = smuggled build) | — |
| **upTime** | `"<D> days HH:MM:SS"` | **NOT seconds** — `R.k()` StringTokenizer throws → aborts stats persist |
| ip | REAL LAN IP | — |
| cpuUti | REAL-ish idle load | — |
| memUti | REAL `1−freeHeap/total` | — |
| txRate / rxRate | coin-API bytes or 0 | — |
| wirelessLinked | `true` | — |
| temp[] | REAL ESP32-S3 die temp | array |
| nf[] | `[-95,-92]` noise floor | FAKE |

**wSettings_2G / wSettings_5G (per-radio `WirelessInfo` → Device Health spider graph):**
| Field | EllaFi mapping (the smuggle) | Trap |
|---|---|---|
| region, ch, bw, rdMode | FAKE constants | — |
| **txR** | `"0.0"` | **must contain `.`** — `h.a()` does `substring(0,lastIndexOf('.'))` → throws on `"0"` |
| **txPower** | `""` (blank) | **`h.b()` does `substring(0,len-3)`** → blank is safe, `"20"` throws |
| **busyUti** | **SOCKET POOL % = sockets/12×100** → health goes red exactly at OUT-OF-SERVICE | — |
| interUti | TLS contention / coin-reject rate | — |
| txUti / rxUti | coin-API / WS traffic or low | — |
| aiRoamingOffset | 0 | — |

**clients[] (`List<ClientStats>` → Clients count vital; table stays synthetic):**
`{mac, rid, ap, ssid, rssi, snr, ccq, rate, down, up, time, ip, name, type, pm,
txR, rxR, txT, rxP, txP, aTime, bw, guest}` — **`time` = `"<D> days HH:MM:SS"`**
(same tokenizer trap). **Do NOT emit real customer MACs** (double-claim vs the real
EAP → station flap); count only, synthetic/empty rows.

**ssidStats_2G/5G[]:** `{id, ssid, clntNum, down, up, downPkts, upPkts, bssid, rxS, txS}`
**radioTraffic_2G/5G:** `{tx, rx, txP, rxP, txRP, rxRP, txEP, rxEP, badfcs}`

**log (→ Alerts/Events — the remote-alert channel):** `{"logs":[{"k":catalogKey,"c":verbatimText,"t":ms}]}`
— key must be enabled in the site; `c` renders verbatim; `alert:true` key → Alerts tab.
Full design: `../memory/omada_device_log_alerts_design.md`.

**mesh (topology uplink/downlink, optional):** `MeshInfo{status, parents[]{mac,rssi,snr,ch,meshVer,radioId}, childAPs[]{mac,txR,rxR,up,down}}`.

### SET_RESPONSE (14) / GET/UPGRADE/REBUILD responses
`{sequenceId, errcode:0, configVersion}` — and **`header.seq` MUST echo the request's
seq** (RPC correlation; wrong seq → stuck Configuring → 10-min reset).

---

## C→D field tables (what we must PARSE / ACK)

### PRE_CONNECT_INFO_RESPONSE (4): `{randomKeyForDeviceVerify, username, destIp}`
### DEVICE_VERIFY_RESPONSE (6): `{error:0, auth=systemAuth}` (verify controller back)
### SYSTEM_NEGOTIATION (10, RC4) — initial config + the credential push
`{userAccount:{newUsername, newPassword=UPPER_MD5}, timeSetting, dst, …}` — **LEARN +
PERSIST `userAccount`** (device NVS equivalent); present it on every reconnect.
### SET_REQUEST (13, RC4): `{sequenceId, configVersion, config{…}}`
Config sections seen in captures: `ssid, ssidName, radioId, band, channel, txPower,
led, portal, …`. **ESP plan: ACK everything, honor only what maps (LED → real).**
### UPGRADE_REQUEST_V2 (19, RC4): firmware url + md5 → **ignore the .bin, pull EllaFi's
GitHub build, report upgrade success** (the one unbuilt handler).
### FORGET_REQUEST (22): 16384 = factory-reset→discoverable; 131072 = unmanage. Drop the account.

---

## Controller→device ACTIONS (⋮ menu) → real ESP ops
Reboot → `esp_restart()` · Locate → LED blink · Firmware Upgrade → GitHub OTA ·
Force-Provision / config pushes → ACK+echo · Forget → drop account.

---

## ESP32 port checklist (the firmware must PRODUCE)
1. Header on every frame (ms timestamp, echoing seq on RPC responses).
2. Handshake: PRE_CONNECT → DEVICE_VERIFY (ecsp2_auth, ≥36-char randomKey) →
   DEVICE_NEGOTIATION (RSA-wrap RC4 key, devCap w/ logNotification) → INIT_SYNC_RESULT.
3. Persist the pushed `userAccount` to NVS; verify with it on reconnect.
4. RC4 decode for C→D frames; plaintext for D→C.
5. INFORM every cycle with a **changing sequenceId**, ms `timeStamp`, and **decoder-safe
   stat strings** (`upTime`/`time` = `"<D> days HH:MM:SS"`, `txR` decimal, `txPower` blank).
6. SET_RESPONSE echoes request `header.seq`, `errcode:0`.
7. Map vitals REAL (uptime/mem/temp/clients/socket-pool→busyUti), FAKE the radio noise,
   SMUGGLE the build into `firmwareVersion`, inject alerts via `log`.
8. Handle actions: Reboot/Locate/Upgrade/Forget → real ops; other config → ACK.

**Every D→C stat string must match the controller's parser or the whole inform
aborts *silently* (device still gets its ack).** Bring-up canary:
`grep -E 'ERROR|Exception' server.log` while informs flow.

## Gate
All of this rides the adopt+inform loop, still behind the Week-1 crypto go/no-go.
