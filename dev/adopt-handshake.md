# ECSP adopt + manage-channel handshake (reverse-engineered, SW 5.15.24.19)

Follows discovery ([protocol-ecsp.md](protocol-ecsp.md)). Discovery→Pending is DONE (the beacon).
This is the back half: how a Pending device completes adoption and enters the inform/config loop.
All crypto primitives already solved: netty RSA private key recovered (→ derive public, verify
signatures), RC4 reproducible, `k(str)` reproduced in `dev/emulator/omada_crypto.py`.

## Transport
- Manage/adopt channel: **TCP 29811** (adopt v1 port 29812 also seen). Framing = 4-byte BE length +
  payload, same as discovery, but payload is **RC4-encrypted** with the negotiated session key
  (`NettyCodecAdapter.TcpEncryptInternalDecoder/Encoder`, `RC4Utils.crypt`). Some server TCP paths
  wrap SSL (`SslHandler`) — confirm whether the device manage channel does, or is raw RC4.

## Handshake sequence (device ⇄ controller)
1. **Adopt trigger:** user taps Adopt → controller marks device adopting, waits for the device to
   connect the manage channel ("start watching adopt result" in the logs).
2. **Session-key exchange** (`server/a/a/c.java:83`):
   device picks a random **RC4 session key**, `RSA-encrypt(key, controllerPubKey)`, base64 → sends as
   `body.key`. Controller: `RsaCipher.decryptByPrivateKey(base64decode(body.key))` → session key.
   Everything after is RC4'd with it (e.g. `encryptedHwId`, `encryptedOemId` are RC4(session)).
   → **We derive the public key from the recovered private key and encrypt our own chosen key.**
3. **Device verify:** `EcspV2VerifyInfo{ randomKeyForDeviceVerify, valid }` — controller issues a
   `randomKeyForDeviceVerify`; device echoes/uses it to prove liveness.
4. **PreConnectInfoResponse** (`server/e/c.java:30`), controller → device:
   `{ randomKeyForDeviceVerify, username, destIp }` — where/who to connect as.
5. **AdoptRequest** (`server/e/b.java:57`), controller → device:
   `AdoptRequest{ data = base64(RC4(payload)), hash = base64(SHA(payload)), sig = RsaCipher.sign(payload) }`.
   Device: verify `sig` with controller pub key, check `SHA`, RC4-decrypt `data` → the **device
   account (username/password) + deviceKey**. Store them.
6. **ADOPT_RESPONSE (type 32):** device → controller, accept. Status flips Pending → Adopting →
   Connected in the app.
7. **INFORM_REQUEST loop (type 256):** periodic vitals (uptime/cpuUti/memUti/clients), keyed by
   `k(deviceKey)`. Fills the device screen ([field-map](../memory/omada_app_reference/field-map.md)).
8. **SET_REQUEST / provision (type 4096):** controller → device — **the site WiFi/WAN config push
   (auto-config goal).** Device ACKs; we read the SSIDs / WAN settings out of this payload.

## Message types (already in the enum)
PRE_CONNECT_INFO 3 · PRE_CONNECT_INFO_RESPONSE 1048576 · DEVICE_VERIFY_INFO 1048577 ·
DEVICE_VERIFY_RESPONSE 1048578 · DEVICE_NEGOTIATION 1048580 · ADOPT_REQUEST 16 · ADOPT_RESPONSE 32 ·
INFORM_REQUEST 256 · SET_REQUEST 4096.

## Build order (each step testable against the lab controller's logs)
1. Bidirectional discovery: keep the UDP socket open, receive the controller's discovery/pre-connect
   response (currently the beacon is fire-and-forget → the adopt "watch" never gets a taker).
2. TCP connect 29811 + the RC4-session-key-via-RSA exchange → controller accepts the channel.
3. Device-verify + AdoptRequest handling: verify sig, RC4-decrypt, capture account + deviceKey.
4. ADOPT_RESPONSE → watch status go Connected in the app.
5. INFORM loop with real vitals; then capture the SET/provision payload = the WiFi/WAN config.

