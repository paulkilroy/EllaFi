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
_Behavioral sections (config-apply timeout, provisioning/pending-site, INFORM contract, forget/reboot/
locate, GET/REBUILD/RC4-rekey, notify/portal events, OTA upgrade) are being appended from the deep
code trace._
