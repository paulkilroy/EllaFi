# Omada ECSP device-plane protocol — master reference (SW 5.15.24.19)

Reverse-engineered from the decompiled controller (`dev/_jadx/`). This is the wire-format + message
backbone. Behavioral per-protocol detail (config-apply, provisioning, notify, upgrade, reconnect) is
being filled in from a deep code trace — see the sections below and `adopt-handshake.md`.

## Transport & framing
- **Channels/ports** (controller startup log): discovery **29810/UDP**; manage **v1 29811**; adopt v1
  **29812**; upgrade **29813**; manage/adopt **v2 29814 (TLS)**; transfer **v2 29815**.
- **Discovery (29810/UDP):** plaintext. `[4-byte BE length][UTF-8 JSON {header, body}]`. Length = payload.
- **v2 manage/adopt (29814):** **TLS** (server cert, no client-auth). Inside TLS: `[4-byte BE length]
  [JSON]`, plaintext until the RC4 upgrade. After `DEVICE_NEGOTIATION` hands over a session key, the
  controller `reinitRc4Handler`s the pipeline: **outbound (controller→device) frames become RC4**
  `RC4(4-byte total-len) + RC4(payload)`; **inbound (device→controller) stays plaintext** in practice
  (send acks/informs plaintext, decode pushes as RC4). RC4 = standard, key = the device-chosen session
  key (RSA-wrapped to the controller in DEVICE_NEGOTIATION `body.key`).

## Message header (MessageHeader)
Order: `seq, version, verCap, device, mac, type, error, compress, dest, timestamp, ip, seqNum`.
- `seq` Int — sequence number (per device, increments).
- `version` String — ECSP protocol version ("2.3.0" = V2). NOT firmware.
- `verCap` Int — version capability (optional).
- `device` String — DeviceType enum: `ap` / `gateway` / `switch` / `olt` (validated by DeviceType.resolve).
- `mac` String — device MAC (format varies: dash `8C-86-DD-..` in discovery; `formatMacCL`/`formatMacDb`).
- `type` Int — MessageType (table below).
- `error` Int — 0 = ok; nonzero = error code.
- `compress` String — e.g. "lzo-2.07" (controller advertises `acceptCompress`; optional).
- `dest` String — destination omadacId (on controller→device / some device→controller).
- `timestamp` Long. `ip` String — claimed device IP. `seqNum` String.

## Message bodies
Most bodies extend `AbstractMessageBody` (→ `AbstractData`) and carry **dynamic JSON**
(`additionalProperties` map) — i.e. the config/get/notify/upgrade payloads are flexible key→value maps,
not fixed schemas. Fixed-field bodies of note below.

## Complete MessageType table  (value · name · body class · direction)
| Type | Name | Body | Dir |
|---|---|---|---|
| 1 | discovery | BaseDiscovery | D→C |
| 2 | pre.adopt.request | PreAdoptRequest{mac, ip, adoptPort} | C→D (UDP) |
| 3 | pre.connect.info | **PreConnectInfo{needUsername, rebuild, token}** | D→C |
| 16 | adopt.request | AdoptRequest{data, hash, sig} | C→D |
| 32 | adopt.response | BaseAdoptResponse{key, deviceInfo{encryptedHwId,encryptedOemId}} | D→C |
| 256 | inform.request | BaseInformRequest (Eap/Osg/Osw…) | D→C |
| 512 | inform.response | NullMessage | C→D |
| 4096 | set.request | BaseConfigRequest (dynamic: lanSetting, ssid_*, wireless*, …) | C→D |
| 8192 | set.response | BaseConfigResponse{sequenceId, configVersion} | D→C |
| 4352 | init.sync | BaseInitialSetSync | C→D (v1) |
| 16384 | forget.request | ForgetRequest{interval,…} (extends BaseConfigRequest) | C→D |
| 20480 | forget.response | NullMessage | D→C |
| 131072 | forget.request.no.reset | ForgetRequest | C→D |
| 196608 | forget.response.no.reset | NullMessage | D→C |
| 24576 | get.request | BaseGetRequest (dynamic) | C→D |
| 28672 | get.response | BaseGetResponse{sequenceId, errCode, …} | D→C |
| 80 | notify.request | NotifyRequest (dynamic) | D→C |
| 144 | notify.response | NotifyResponse | C→D |
| 32768 | upgrade.request | BaseUpgradeRequest | C→D |
| 65536 | upgrade.response | BaseUpgradeResponse | D→C |
| 77824 | upgrade.notify.request | BaseUpgradeRequest | C→D |
| 81920 | upgrade.notify.response | BaseUpgradeResponse | D→C |
| 36864 | rebuild.request | BaseRebuildRequest | D→C |
| 40960 | rebuild.response | BaseRebuildResponse{errCode, resetConfig} | C→D |
| 86016 | rebuild.result | BaseConfigResponse{sequenceId, configVersion} | D→C |
| 64 | portal.query | PortalQuery | D→C |
| 128 | portal.auth | Authorization | D→C |
| 352 | portal.auth.response | NotifyResponse | C→D |
| 1048577 | device.verify | DeviceVerifyInfo{auth, randomKeyForSystemVerify} | D→C |
| 1048578 | device.verify.response | DeviceVerifyResponse{auth} | C→D |
| 1048579 | system.verify.result | NullMessage | D→C |
| 1048585 | verify.result.ack | NullMessage | C→D |
| 1048580 | device.negotiation | BaseAdoptResponse{key, devCap, deviceInfo, configVersion,…} | D→C |
| 1048581 | system.negotiation | BaseInitialSetSync{userAccount, timeSetting, components, informInterval,…} | C→D |
| 1048582 | init.sync.result | NullMessage | D→C |
| 1048586 | init.sync.result.ack | NullMessage | C→D |
| 1376256 | report | Report{reportData} | D→C |