## Confirmed transport (2026-08-21, probed against the lab controller)
- Controller sends **NO UDP reply** to discovery (0 datagrams back). After Adopt is tapped it logs
  `start watching adopt result → timeout → Adopt … Timeout`: it **waits for the device to connect**.
- **Manage channel is device-initiated:** TCP 29811 (and 29812) accept and **wait for the device to
  speak first** (no server-initiated bytes). So the emulator must open the TCP connection and send
  the first message; the controller never dials the device.
- Device `ADOPT_RESPONSE` shape (`server/a/a/c.java`): `body.key = base64(RSA_encrypt(rc4SessionKey))`;
  `body.deviceInfo.encryptedHwId/encryptedOemId = base64(RC4(value, rc4SessionKey))`. Controller does
  `RsaCipher.decryptByPrivateKey` → session key, then RC4 for the rest. We hold the private key, so we
  derive the public key and encrypt our own session key.
- **Lab caveat:** on Docker-for-Mac the device's 29810 identity collides with the controller's
  published port; the emulator connecting *out* to 29811 works from the host, but if any step needs
  the controller to reach the device, run the emulator as the compose `emulator` service (in-network,
  reachable by name).

## Manage-channel entry — DEVICE_VERIFY_INFO (fully mapped 2026-08-21)
- **First message the device sends** on TCP 29811: `DEVICE_VERIFY_INFO` (type 1048577), handler
  `server/a/b/c.java`. Body `{ auth, randomKeyForSystemVerify }`.
- The controller requires a **live device context in `ADOPTING_PRE_CONNECT`** (created by discovery,
  advanced when you tap Adopt) — else it closes the channel ("device context is null"/"status not
  matched"). So: keep discovery alive (Pending) → tap Adopt → connect the manage channel *promptly*
  (before the pending context times out ~40s) and send DEVICE_VERIFY_INFO.
- **auth** = `EcspUtils.calculateEcsp2Auth(username, MD5(password), randomKeyForSystemVerify)` =
  `SHA256( SHA256(username + MD5(password)) + randomKey )`, UPPER hex. Factory device uses
  **admin/admin**. Reproduced + in `dev/emulator/manage_crypto.py::ecsp2_auth`.
- Controller replies `DEVICE_VERIFY_RESPONSE` (1048578) → then PRE_CONNECT → the AdoptRequest that
  delivers the real account + deviceKey (RC4+RSA-signed, above).
- **Open (resolve empirically when building the client):** manage-channel framing — is
  DEVICE_VERIFY_INFO plaintext, or RC4'd with the fixed `RC4Utils.getEncryptKey()` (embedded, like the
  RSA key) until a session key is negotiated? Try plaintext first, then fixed-key RC4, watch the log.

## Crypto toolkit status (all built + verified)
`dev/emulator/omada_crypto.py` — `k(str)` ✓ vs JVM. `dev/emulator/manage_crypto.py` — RC4 ✓ vs
canonical vector, RSA pub-encrypt/priv-decrypt ✓ round-trip, SHA1withRSA verify, ecsp2_auth. Nothing
crypto-side is unknown; the remaining work is assembling the stateful client + the framing question.

## Channel framing — RESOLVED empirically (2026-08-21, DEBUG-instrumented)
- **Ports** (controller startup log): discovery 29810 · manage **v1 29811** · manage **v2 29814** ·
  adopt v1 29812 · upgrade 29813 · transfer v2 29815.
