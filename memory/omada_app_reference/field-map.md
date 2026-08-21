# Omada app field map — what an adopted device must fill

Catalogued from real EAP603 (Negra AP) app screens, 2026-08-21. This is the **target spec** for
the emulator (Phase A) and the ESP firmware (Phase B): every field the app renders, mapped to
whether the ESP can fill it **REAL**, must **SMUGGLE** it into an EAP-shaped field, must **FAKE**
a plausible constant, or can **IGNORE** it. Public repo — real serials / client hostnames redacted;
the `8C-86-DD` OUI is noted only because it's already in the doc as a borrowable TP-Link OUI.

## Device identity block (Overview ▸ lower) — all device-settable in the inform
| App field | Example | ESP plan |
|---|---|---|
| Model | `EAP603-Outdoor(US) v1.0` | **FAKE** → spoof `EAP225-Outdoor` / `EAP245` (old, always in the model registry; fewer radios = fewer config pushes to ack) |
| Serial Number | `Y2570570000••` | **FAKE** → synthetic, stable |
| MAC Address | `8C-86-DD-E3-77-••` | **FAKE** → TP-Link OUI + stable tail (adoption binds to it) |
| IP Address | `192.168.1.38` | **REAL** → the ESP's LAN IP |
| Firmware Version | `1.5.6 Build 20260629` | **SMUGGLE** → report EllaFi's real build here (`EllaFi 1.4.2 b84`) — a native, always-visible text field = free version reporting |

## Overview vitals — the honest ones
| App field | Example | ESP plan |
|---|---|---|
| Uptime | `Connected \| 2d 2h` | **REAL** → `millis()` uptime |
| CPU | `5%` | **REAL-ish** → FreeRTOS idle-task load (or a low constant) |
| Memory | `37%` | **REAL** → `1 - freeHeap/totalHeap` — the doc's core mapping |
| Rate ↑↓ | `↑49.48 Kbps ↓2.81 Mbps` | **FAKE/REAL** → report coin-API traffic or a small constant |
| Device Health | `10` (green) | **controller-DERIVED** from vitals — report healthy vitals → good score (probably not settable directly) |
| Clients | `5` | **REAL count** → `SessionCache.size()` (see client-table note) |

## Statistics ▸ Usage & Client History — periodic samples the controller graphs
CPU / Memory / Clients are **time-series**: report them each inform and the charts populate for
free. Channel-utilization graph is radio-shaped → **FAKE** a low line or leave empty.

## Clients table — DO NOT emit real client MACs (collision confirmed)
The screens prove the hazard: Negra's Clients list shows the real voucher/piso phones
(radio-associated to the **real** EAP). If the ESP node also claimed those MACs it's a double-claim
→ station flaps between two devices. **Decision: count-only** — report the real number as a vital,
empty/synthetic table. (Switches show wired "Clients" and mesh shows downlink relations — non-radio
relations exist, but only pursue one if the schema proves it device-settable. Safe fallback = count.)

## Linked Devices (mesh uplink/downlink) — the one honest non-radio relation
Screens: Negra ▸ Uplink = House Big AP, −67 dBm, Ch 157, Hop 0, Mesh Downlink 1, Up 206 / Down 172
Mbps. This is a **device→device** relation, not a client claim. The piso node genuinely transits an
uplink hop → we *may* be able to assert an honest uplink relation here without colliding. **Verify
settable in schema before relying on it.** Otherwise IGNORE (Hop 0, no uplink shown).

## Controller → device ACTIONS (the ⋮ menu) — must ACK, and some map to REAL ESP ops
| Action | ESP plan |
|---|---|
| **Reboot** | **REAL** → actually `esp_restart()` — the app becomes a real remote reboot |
| **Locate** | **REAL** → blink the ESP status LED (`ledError`/`ledReady` already exist) |
| **Firmware Upgrade** | **SMUGGLE→REAL** → trigger EllaFi's existing GitHub OTA path |
| Force Provision | **ACK** → accept + reply "provisioned" (no real radios to configure) |
| Config pushes (IP / Radios / WLAN / LED / Client-Access) | **ACK** → accept + echo; honor only what maps (LED) |
| Forget | **REAL** → drop the device account, return to Pending |

## Strategic reads (why this is better than the doc assumed)
1. **Firmware-version field = free real-info channel.** No smuggling trick needed — it's a native
   text field the app always shows. Put the EllaFi build there.
2. **Reboot / Locate / Firmware-Upgrade are genuine remote CONTROL,** not just monitoring — they map
   to real ESP capabilities the firmware already has. The Omada app becomes a real control surface.
3. **Vitals are honestly reportable** (uptime, memory, client count) → the device screen shows true
   node health, not theatre.
4. **Model choice is a lever:** a minimal old model = fewer config messages the emulator must fake.

## Still gated on the Week-1 crypto go/no-go — none of this ships until the key scheme is known.
