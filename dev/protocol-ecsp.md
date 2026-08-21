# ECSP device-plane protocol — wire spec (extracted, SW 5.15.24.19)

Reverse-engineered from `ecsp-common` + `ecsp-transporter-netty` (jadx). This is what the emulator
sends/receives. Crypto details in [findings-crypto.md](findings-crypto.md); key derivation is
reproduced + verified in `dev/emulator/omada_crypto.py`.

## Transport
- **Discovery**: UDP **29810**. One datagram = one message.
- **Manage / adopt / upgrade**: TCP **29811–29814**.
- **Framing** (`EcspLengthFrameDecoder`): 4-byte **big-endian** length prefix + payload.
  `PACKET_LENGTH_FIELD_LEN=4`; max discovery frame 2000 B, adopt 1 MB, manage 10 MB.

## Message envelope — JSON (not protobuf)
`EcspMessage.pack()` = `pojo2Json({header, body}).getBytes()`. So the payload is UTF-8 JSON:
```json
{ "header": { "seq":.., "version":"..", "verCap":.., "device":"..", "mac":"..",
              "type":<int>, "timestamp":.., "ip":"..", "compress":null, "seqNum":.., "last":.. },
  "body":   { ...message-type-specific... } }
```
- `header.type` selects the message + its body class (table below).
- `header.mac` is normalized (`MacUtils.formatMacDb`).
- `header.compress` non-null ⇒ body is LZO-compressed (`LzoUtils`); null ⇒ plain.

## Message types (header.type → body class)
| type | name | body |
|---|---|---|
| 1 | discovery | BaseDiscovery |
| 2 | pre.adopt.request | PreAdoptRequest |
| 3 | pre.connect.info | PreConnectInfo |
| 16 | adopt.request | AdoptRequest |
| 32 | adopt.response | BaseAdoptResponse |
| 256 | inform.request | BaseInformRequest |
| 512 | inform.response | NullMessage |
| 4096 / 8192 | set.request / response | BaseConfig* |
| 16384 / 20480 | forget.request / response | ForgetRequest / Null |
| 24576 / 28672 | get.request / response | BaseGet* |
| 1048577 / 1048578 | device.verify / response | DeviceVerifyInfo / Response |
| 32768 / 65536 | upgrade.request / response | BaseUpgrade* |
(+ notify, portal, rebuild, report, file-transfer, v2 variants — see MessageType enum.)

## Crypto layering (to confirm per-channel while building the beacon)
- Suite (`ecsp-common util/encrypt`): RSA (fixed embedded netty key — **recovered**), AES (`k(str)`
  SHA1PRNG-derived — **reproduced**), RC4, TEA, MD5/SHA/SHA-256, Base64.
- **First thing to nail for the beacon:** is the UDP discovery payload plaintext JSON, lightly
  wrapped, or AES-encrypted (and if so, under which seed)? The `NettyUdpHandler` hands the datagram
  to a `ChannelHandler`/codec — trace `handler.received()` (NettyCodecAdapter) to see the discovery
  decrypt path. Manage/adopt channel: RSA handshake → AES session, keys via `k(deviceKey)`.

## Adoption sequence (device ⇄ controller)
1. Device → **DISCOVERY** (type 1, UDP 29810), identity in BaseDiscovery (mac, model, version…).
2. Controller shows device **Pending** in the app.  ← first visible win target.
3. Human taps **Adopt** → controller → device **ADOPT_REQUEST** (16) with a device account
   (username/password) + `deviceKey`.
4. Device → **ADOPT_RESPONSE** (32) accept; connects to the manage channel keyed by `k(deviceKey)`.
5. Device → periodic **INFORM_REQUEST** (256) with vitals (the field map:
   [omada_app_reference/field-map.md](../memory/omada_app_reference/field-map.md)).
6. Controller → SET/GET/REBOOT/UPGRADE/FORGET commands; device ACKs (some map to real ESP ops).

## Emulator build order
1. **Discovery beacon** (this step): send a DISCOVERY datagram to the controller (directed to the
   `controller` service on the lab bridge net) → device appears **Pending**. Iterate crypto/fields
   against the controller's device-gateway logs until it accepts.
2. Handle ADOPT_REQUEST → send ADOPT_RESPONSE, store the pushed deviceKey/account.
3. INFORM loop with real-ish vitals → device shows **Connected** with uptime/CPU/mem/clients.
