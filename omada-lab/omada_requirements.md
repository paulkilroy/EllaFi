# Omada integration — requirements & status

Original requirements for making the EllaFi piso node work with the Omada controller, plus live
status. Everything below is **proven at the emulator/protocol level** (`omada-lab/emulator` against a
6.2.14.11 lab controller). The ESP32 firmware port (**Phase B**) makes each real on-device.

Legend: ✅ proven · 🟡 mechanism known, not built · ❌ not started · (n/a) not an Omada concern

## Health monitoring
| Item | Status |
|---|---|
| Up / down | ✅ |
| CPU | ✅ |
| Temperature | ✅ |
| Critical errors | ✅ — raised into the device's **Logs → Alerts** via the inform `log` field; content renders verbatim (e.g. "coin acceptor jam") |

## Activity monitoring
| Item | Status |
|---|---|
| Clients connected | ❌ **removed** — AP-reported clients are ghosts ("This client does not exist" on click) and real clients roam AP↔AP → flap. Not worth it. |
| Wi-Fi RSSI | ✅ **free** — the ESP32 is a real Wi-Fi client of its EAP, which reports it with RSSI automatically. (The ESP32-as-AP's own topology "Link" is intentionally blank; its connection shows on the client record.) |
| Client **activity log** | 🟡 decided, not built — see below |

**Activity log — decided: log to both surfaces** from one ESP32 event stream. Only events the firmware
itself initiates: **AUTH, EXTEND, REFUND, PAUSE, RESUME**. (Natural session *expiry* is not in the
stream — the controller owns the countdown and drops the session silently, so the ESP32 never sees a
real expire event; Omada already shows expiry natively.)
1. **Omada** device → Logs → Events (inform `log` injection). One device Event key (`AP_CH_C`); Content
   leads with the action (`[EllaFi] COIN INSERT - client … +30 min`). The **Type column is a borrowed
   label** ("EAP Channel Changed") — unavoidable, Omada's catalog has no AP-postable neutral type.
   Critical errors → Alerts.
2. **EllaFi admin** — a dedicated activity-log view with proper columns (Action / Client / Amount /
   Remaining / Node). The clean surface; will later be plumbed into the Omada UI.

## Firmware — GitHub OTA, device knows if out of date ✅
Proven end-to-end: ESP32 checks GitHub hourly → flips the version it *reports* to Omada (low =
update-available, high = current) → controller's cloud check lights the **native upgrade indicator** →
on approve, device **ignores the pushed TP-Link image and pulls the EllaFi `.bin` from GitHub**, then
reports high to clear it.
- Needs the genuine EAP225-Outdoor identity (real `hwId`/`oemId`, `5.x` version scheme).
- Fallback (no TP-Link cloud): write `last_fw_ver`/`fw_url` into the controller's `modelfw` collection.

## Config
| Item | Status |
|---|---|
| Network config pushed to device | ✅ `SET_REQUEST` applied + echoed |
| Auto-config ("could be huge") | ❌ not started |
| Piso settings (rates, session length) via UI | (n/a) EllaFi admin, not Omada |

## Access
| Item | Status |
|---|---|
| iPhone | ✅ node visible in the Omada iOS app; EllaFi admin is web/iPhone-reachable |
| High-WAF read-only view for Ella | ❌ not built |

## Notes
- **Client auth is orthogonal.** Coin sessions authorize clients via Omada's **External Portal** REST
  (`/api/v2/hotspot/extPortal/auth {clientMac, time}` + client `extend`/`disconnect`) — coins buy time
  on an auth call, **no vouchers**. The device-plane adoption above monitors the **node**, not clients.
- Mechanism detail: `omada-lab/omada-protocol-reference.md` + the `omada_62_adopt` memory note.
