# Week-1 go/no-go — VERDICT: **GO**

Extracted from the lab SW controller **5.15.24.19** (jadx over `manager-core`, `omada-common`,
`device-gateway-core`, `ecsp-common`), 2026-08-21. **Mechanism only — no key material in this public
repo.** Recovered keys/creds live in gitignored `dev/findings-secrets.local` when we build the emulator.

## Why GO — nothing on the wire is unreproducible per-device
The device-plane ("ECSP") crypto is entirely **static + recoverable**:

1. **Wire transport RSA key is a fixed embedded constant.**
   `ecsp-common … encrypt/GlobalConfig`: `ENCRYPTED_NETTY_RSA_PRIVATE_KEY` (byte[]) is TEA-obfuscated
   (`TEAUtils.decrypt(...)`, TEA key also in the binary) → the netty RSA **private** key, identical
   across all controllers of this version. Recoverable locally; not per-device.
2. **Symmetric key derivation is deterministic from a string.**
   `omada-common … util/b/b.k(str)`: `SecureRandom.getInstance("SHA1PRNG").setSeed(str)` →
   `KeyGenerator("AES").init(128)` → key. SHA1PRNG-seeded = **fully determined by the seed string**,
   reproducible on any JVM, in Python/Node, and on ESP32/mbedTLS.
3. **Crypto suite** (`ecsp-common … encrypt/`): RSA, AES (ECB+CBC/PKCS5&7), RC4, TEA, MD5, SHA/SHA-256
   — all standard, all reimplementable in firmware.
4. **Adopt flow confirmed** (`manager-core` device/application/adoption + port/transport):
   discovery → `PRE_ADOPT_REQUEST` (first inform) → controller shows **Pending** → human adopts →
   controller pushes a **device account** (username/password) + **`deviceKey`** string → later informs
   keyed by `k(deviceKey)`. Exactly the UniFi-shaped model the doc predicted.

**No unreproducible per-device secret gates the handshake.** The lone "secret" (netty RSA private key)
is an embedded constant we extract once. Emulating a device is mechanically achievable → the two-week
Phase A is on.

## Confirmed protocol vocabulary (for the emulator)
- Framing: `com.tplink.smb.ecsp.protocol.packet.EcspMessage`; body `DeviceInformMessage`; internal gRPC.
- Ports: 29810/udp discovery, 29811–29814/tcp manage/adopt/upgrade.
- Adopt DTOs: `isFirstInform`, `pendingAdoptInfo`, `DeviceAdoptInfoService`, `setAdoptResp`,
  `getAdoptFailType`, `autoAdopt`; identity = `{sn, deviceKey, mac, deviceModel, userName, password, deviceHwVer}`.

## Remaining before an emulator handshake (Phase A, unblocked)
1. Recover the netty RSA private key locally (TEA-deobfuscate the blob) → **gitignored**, never committed.
2. Port SHA1PRNG-key-derivation to Python (documented deterministic algo).
3. Implement discovery → pre-adopt inform → adopt-accept → heartbeat against the lab controller;
   **decrypt a real device's first inform end-to-end** = the actual proof (dynamic confirmation).

## Handling note (public repo)
Recovered vendor key material + any device-account creds stay in gitignored local files. Committed docs
describe mechanism only. This is interop RE on our own controller/device — no third-party secrets.
