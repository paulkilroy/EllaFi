# Omada integration — requirements & status

The original requirements for making the EllaFi piso node work with the Omada controller, with
live status. Status reflects what is **proven at the emulator/protocol level** (`omada-lab/emulator`
driven against a 6.2.14.11 lab controller). The ESP32 firmware port (**Phase B**) is what makes
each one real on-device.

Legend: ✅ done/proven · 🟡 mechanism known, not wired yet · ❌ not started · (n/a) not an Omada concern

## Health monitoring
- ✅ Up / down
- ✅ CPU
- ✅ Temperature
- ✅ Critical errors — device raises entries into **Device → Logs (Alerts/Events)** via the
  inform `log` field (`EapSysLog`); content renders verbatim, so any coin-jam / hardware fault
  surfaces as a native alert.

## Activity monitoring
- ✅ Clients connected
- 🟡 Client log — mechanism exists (`clientConnection` / `connRec` inform fields); not wired yet
- ✅ Wi-Fi RSSI (node's uplink to its AP) — reported as the node's **client entry** on the
  upstream AP (added later in the requirements, prioritized over temperature)

## Firmware
- ✅ GitHub-hosted; device knows if it's out of date — **full loop proven**:
  ESP32 checks GitHub hourly → flips the version it *reports* to Omada (low = update-available,
  high = current) → the controller's cloud check lights the **native "upgrade available"
  indicator** → on approve, the device **ignores the pushed TP-Link image and pulls the EllaFi
  `.bin` from GitHub**, then reports the high version to clear the prompt.
  - Requires the node to present the genuine EAP225-Outdoor identity (real `hwId`/`oemId`, `5.x`
    version scheme) so the controller's cloud firmware check recognizes it.
  - Fallback (no TP-Link dependency): write `last_fw_ver`/`fw_url` into the controller's `modelfw`
    collection directly on a self-hosted controller.

## Config
- ✅ Network config pushed to device — `SET_REQUEST` applied + echoed (SSID/wireless/etc.)
- ❌ Auto-config ("could be huge") — not started
- (n/a) Piso settings (rates, session length) via UI — EllaFi's own admin, not an Omada concern

## Access
- ✅ iPhone ("the key") — node visible in the Omada iOS app; the EllaFi admin is web/iPhone-reachable
- ❌ High-WAF read-only view for Ella — not built

## Notes
- **Client auth is orthogonal to all of the above.** Coin sessions authorize clients via Omada's
  **External Portal** REST (`/api/v2/hotspot/extPortal/auth {clientMac, time}` + client
  `extend`/`disconnect`); coins buy time on an auth call — **no vouchers involved**. Vouchers are a
  separate operator-revenue feature. The device-plane adoption above monitors the **node**; it does
  not touch client auth.
- Mechanism detail: `omada-lab/omada-protocol-reference.md` + the `omada_62_adopt` memory note.