## Confirmed sequences
**Adoption (v2):** discovery → (tap) PRE_ADOPT_REQUEST(UDP) → TLS 29814 → PRE_CONNECT_INFO{needUsername}
→ PRE_CONNECT_INFO_RESPONSE{rkdv, username} → DEVICE_VERIFY_INFO → DEVICE_VERIFY_RESPONSE(0) →
SYSTEM_VERIFY_RESULT → VERIFY_RESULT_ACK → DEVICE_NEGOTIATION(RC4 key) → SYSTEM_NEGOTIATION(config+account,
RC4) → INIT_SYNC_RESULT → CONNECTED → SET_REQUEST(config) → SET_RESPONSE{seq,configVersion} → INFORM loop.

**Key open clue — clean reconnect:** `PreConnectInfo` carries `rebuild` (Int) + `token` (String). A
re-linking (already-adopted) device most likely sets `rebuild=1` + prior session `token` in
PRE_CONNECT_INFO to reconnect WITHOUT a fresh factory re-adopt — this is the fix for the reconnect flap
(our emulator currently re-runs full adoption). Being confirmed by the GET/REBUILD trace.

## Config surface (what the controller pushes)
**SYSTEM_NEGOTIATION (1048581)** = initial sync (RC4): `configVersion, sequenceId, controllerSetting
{controllerId,destOmadacId,acceptCompress,managePort,portalHttpPort,portalHttpsPort,logoutDomain,
preferHttpsPortal}, timeSetting{timeZone,date,time,ntpEnable}, dst, userAccount{curUsername,curPassword=
MD5(factory),newUsername,newPassword=MD5(real)}, components{…}, components_v2{…}, informInterval{per-
component seconds}`.