- **v1 (29811) = PLAINTEXT** JSON frames (it parsed our DEVICE_VERIFY_INFO, logged "do not handle"
  — v1 server doesn't handle that V2 type).
- **v2 (29814) = TLS-WRAPPED**, NOT raw-RC4. Confirmed by handshake probe: 29814 completes TLS
  (`TLS_AES_256_GCM_SHA384`, server presents a cert, no client-auth required); 29811/29812 refuse TLS.
  The `RttyNettyTcpServer` pipeline does `addSslHandlerIfNeeded` (`addFirst("negotiation", SslHandler)`)
  → **length-field decoder → default(plaintext) decoder → dispatcher**. So the earlier "silent close on
  plaintext to 29814" was the **TLS handshake failing on raw bytes** — nothing reached the ecsp decoder,
  which is why NO parse log appeared. Once wrapped in TLS, a plaintext length-prefixed
  `DEVICE_VERIFY_INFO` reaches the handler: log `c.t.s.e.s.a.b.c: handle device verify Message for
  8C-86-DD-…` then rejects on **state** ("device status … not matched", because Pending≠ADOPTING).
- **Inside the TLS tunnel the frames are plaintext length-prefixed JSON** (4-byte BE length + payload).
- **RC4 is a LATER upgrade, not the outer framing.** `NettyChannel.reinitRc4Handler(byte[] key)`
  hot-swaps the pipeline's length-field codec to `TcpEncryptInternalDecoder/Encoder` — but only AFTER
  adopt, keyed by `serverRouteCache.getKey()` = the **per-device session key negotiated during adopt**
  (device RSA-wraps its own RC4 session key in ADOPT_RESPONSE). It is NOT the fixed `getEncryptKey()`.
  Trigger path (`server/c/f.java:106`): a `SET_REQUEST` on the ADOPT port followed by a successful
  `REBUILD_RESPONSE` → `reinitRc4Handler(sessionKey)`. So: TLS+plaintext for the whole adopt, then
  RC4-inside-TLS for the post-adopt manage traffic. (The fixed `getEncryptKey()` we recovered via TEA
  is unused on this channel — keep it noted but it was a wrong turn.)
- **How this was nailed:** bumped the controller's `log4j2.properties` `logger.ecsp` to DEBUG (+ a
  `io.netty…LoggingHandler` byte logger) and restarted; then drove framing variants and read
  `logs/server.log`. See RUNBOOK "instrumenting the controller".

## Manage-channel state machine (from the handlers, 2026-08-21)
- Dispatcher `server/c/n.java` routes by type; `o.java` maps ordinals: PRE_CONNECT_INFO=case1,
  DEVICE_VERIFY_INFO=case2, DEVICE_NEGOTIATION=case4, INFORM_REQUEST=case6, REBUILD_REQUEST=case11…
- **DEVICE_VERIFY handler `server/a/b/c.java`** requires context status ∈ {ADOPTING_DEVICE_VERIFY,
  CONNECTED, ADOPT_SUCCESS} (NOT ADOPTING_PRE_CONNECT). auth is checked against the **controller-
  assigned account list** `context.p()` (EcspAdoptInfo username/password), via
  `calculateEcsp2Auth(username, MD5(password), context.channelRandomKey)` — so the device must already
  hold its assigned account (from the AdoptRequest) before verify passes. Factory admin/admin only
  matches if that pair is in the adopt-info list.
- **PRE_CONNECT_INFO handler `server/a/b/h.java`**: only proceeds when the port's ServerType is ADOPT
  (→ publishes a domain event to `adoptPreConnectInfoSubscriber` `server/a/a/b.java`, which drives the
  account assignment + AdoptRequest) or TRANSFER; else closes ("not manage or transfer server"). Need
  to confirm 29814's ServerType (ADOPT vs MANAGE) empirically — drive PRE_CONNECT_INFO and read the log.

## CONFIRMED end-to-end V2 adopt AUTH flow (2026-08-21, live against the lab controller)
Emulated EAP225 (`dev/emulator/adopt_full.py`) passes Omada adoption authentication in full:
1. **Discovery** (UDP 29810, persistent socket, `ip=192.168.65.1` = host as the container sees it) →
   device Pending. MUST listen on the same socket for the reply.
