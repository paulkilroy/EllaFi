# Omada device emulator — Phase A (feasibility, proven)

**Goal:** make an ESP32 (EllaFi pisowifi node) appear in the Omada app as an adopted device, so it can
be monitored/reached through the controller's existing outbound cloud channel — no VPS, no tunnel,
sidestepping Starlink CGNAT. This is **Phase A**: prove the whole device-plane protocol with a Python
emulator against a local Docker SW controller. Phase B (ESP32 firmware port) is deferred.

**Status: feasibility PROVEN.** An emulated EAP225 is discovered, adopted, authenticated, provisioned
(its real SSID + passkey pulled from the provisioning push), and holds a stable Connected session with
live vitals. What is *not* yet rock-solid is keeping the controller's status label permanently on
"Connected" through config re-applies — see "Remaining" at the bottom.

Everything runs against the throwaway Docker controller on localhost. Interop reverse-engineering is on
the user's own controller/device (legitimate). **Secrets stay in gitignored `.local` files; the repo is
public and describes mechanism only.**

## What works, end to end
1. **Discovery** (UDP 29810) → device shows **Pending/adoptable**. `discovery_beacon.py`.
2. **Adopt tap** → controller sends **`PRE_ADOPT_REQUEST` via UDP** `{adoptPort=29814}` back to the
   device's discovery socket (received through Docker NAT by listening on the same socket +
   advertising `ip=192.168.65.1` = host as the container sees it).
3. **Adopt channel = TLS on 29814** (the key transport discovery — NOT raw RC4). Inside TLS, frames are
   plaintext length-prefixed JSON `{header, body}`.
4. **Handshake:** `PRE_CONNECT_INFO{needUsername}` → `PRE_CONNECT_INFO_RESPONSE{randomKeyForDeviceVerify,
   username}` (username auto-syncs — the controller returns what the operator typed) →
   `DEVICE_VERIFY_INFO{auth=ecsp2_auth(user, MD5(factoryPass), rkdv)}` → `DEVICE_VERIFY_RESPONSE(error:0)`
   → `SYSTEM_VERIFY_RESULT` → `VERIFY_RESULT_ACK`. Mutual auth complete (factory `admin/admin`).
5. **RC4 config channel:** `DEVICE_NEGOTIATION` hands the controller our RSA-wrapped RC4 session key →
   the channel's **outbound** flips to RC4 (we decode it), **inbound stays plaintext** (asymmetric
   reinit — send acks plaintext). Controller RC4-pushes `SYSTEM_NEGOTIATION` (initial config sync incl.
   the provisioned `userAccount{newPassword=MD5}`). We ack `INIT_SYNC_RESULT` → device **CONNECTED**.
6. **Provisioning:** controller streams `SET_REQUEST` (type 4096) with the full site config —
   `lanSetting`, `wirelessBasic/Adv`, **`ssid_2G/5G` (SSID name + passkey)**, portal, qos, snmp, etc.
   We echo `SET_RESPONSE{sequenceId, configVersion}` to confirm it applied.
7. **Stay online:** fast (~3s) **live-vitals INFORM** heartbeat refreshes the connected route so it
   doesn't expire (~60s TTL); reports `deviceInfo` (IP, cpu/mem/temp), per-radio `WirelessInfo`
   (health spider graph), and `ssidStats` (SSIDs broadcasting).
8. **Auto-reconnect:** on a drop, proactively re-opens the manage channel (no manual re-adopt).

## The crypto (all recovered + reproduced; keys in gitignored `.local`)
- **TLS** on 29814 (server cert, no client-auth).
- **RSA 1024** netty keypair (private key recovered, TEA-deobfuscated) → derive public; verify
  `SHA1withRSA`; `rsa_decrypt_pub` reverses the controller's `encryptByPrivateKey` account push.
