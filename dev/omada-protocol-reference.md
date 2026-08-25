# Omada ECSP device-plane protocol — authoritative reference

Reverse-engineered from the decompiled controller (`dev/_jadx/`: ecsp-common/ecsp-server-core/ecsp-transporter-netty/manager-core/manager-message, all 1.3.7 / 5.15.24.19) and verified live against the Docker lab controller. This is the spec `dev/emulator/adopt_full.py` implements. Ports/framing/crypto/dispatch/state-machine/timing are all here; every non-obvious claim traces to a `class:line` in the jars.

---

## 0. EMULATOR CONTRACT (the rules the device code MUST satisfy)

1. **Timestamps are milliseconds.** `MessageHeader.timestamp` is a `Long` ms. The manager sets a connected device's `lastSeen` directly from each inform's `header.timestamp` (`inform/a.java` `d.a(informDTO.getTimeStamp().longValue())`). Sending seconds → `lastSeen` reads as ~Jan 1970 → the staleness poll marks **Heartbeat-Missed** every cycle while each inform flips it back = the flap. **Use `int(time.time()*1000)` everywhere.**
2. **One persistent socket per session.** The socket promoted to CONNECTED becomes the context's manage channel `A()`. `context/c.java:334` refreshes/keepalives the route ONLY if the message arrives on `A()` and status==CONNECTED; otherwise `V2_MANAGE_MESSAGE_HANDLER` (`a/b/d.java:23-31`) **closes the channel**. Send every INFORM on the same socket that finished the handshake.
3. **Finish the handshake to CONNECTED**, not ADOPT_SUCCESS — send `INIT_SYNC_RESULT` on the manage socket so the context promotes to CONNECTED (`updateInitSyncSuccessContext`). Until then every inform trips the status guard.
4. **INFORM body**: carry a ms `timestamp` and a `sequenceId` that **changes when content changes** (equal seq = idle repeat). After a SET, echo the config `sequenceId` so the manager confirms the apply.
5. **SET_RESPONSE echoes the request's `header.seq`.** The config RPC correlates by `EcspMessage.getSeq()` = `header.seq` (else body `sequenceId`). Wrong seq → RPC never completes → stuck Configuring → ~10-min timeout → channel reset. Include `errcode:0` and header `error:0`.
6. **RC4 is asymmetric on v2**: after `DEVICE_NEGOTIATION`, controller→device frames are RC4 (device must decode: `RC4(len4)++RC4(body)`, keystream reset per field, key = the device's own session key). Device→controller stays **plaintext** length-prefixed JSON inside TLS. (The symmetric `reinitRc4Handler` is v1-only; v2 swaps only the controller's outbound encoder.)
7. **Stop discovery beaconing once adopted** (a real AP does). Resume only if the link genuinely drops.
8. **Reconnect = fresh `PRE_CONNECT_INFO`** on a new TLS socket, then verify with the **learned site Device Account**. Only works after a clean adoption persisted the device into the managed model. Back off (~adopt/context timeout); never storm — a silent close is a manager null-branch a retry cannot fix.

---

## 1. Transport, ports, framing, TLS

Ports (PortConfig.java): **29810** discovery UDP · 29811 manage-v1 TCP · 29812 adopt-v1 TCP · 29813 upgrade-v1 · **29814** adopt+manage-v2 TCP **TLS** · 29815 transfer-v2 TLS. v1/v2 split by URL param `ecsp-version`.

Framing: **4-byte big-endian length prefix + UTF-8 JSON `{header, body}`**. Max frame 1048576 plaintext / 10485760 RC4-inbound / 2000 UDP-discovery (UDP length must equal readable bytes). TLS on 29814 = server cert only, `needClientAuth=false` (device presents no cert; client side does no hostname/pin verification → emulator may use CERT_NONE).

`MessageHeader` fields (order): seq, version, verCap, device, mac, type, error(=0 default), compress, dest, **timestamp(Long ms)**, ip, +seqNum/last (fragmentation). Required to be accepted: `mac` non-empty, `device` resolves via `DeviceType.resolve`, `type` a known `MessageType`, body matches the type's body class (empty-body types exempt) — else the channel is **closed** (`EcspMessage.isNotValid` + `n.received`).