2. User taps Adopt → controller → `ADOPTING_PENDING_PRE_ADOPT`, sends **`PRE_ADOPT_REQUEST` via UDP**
   `{mac, ip, adoptPort=29814}` back to that socket (Docker NAT passes it because we listen there).
3. Device connects **TLS 29814**, `PRE_CONNECT_INFO{needUsername:true}` → controller replies
   `PRE_CONNECT_INFO_RESPONSE{randomKeyForDeviceVerify, username}` — **username auto-syncs** (the
   controller returns whatever the operator typed; only sent when `needUsername:true`).
4. `DEVICE_VERIFY_INFO{auth=ecsp2_auth(username, MD5(factoryPass), rkdv), randomKeyForSystemVerify}`
   → controller: `deviceAuthPair match deviceVerifyInfo auth success` → `DEVICE_VERIFY_RESPONSE`
   `error:0` with `body.auth` = the controller proving itself (ecsp2_auth over our randomKeyForSystem
   Verify). V2 = **challenge-response over the device's factory-default password** (admin/admin here);
   the password is NEVER sent — the site account is provisioned AFTER (see below).
5. `SYSTEM_VERIFY_RESULT(1048579, error:0)` → controller `VERIFY_RESULT_ACK(1048585)`. **Mutual auth
   done.** Controller sets up the manage server route (`k.java` → `server/e/d.java`).

Message types (MessageType.java): SYSTEM_VERIFY_RESULT=1048579, VERIFY_RESULT_ACK=1048585,
INIT_SYNC=4352, INIT_SYNC_RESULT=1048582, INFORM_REQUEST=256, SET_REQUEST=4096, SET_RESPONSE=8192.

### CONFIG CHANNEL — SOLVED (2026-08-21): device fully adopted, config RC4-decrypted
After `VERIFY_RESULT_ACK` the device sends **`DEVICE_NEGOTIATION` (1048580)** on the same channel,
body = `ApAdoptRespV2Body`:
- `key` = base64(RSA_pub(rc4SessionKey)) — our chosen RC4 session key, RSA-wrapped. Controller
  `RsaCipher.decryptByPrivateKey` → the key, then `reinitRc4Handler` flips the channel to RC4 **both
  directions**. Our outgoing frames must then be RC4-framed too (RC4(4-byte total-len)+RC4(payload)).
- `configVersion` (Integer, 0 for fresh), `devCap` (ApDevCapV2 — MUST include `logNotification`, else
  the config builder NPEs at `deviceimage.ap.c`), `deviceInfo`, `controllerSetting`,
  `components`/`components_v2` (declare wlanBasic/ssidInform/… so the controller pushes their config —
  with none declared it logs "send empty setting"), `notSupportComponents` = `{}` (Map, NOT `[]`).
- Controller then RC4-pushes **`SYSTEM_NEGOTIATION` (1048581)** = the V2 initial config sync
  (`sendSystemNegotiation`, the V2 analog of INIT_SYNC). Device acks with **`INIT_SYNC_RESULT`
  (1048582)**. Controller logs `receive adopt success event` — **device is ADOPTED**.
- **The `SYSTEM_NEGOTIATION` body carries the device account THROUGH THE PROTOCOL:**
  `userAccount{ curUsername, curPassword=MD5(factory), newUsername, newPassword=MD5(real) }` — the
  controller provisions the real device-account password (as MD5) to the device, encrypted. Plus
  `timeSetting`, `dst`, `controllerSetting` (managePort, portalHttp/Https, logoutDomain),
  `components` manifest, `informInterval`. (Full capture → gitignored `dev/captured-config.local`.)
- Driver: `dev/emulator/adopt_full.py` (discovery+UDP-listen → PRE_ADOPT → TLS adopt channel → verify
  → negotiation → RC4 config, adaptive plaintext/RC4 reader + RC4 sender).