- **RC4** (standard) for the post-negotiation config channel, keyed by our own session key.
- **`ecsp2_auth`** = `SHA256(SHA256(user+MD5(pass))+randomKey)` UPPER hex; factory `admin/admin`.
- `k(str)` SHA1PRNG, TEA — reproduced/verified. See `manage_crypto.py`, `omada_crypto.py`.

## Files
- `emulator/adopt_full.py` — the end-to-end driver (discovery + UDP listen → TLS adopt → verify →
  negotiation → RC4 config → SET_REQUEST capture → live-vitals heartbeat → auto-reconnect).
- `emulator/discovery_beacon.py` — discovery beacon / spoofed device identity.
- `emulator/manage_crypto.py`, `emulator/omada_crypto.py` — crypto toolkit (RSA/RC4/ecsp2_auth/k/TEA).
- `adopt-handshake.md` — the full protocol spec + message types + the connected-lifecycle findings.
- gitignored: `findings-secrets.local` (RSA key), `rc4key.local`, `captured-config.local` (decrypted
  config incl. SSID passkey + device-account MD5 — real credentials, never committed).

## Run (lab)
```
~/.platformio/penv/bin/python3 -u dev/emulator/adopt_full.py
# tap Adopt on the device in the app, username/password admin/admin (the emulator's factory default)
```

## Message types (v2)
DISCOVERY=1 · PRE_ADOPT_REQUEST=2 · PRE_CONNECT_INFO=3 · INFORM_REQUEST=256 · INFORM_RESPONSE=512 ·
SET_REQUEST=4096 · SET_RESPONSE=8192 · INIT_SYNC=4352 · REBUILD_REQUEST=36864 · ADOPT_REQUEST=16 ·
DEVICE_VERIFY_INFO=1048577 · DEVICE_VERIFY_RESPONSE=1048578 · SYSTEM_VERIFY_RESULT=1048579 ·
DEVICE_NEGOTIATION=1048580 · SYSTEM_NEGOTIATION=1048581 · INIT_SYNC_RESULT=1048582 ·
VERIFY_RESULT_ACK=1048585 · INIT_SYNC_RESULT_ACK=1048586.

## Connected-lifecycle findings (the hard-won ones)
- Connected **route expires ~60s** (`connectedServerRouteExpire`); `INFORM` refreshes it only via the
  keepalive (`server/a/b/d.java` → `context/c.java`) which requires status==CONNECTED AND channel==A().
  A slow heartbeat lets the TTL lapse → fast 3s heartbeat fixes it.
- **INFORM/INIT_SYNC_RESULT race:** don't INFORM immediately after `INIT_SYNC_RESULT` — the status goes
  CONNECTED asynchronously; an INFORM racing ahead hits ADOPT_SUCCESS in the keepalive → channel closed.
- **`informIdle`:** byte-identical informs get flagged idle → make vitals progress (upTime climbs).
- **Configuring → Connected:** `SET_RESPONSE` must echo `{sequenceId, configVersion}`, and the INFORM
  must report `ssidStats` (SSIDs on the air) or the manager keeps it in Configuring/Provisioning.
- Device status codes (`DeviceStatusEnum`): 20=Pending, 10=Provisioning, 11=Configuring,
  14=Connected, 30–33=Heartbeat-Missed.

## Remaining (Phase A polish — well-scoped, documented)
Keeping the status label *permanently* on Connected through config re-applies. Each layer cleared
revealed the next: keepalive → informIdle → INFORM race → ssidStats → **per-feature config-apply
confirmation** and **site-state validation** (`Invalid v2 adopt process with pending site` reset the
device after a config-apply whose features "timed out"). A production-grade fake AP needs to emulate
more of a real device's config-application + operational reporting (per-feature apply results, complete
inform component set). This is the difference between "protocol proven" (done) and "never flickers."
Pick up from `adopt-handshake.md`; re-enable controller DEBUG (`log4j2.properties` `logger.ecsp`/`omada`
= debug, auto-reloads) to trace the manager's status computation.