`EcspMessage.getSeq()` = `header.seq` if non-null, else body `sequenceId` (`BaseConfigResponse`/`BaseGetResponse`), else `additionalProperties[sequenceId]`.

## 2. Crypto
All hex digests **UPPERCASE** (`CipherUtils.bytesToHexString`). `ecsp2_auth(user, md5pw, rndKey) = SHA256_UPPER(SHA256_UPPER(user+md5pw)+rndKey)`, `md5pw = UPPER MD5(plaintext)`; controller compares `equalsIgnoreCase`. `DeviceVerifyInfo = {auth, randomKeyForSystemVerify}` (mutual proof: device proves over `randomKeyForDeviceVerify`, controller proves back over `randomKeyForSystemVerify`). RC4 standard KSA/PRGA, per-field keystream reset. RSA `SHA1withRSA` sign; `encrypt/decryptByPrivateKey`; netty keypair from `GlobalConfig.NETTY_RSA_PRIVATE_KEY`. Session RC4 key: device RSA-wraps with controller pubkey → `DEVICE_NEGOTIATION.body.key`.

## 3. Message types & dispatch (ecsp-common MessageType.java is canonical)
DISCOVERY=1 PRE_ADOPT_REQUEST=2 PRE_CONNECT_INFO=3 ADOPT_REQUEST=16 EVENT_PORTAL_QUERY=64 NOTIFY_REQUEST=80 EVENT_PORTAL_AUTH=128 INFORM_REQUEST=256 INFORM_RESPONSE=512 SET_REQUEST=4096 INIT_SYNC=4352 SET_RESPONSE=8192 FORGET_REQUEST=16384 GET_REQUEST=24576 GET_RESPONSE=28672 UPGRADE_REQUEST=32768 REBUILD_REQUEST=36864 REBUILD_RESPONSE=40960 UPGRADE_RESPONSE=65536 UPGRADE_REQUEST_V2=69632 UPGRADE_RESPONSE_V2=73728 REBUILD_RESULT=86016 FORGET_REQUEST_NO_RESET=131072 **PRE_CONNECT_INFO_RESPONSE=1048576** DEVICE_VERIFY_INFO=1048577 DEVICE_VERIFY_RESPONSE=1048578 SYSTEM_VERIFY_RESULT=1048579 DEVICE_NEGOTIATION=1048580 SYSTEM_NEGOTIATION=1048581 INIT_SYNC_RESULT=1048582 VERIFY_RESULT_ACK=1048585 INIT_SYNC_RESULT_ACK=1048586. Empty-body: INFORM_RESPONSE, FORGET_*, SYSTEM_VERIFY_RESULT, VERIFY_RESULT_ACK, INIT_SYNC_RESULT(_ACK).

Dispatcher `server/c/n.java`: verify/negotiation inbound (cases 1-5) → dedicated handlers (state-driven, **no seq**); INFORM (case 6) → manage handler + relay; SET/GET/UPGRADE_V2/PORTAL_AUTH_RESPONSE/REBUILD_RESULT (cases 12-16) → **RPC completion** gated by `getSeq()!=null && channel==A() && status==CONNECTED`; FORGET responses (17-18) mac-only; **default → channel.close()** (controller→device-only types rejected inbound).

**Seq correlation** (`server/c/a.java` + waiter cache keyed by mac+response-type+seq): controller waits only for GET/SET/FORGET/UPGRADE_V2/REBUILD_RESPONSE/PORTAL_AUTH. **Device MUST echo request `header.seq`** on SET/GET/REBUILD_RESULT/UPGRADE_V2/PORTAL_AUTH responses. FORGET = mac-only. Verify/negotiation = never seq-correlated.

## 4. Adoption state machine (v2/29814)
States: DISCOVERY(1) … ADOPTING_PRE_CONNECT(4) ADOPTING_DEVICE_VERIFY(5) ADOPTING_SYSTEM_VERIFY(6) ADOPT_SUCCESS(7) CONNECTED(8) … CONNECTED_TIMEOUT(12) CONNECTED_ERROR(13).

