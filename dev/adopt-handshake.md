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

## Channel framing — RESOLVED empirically (2026-08-21)
- **Ports** (controller startup log): discovery 29810 · manage **v1 29811** · manage **v2 29814** ·
  adopt v1 29812 · upgrade 29813 · transfer v2 29815.
- **v1 (29811) = PLAINTEXT** JSON frames (it parsed our DEVICE_VERIFY_INFO, logged "do not handle"
  — v1 server doesn't handle that V2 type).
- **v2 (29814) = RC4-framed** with the fixed embedded key (silently closes plaintext; no parse log).
  DEVICE_VERIFY_INFO (a V2 type) must go to 29814 **RC4'd**.
- **Fixed RC4 key** = `RC4Utils.getEncryptKey()` = `TEAUtils.decrypt(ENCRYPTED_ENCRYPT_KEY)`.
  TEA params (recover in Python, then RC4 the v2 frames): `KEY={-707509657,-1887749506,1427902494,
  -1686606610}`, `DELTA=0x9E3779B9` (-1640531527), 64 rounds, 8-byte blocks, first decrypted byte =
  pad length. `ENCRYPTED_ENCRYPT_KEY` byte[] is in `RC4Utils` (dev/_jadx). → add `tea_decrypt` +
  `RC4_FIXED_KEY` to manage_crypto.py.

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