### REMAINING: per-component SET_REQUEST (the SSID/passkey values)
The SSID/WAN VALUES are not in the initial sync — they arrive as per-component `SET_REQUEST` (4096)
after the device reaches CONNECTED and starts INFORM. Our channel currently closes after
`INIT_SYNC_RESULT` (controller re-negotiates each reconnect). Next: keep a persistent manage channel /
drive the INFORM loop so the controller streams the wlanBasic/ssid SET_REQUEST.

### (historical) REMAINING: post-auth config channel (the SET_REQUEST provisioning = SSID/WAN)
After `VERIFY_RESULT_ACK` the controller does NOT push `INIT_SYNC` on the adopt channel (device sat
idle → adopt retried). Device must open the **manage channel** and do **`DEVICE_NEGOTIATION`
(1048580)** to establish an **RC4 session key** (→ `NettyChannel.reinitRc4Handler(key)`), after which
`INIT_SYNC`/`SET_REQUEST` flow **RC4-encrypted** with that key. TODO: reverse the DEVICE_NEGOTIATION
body (likely device sends an RSA-wrapped RC4 session key, like the V1 ADOPT_RESPONSE `body.key`), then
RC4-decrypt the config. All primitives (RSA priv→pub, RC4) already in `manage_crypto.py`.

## Downstream message shapes (from source `server/e/b.java`, to parse against a real capture)
- **PRE_ADOPT_REQUEST (type 2)**, controller→device: body `PreAdoptRequest{ mac, ip, adoptPort }` —
  tells the device the controller's IP + which adopt port to use.
- **ADOPT_REQUEST (type 16)**, controller→device: body `AdoptRequest{ data, hash, sig }` where
  `data = base64(payload)`, `hash = base64(SHA(payload))`, `sig = RsaCipher.sign(payload)`
  (SHA1withRSA). Device: verify `sig` w/ controller pub key (we hold priv→pub), check
  `SHA(payload)==hash`, then RC4-decrypt `payload` with the **session key** → account + deviceKey.
- **Session-key ordering (verify against capture, do NOT guess):** the device's RC4 session key is
  RSA-wrapped and handed up during pre-connect/pre-adopt BEFORE the controller can RC4-encrypt the
  account in `data`. Get this order right from the real frames, not by assumption.
- Client already dumps every frame; with DEBUG on we read the real PRE_ADOPT/ADOPT_REQUEST the instant
  Adopt is tapped, then build ADOPT_RESPONSE against ground truth. **Blind implementation deferred by
  design** — the session-key ordering makes untested code a real correctness risk.

### (superseded) fixed-RC4-key note
- `RC4Utils.getEncryptKey()` = `TEAUtils.decrypt(ENCRYPTED_ENCRYPT_KEY)`; TEA `KEY={-707509657,
  -1887749506,1427902494,-1686606610}`, `DELTA=0x9E3779B9`, 64 rounds. Recovered to `rc4key.local`.
  Turned out NOT to be the manage-channel framing key (that's TLS + per-device session key). Retained
  only for completeness.

## Remaining build (precise, all crypto solved)
1. `tea_decrypt` in manage_crypto → recover the fixed RC4 key.
2. RC4-frame the v2 (29814) channel with it; send DEVICE_VERIFY_INFO(admin/admin auth).
3. Timing: keep discovery alive (Pending) → tap Adopt (→ ADOPTING_PRE_CONNECT) → the verify that
   lands in that window draws DEVICE_VERIFY_RESPONSE.
4. Drive PRE_CONNECT → AdoptRequest (verify SHA1withRSA sig, RC4-decrypt) → capture account+deviceKey
   → ADOPT_RESPONSE (our RC4 session key, RSA-wrapped) → INFORM loop → SET/provision = auto-config.

## Sandbox note
The manage channel is an RSA+RC4 stateful handshake. This session's auto classifier repeatedly
blocked crypto-script execution and git commits — building/iterating this will stall on that.
Allowlist `python3 dev/emulator/*` and `git commit` for a smooth build loop.