Sequence (device-initiated on TLS, controller replies same socket):
1. DISCOVERY (UDP 29810) → Pending. Operator taps Adopt → controller sends **PRE_ADOPT_REQUEST** (UDP) `{adoptPort}`.
2. Device opens TLS 29814 → **PRE_CONNECT_INFO** `{needUsername:true, controllerSetting.controllerId, deviceInfo, deviceMisc, header.dest=omadacId}`.
3. Controller → **PRE_CONNECT_INFO_RESPONSE** `{randomKeyForDeviceVerify, username, destIp}` (echoes device seq).
4. Device → **DEVICE_VERIFY_INFO** `{auth=ecsp2_auth(user,MD5(pass),rkDeviceVerify), randomKeyForSystemVerify}`.
5. Controller → **DEVICE_VERIFY_RESPONSE** `{error:0, auth=systemAuth}`.
6. Device → **SYSTEM_VERIFY_RESULT** (empty) → ctx ADOPT_SUCCESS. Controller → **VERIFY_RESULT_ACK**.
7. Device → **DEVICE_NEGOTIATION** `{key=b64(RSA_pub(RC4key)), deviceInfo:{devCap,radioCap,components,encryptedHwId,encryptedOemId}}` (fragmentable).
8. Controller → **SYSTEM_NEGOTIATION** (RC4) = initial config incl **userAccount{newUsername,newPassword=UPPER MD5}**, timeSetting, dst.
9. Device → **INIT_SYNC_RESULT** (empty, error 0) → ctx **CONNECTED**. Controller → **INIT_SYNC_RESULT_ACK**.
10. Controller → **SET_REQUEST** (RC4) `{sequenceId, configVersion, config}` → Device → **SET_RESPONSE** `{sequenceId, errcode:0, configVersion}` with **header.seq = request seq** → Configuring→Connected.

Credentials: (a) factory admin/admin — first adopt; (b) adopt-dialog/pending — candidate only; (c) **site Device Account** pushed via `userAccount` — device stores + verifies with it on reconnect. Controller iterates the whole candidate list.

## 5. Connected keepalive & the flap (see §0.1)
`context/c.java:334` refreshes the Redis route (TTL `connectedServerRouteExpire`=86400s/24h) every `updateServerRoutePeriod`=6h — but only if the message is on `A()` and status==CONNECTED, else the channel is closed. period(6h)<expire(24h) ⟹ route config is stable; the flap was purely the seconds-vs-ms `lastSeen` bug. INFORM_RESPONSE (512) is a fire-and-forget empty ack. Manager Heartbeat-Missed uses `lastSeen` (inform ms timestamp) + `refreshTimeout`, not the route TTL.

## 6. Reconnect, context cleanup, forget
Reconnect = new TLS socket + **PRE_CONNECT_INFO** (never REBUILD_REQUEST — that needs a live `A()` in ADOPT_SUCCESS). Controller finds creds from the managed model `L` (`q()` current + `p()` previous site Device Account); device verifies with `q()`. **Null (silent close)** when the MAC isn't in `L` (never cleanly adopted) — a retry can't fix it; back off. A stale CONNECTED context doesn't block re-link (old channel closed at adopt-success, reconnect counter bumped); it self-heals via CONNECTED_TIMEOUT. Mid-adopt drop → context resets to DISCOVERY via adoptChannelTimeout. FORGET_REQUEST(16384)=factory reset→discoverable; FORGET_REQUEST_NO_RESET(131072)=unmanage.

## Timing constants (EcspServerProperties / DeviceContextTimeout .class defaults)
updateServerRoutePeriod=21600000ms(6h) · connectedServerRouteExpire=86400s(24h) · adoptingServerRouteExpire=300s ·
informChannelTimeout=300000ms(5m) · informResetTimeout=30000ms(yaml) · adoptChannelTimeout=60000 · adoptSuccessTimeout=65000 ·
pendingTimeout/pendingPreAdoptTimeout=40000 · deviceVerify/systemVerifyTimeout=10000 · manageCoolDownTimeout=5000 · grpcTimeout=5000 · DEVICE_CONTEXT_TIMEOUT=900000(15m).