**SET_REQUEST (4096)** = the site config, one message carrying all feature sections (each maps to a row
on the app's "Configuration Result" page). Top-level `sequenceId` + `configVersion`, plus features:
`lanSetting{connType,useFallBack,fallBackIp,fallBackMask}` (IPv4) · `wirelessBasic_2G/5G{radioId,
radioEnable,chanWidth,channel,txPower,freq}` (Radios) · `wirelessAdv_2G/5G{beaconInterval,dtimPeriod,
rtsThreshold,fragThreshold,airtimeFairness,beaconIntvMode}` (Beacon Control) · `ssid_2G/5G{radioId,
ssid:[{id,index,operation,ssidName,securityMode,pskVer,pskCipher,pskKey,…}]}` (WLAN) · `qosConfig_2G/5G`
(QoS) · `snmp` · `ssh` · `led{enable,locate}` · `lldp` · `mesh` · `managementVlan` · `loopback` ·
`dot1x` · `ipGroup{ipGroups}` · `ipv6Group` · `macFilterGlobal` · `portalFreePolicyConfig{portalFree
Policy,urlPortalFreePolicy}` (Portal) · `schedulerAssoc{rule}` · `schedulerGlobal` · `logSetting`
(Remote Logging) · `dhcpSrvChanged`. Device confirms with `SET_RESPONSE{sequenceId, configVersion}`.
(The controller's per-feature "Device response timed out" tracking is under trace — likely each feature
is marked applied/failed off the single SET_RESPONSE and/or the device's subsequent config report.)

## Device status codes (DeviceStatusEnum)
category CONNECTED: 10 Provisioning · 11 Configuring · 12 Upgrading · 13 Rebooting · **14 Connected** ·
15 Connected(Wireless). category PENDING: **20 Pending** · 22 Adopting · 24 Adopt Failed · 26 Managed By
Others. category HEARTBEAT_MISSED: 30–33. category ISOLATED: 40–41. category DISCONNECTED: 0–1.

---
# Behavioral protocols (deep code trace, 2026-08-22)

## A. Config-apply (SET_REQUEST / SET_RESPONSE) — how a device confirms config
- Config is **batched**: many features → **one SET_REQUEST with one `sequenceId`**. A full apply is
  several such SETs (each its own body + sequenceId + configVersion). A "feature" (row on the app's
  Configuration-Result page) = a `SetKey` = a JSON property on the config body. (ES switches fragment
  one body into multiple SETs sharing a sequenceId; APs/gateways don't.)
- The controller **correlates SET_RESPONSE to SET_REQUEST by `sequenceId`** (via
  `EcspV2DeviceServerProxy.sendSetRequest(mac, ver, body, sequenceId, timeoutMs)`). No response in time
  → `TransError` **2600 = "Device response timed out"** (the exact per-feature failure the app shows).
  Timeouts: **60 s AP**, 300 s gateway.
- **A successful SET_RESPONSE (type 8192, body `BaseConfigResponse`) MUST:** echo `sequenceId` exactly ·
  `errcode` = 0 (`@JsonProperty("errcode")`; `getErrcode()==0` required, `g/a.java:408`) · `configVersion`
  = the request's configVersion · and the ECSP **header `error` = 0**. A wrong/missing `sequenceId`
  yields 2600; a wrong `configVersion` instead raises a config-miss/desync event (`DEV_C_DSYNC`,
  `configMissMatch`) but the feature may still clear.
- **CONFIGURING(11) → CONNECTED(14):** on a version-bearing SET the device image goes CONFIGURING with
  `configuringTimeoutTimestamp = now+timeout`, `configuringSequenceId`, and a `configuringSequenceIdSet`.
  It flips to CONNECTED only when **every outstanding sequenceId is acked** (set emptied) —
  `message/set/i.java`. A **plain INFORM does NOT** move a CONFIGURING device to CONNECTED. On timeout
  the next inform reverts it to CONNECTED logging "…by long time inform, maybe something wrong".
- **PROVISIONING(10):** SETs are cached/skipped; if `statusCategory` is not CONNECTED/HEARTBEAT_MISSED
  the send is skipped entirely (`e.java:902` — the "statusCategory is not CONNECTED/HEARTBEAT_MISSED,
  skip" log). So the device must already be CONNECTED for SETs to dispatch.
- ⇒ **Emulator rule:** for every SET_REQUEST, within 60 s send SET_RESPONSE{sequenceId echoed, errcode:0,
  configVersion:request's} + header error 0. Answer each distinct sequenceId. (Now implemented.)

## B. Provisioning state machine — when config is (re)pushed
- Config push is triggered by **(1) a site/config change that bumps configVersion** (primary), **(2)
  adoption** (inits configVersion from the device's reported value — `message/set/c.java:24`), **(3) a
  device-initiated rebuild** carrying its configVersion (`rebuild/d.java:140` → controller replies full
  config). It is **NOT every reconnect** — a plain heartbeat inform assembles no SET.
- A controller-detected configVersion mismatch only sets `configMissMatch` + emits a desync log; the
  actual re-push comes from the rebuild flow or the next config change.
- ⇒ Our emulator flaps because it **re-runs full adoption on every reconnect** (fresh SYSTEM_NEGOTIATION
  + SET_REQUEST = a re-provision), instead of the lightweight re-link (§D). Fixing the reconnect removes
  the churn (and the SET-timeout window during drops).

## C. INFORM contract — what keeps a device CONNECTED
- **Heartbeat is transport-driven and content-agnostic**: the connected route/`lastSeen` is refreshed
  per INFORM in `ecsp .../context/c.java` (requires status CONNECTED + channel==A()); a background
  sweeper fires CONNECTED_TIMEOUT if `lastSeen` goes stale (→ HEARTBEAT_MISSED / drop). Fast heartbeat
  keeps it alive (already implemented, ~3 s).
- **Only `deviceInfo` is load-bearing**, and only the **first inform** hard-requires it (config-sync +
  status→Connected abort if null — `inform/i.java:143`). Everything else (`ssidStats`, `wSettings`,
  `radioTraffic`, `clients`, `lanTraffic`, `radioAccess`, …) is **optional stats** — omit freely; only
  populates UI panels. `widsInform`/`wipsInform`/`callLogInform` are non-negligible only if you include
  the key at all.
- **No applied-configVersion field inside INFORM.** Config-applied is reported out-of-band via the
  SET_RESPONSE path, OR on the first inform via `lastCfgResult`/`cfgResults` (=`ApConfigRespBody`
  {sequenceId, errcode, configVersion}) **only if the controller is awaiting a result**.
- Use a **changing header `seq`** each inform (the manager dedups repeats: equal seq → "ignore repeat
  inform"). `needReply=1` asks for a reply; leave unset. `upTime` is display-only for APs (no reboot/
  reprovision logic keys on it) — send increasing for clean UI, not load-bearing.

## D. Reconnect / REBUILD / RC4 re-key — the clean resume path
- **Adopted-device reconnect does NOT re-provision.** Path: `discovery → PRE_CONNECT_INFO re-link →
  cached device-verify/negotiation (fast branch for CONNECTED/ADOPT_SUCCESS) → ADOPT_SUCCESS →
  REBUILD_REQUEST(36864, device→controller) → CONNECTED`, reusing the cached adopt info + server route.
  `updateV2RebuildingContext` requires status ADOPT_SUCCESS + same channel, then promotes to CONNECTED
  (`EcspV2DeviceContextHelper.java:256`). Only a full re-adopt occurs if the server route/context/key
  expired.
- `PreConnectInfo` carries `rebuild` (Int) + `token` (String) — a reconnecting device signals this; the
  manager parses the token (`deviceInfo/c.java:99`). (Exact rebuild-branch semantics still being traced.)
- **RC4 re-key**: `reinitRc4Handler` has exactly one caller (`server/c/f.java`, EcspVersion **V1**) — so
  the pipeline-swap re-key is **V1-only**. On V1, the controller flips BOTH directions to RC4 (a)
  immediately BEFORE sending the first SET_REQUEST on the ADOPT port, and (b) immediately AFTER sending
  a REBUILD_RESPONSE on reconnect; key = per-device `serverRouteCache.getKey()`. The `resetConfig` field
  inside REBUILD_RESPONSE is itself RC4'd with that key even while the frame is plaintext.
  **For our V2 device** confidentiality is established by the device-negotiation handshake instead
  (controller→device SYSTEM_NEGOTIATION is RC4 with our session key; device→controller stays plaintext —
  matches what the emulator does).
- REBUILD_RESULT (86016, device→controller) body = `BaseConfigResponse{sequenceId, errcode, configVersion}`.
- ⇒ **Emulator TODO:** on reconnect, don't full-readopt — do the re-link + send **REBUILD_REQUEST** to
  reach CONNECTED without a re-provision. (Blocked understanding: the "pending site" close on re-link —
  final trace pending.)

## E. Operational commands (controller→device) — FORGET has a type; the rest are SET
**Only FORGET is a dedicated message type. REBOOT, LOCATE, clear-settings, RF-scan, speed-test, etc.
are all `SET_REQUEST` config sections answered with `SET_RESPONSE`.**
- **FORGET**: `isResetConfig=true` → **FORGET_REQUEST(16384)** (device factory-resets) → **FORGET_
  RESPONSE(20480)**; `false` → **FORGET_REQUEST_NO_RESET(131072)** (un-adopt, keep config) → **FORGET_
  RESPONSE_NO_RESET(196608)**. Bodies are **empty** on the wire (`ForgetRequest.interval` only set on a
  reset-with-delay). Response = empty, **`seq=null`, MAC-correlated** (NOT seq-correlated), header
  `error=0`. Timeout 25 s. On send the controller tears down the device context (`c/a.java:61`). The
  earlier log `failed to send v2 request FORGET_REQUEST seqId null` — `seqId null` is by design.
- **REBOOT**: `SET_REQUEST{ "system": {action:0} }` (`BaseSystemConfig{action,param}`), blocking SET;
  success read from SET_RESPONSE → status→Rebooting + DeviceEvent.REBOOT. No reboot message type.
- **LOCATE (LED)**: `SET_REQUEST{ "led": {locate:true|false} }` (AP `EapLedConfig{enable:String,
  locate:Boolean}`; switch `OswLedConfig` locate 1/0). Async/best-effort; controller auto-expires the
  locate after **600 s** client-side (no duration on the wire). No locate message type.
- **Other commands via SET keys**: `clearSettings`(Int), `remoteReset`(ApRemoteResetConfig),
  `rfScan`(RFScanConfig), `speedTest`(ApSpeedTestConfig), `cableTest`(OswCableTestConfig),
  `wifiControlLed`, `rssiLeds`. Long-running results (rfScan/speedTest/cableTest) come back **later as
  `NOTIFY_REQUEST_V2(1048583)`** device→controller (`notify/req/*NotifyContent`), not in the SET_RESPONSE.
- **Response-required gate** (`server/c/a.java:85`): the controller waits for a device reply ONLY for
  `GET_REQUEST, SET_REQUEST, FORGET_REQUEST(_NO_RESET), UPGRADE_REQUEST_V2, REBUILD_RESPONSE`, and
  `EVENT_PORTAL_AUTH` (seq). Everything else (INFORM, NOTIFY, discovery…) is fire-and-forget. **No
  packet-level retransmit** — a missing reply just times out → the operation is marked failed.
  ⇒ Emulator must reply to SET/GET/FORGET (and the portal-auth push) to avoid failed-command results.

## F. Events / notify / captive-portal  (pisowifi-relevant)
- **Client/roaming/rogue/WIDS events ride inside INFORM** (`ApInformKeyEnum` keys: `clients`, `connRec`,
  `portalAuthClients`, `roaming`/`roamRec`/`bSteerRec`, `rogueApList`, `widsInform`/`wipsInform`), not a
  separate message. Join/leave is diffed from successive `clients` lists.
- **NOTIFY_REQUEST(80)/_V2(1048583)** body `NotifyRequestBody{nid, sub, nre, ctnt}`; `sub` =
  NotifySubjectEnum (1 MAC_AUTH, 2 SET_NOTIFY, 3 GENERAL, 5 TROUBLESHOOTING, 6 FILE_TRANSFER, 8
  CLIENT_OPERATION, 10 DEVICE_DATA_REQUEST…). Controller replies **NOTIFY_REPLY(144)** `{nid, sub, err,
  rst}` unless `nre==1`. Not required for health.
- **Captive-portal auth (coin-op path):** device → **EVENT_PORTAL_QUERY(64)** `{queryInfos:[{mac, rid,
  ssid, vid}]}` ("are these clients authorized?") → controller → **EVENT_PORTAL_AUTH(128)** `{authedUsers:
  [{mac, rst(0=ok), start, end, up, down, trafficLimit/upload/downloadLimit}]}` (grant with validity
  window + rate/data caps) → device → **EVENT_PORTAL_AUTH_RESPONSE(352)** seq-correlated ack. For an AP,
  deauth/reauth may instead arrive as a `CLIENT_OPERATION` SET (`ClientControl`). The device also mirrors
  the current authorized set in every INFORM `portalAuthClients`.
- **REPORT(1376256)** body `{reportData:String}` — opaque bulk telemetry; optional, no reply.

## G. Firmware upgrade (OTA)
- **V1 (EAP path):** controller sends **UPGRADE_REQUEST(32768)** body `{port:29813, length:N}` — no URL/
  MD5. Device connects **port 29813**, controller streams the **whole firmware image as one raw byte
  payload** (no app-layer chunking; 64 MB cap). Device verifies MD5 itself and replies **UPGRADE_
  RESPONSE(65536)** body `{errcode}` (0=ok, 50002=same version, else fail; null=send-fail). No progress %
  on the wire — controller derives progress from its own stage machine.
- **V2 (newer/cloud):** controller sends a **download URL** (`downloadLink` + staged timeouts); device
  pulls firmware over HTTP itself (logic in an out-of-tree `firmware.upgrade` module).
- **FILE_TRANSFER_V2 (port 29815)** is separate and **device→controller** (support/diag files):
  FILE_TRANSFER_REQUEST_V2(1441792) per 512 KB chunk `{fileName,filePath,startIndex,endIndex,partition}`
  → FILE_TRANSFER_RESPONSE_V2(1507328) `{errCode,data(base64),partition,…}`, controller reassembles +
  MD5-checks. NOT firmware download.
- UPGRADE_NOTIFY_REQUEST(77824) = empty relay/notify, not the transfer. An adopted device only needs to
  participate in upgrade once one is initiated; health is independent (INFORM heartbeat).

_Remaining trace: operational commands (forget/reboot/locate full bodies) and the "Invalid v2 adopt
process with pending site" reset condition — agents still running; will append._
