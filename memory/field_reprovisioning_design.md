# Field WiFi Re-provisioning — Design

Status: **not yet implemented** — design agreed, ready to build.

## Problem

A deployed node (3 coin acceptors across Bakhaw/Daram/Samar, each in a locked box, no
casual USB access) can fall off the WiFi when the operator changes the management SSID/
password or swaps the AP. Today `setupWifi()` is wrapped in `RETRY_ON_FAIL` (main.cpp),
which is an **infinite loop** — a node with stale credentials never reaches `setupNetwork()`/
`setupWeb()`. It blinks red forever and the only recovery is a USB cable + the serial
provisioner (files.cpp `runProvisioningMode`). For remote nodes that's a boat trip.

Note the contrast: the *runtime* health check (network.cpp, every 30 s `WiFi.reconnect()`)
already self-heals transient drops. The gap is **escalation + an on-site, no-USB recovery
path**.

## Goals

1. Recover a fallen-off node **on-site with just a phone** — no USB.
2. Don't trip on transient outages — but don't make a knowing operator wait either.
3. Gate the recovery surface so it isn't a free "tell me your WiFi" service.
4. Always **auto-exit** — never strand a node in setup mode.
5. New credentials go through the existing config.json write path (single source of truth).

## Transport decision: SoftAP, not BLE

Espressif `WiFiProv` supports BLE and SoftAP over the same `protocomm` security layer, so
**BLE buys no security advantage** — its only real edge ("provision without going off-air")
is moot because we only arm *after* the node is already offline. SoftAP also reuses our
PsychicHttp stack + config.json write path, and avoids the BLE stack's flash/RAM cost
(tight against the OTA partitions). → **bespoke SoftAP captive flow.**

## Security: physical gate (locked box) is the real control

The node is in a **locked box**, and arming requires holding the **internal GPIO0 boot
button** (already read in coin.cpp; inside the enclosure). Anyone who can hold that button
has already opened the box — at which point they could just USB-reflash or `FACTORY_RESET`
anyway. So a PoP would guard a door that's already wide open beside it.

- **No PoP by default.** But use **`WIFI_PROV_SECURITY_1` with a NULL PoP**, NOT security0 —
  the NULL-PoP scheme still does X25519+AES, so the new WiFi password the operator types is
  **encrypted over the air** (not broadcast cleartext). Same UX (nothing to type), no leak.
- **Optional `provisioning_pop` field** in config.json for anyone wanting a second factor;
  absent → blank. (Rejected: reusing a real credential as the PoP — `wifi_password` goes
  stale exactly when re-provisioning is needed; `omada_password`/`network_key` create a
  blast-radius / coin-spoof escalation if the PoP leaks.)
- **Interlock bonus:** arming is only possible from the WiFi-dead state, so holding the
  button does nothing on a healthy node — it can't be triggered during normal coin operation.

## State machine (LED = single WS2812 on GPIO48)

Existing palette: white=boot, purple=master idle, orange=slave idle, green=coin blink,
red blink=error, red fast blink=halt.

- **Disconnected → SOLID RED, immediately and live.** Self-clears to idle the instant the
  background reconnect succeeds (so an AP-reboot flicker is harmless). No artificial wait —
  an operator who knows they changed the password acts at once.
- **Hold GPIO0 ~3 s while red** → enter provisioning. Pulse red faster during the hold as
  "registered," confirm flash at 3 s. (Double-gated by the red state, so 3 s is plenty.)
- **Provisioning active:** SoftAP `EllaFi-Setup-<last4 MAC>` up.
  - **blue blink** = armed & listening
  - **solid blue** = phone connected to the setup AP
  - **green flash** = creds accepted → write config.json → reboot
- **Window:** 5 min idle timeout; **kept alive while a client is associated** (cap ~15 min).
  On expiry → **reboot** → retries real creds → solid red again if still dead. The reboot
  cycle gives transient self-heal for free (no AP+STA juggling needed).

No explicit coin-slot power cut needed: the slot is only powered by the selling path, which
requires `isServiceReady()` — impossible without WiFi — so it's already off in this state.

## Boot integration

`setupWifi()` must stop being an infinite `RETRY_ON_FAIL`. On failure: continue boot into a
degraded state where the LED is solid red, provisioning is armable, and a background loop
keeps retrying the real (and optional backup) credentials.

## Cheap companion (optional tier): backup SSID list

`"wifi_backup": [{"ssid": "...", "password": "..."}]` — tried before lighting solid red, so
a pre-staged spare AP / known secondary recovers with **no human at the node** at all.

## Persistence

New creds from the SoftAP flow → write `/config.json` (LittleFS) → `esp_restart()`, reusing
the existing `CONFIG_OK → restart` pattern in files.cpp. config.json stays the source of truth
(don't fork creds into NVS).

## Ops signal (multi-node)

Siblings can't reach a node that's off the LAN (UDP broadcast needs the shared subnet). But
the master already tracks heartbeats in nodeMap — surface "node X missing, may need
re-provisioning" on the admin page / Healthchecks, **with a 1–2 min debounce** so it doesn't
fire on every AP reboot. (The immediate solid-red LED is a separate, undebounced layer.)

## Open / to confirm

- Confirm GPIO0 boot button is exposed *inside* the locked box on the rev-B board.
- Tunables: 3 s hold, 5 min idle / 15 min cap — all adjustable after field feel.
- Scope: ship backup-SSID tier with v1, or button-SoftAP flow first?
