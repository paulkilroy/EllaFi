# EllaFi

![EllaFi](data/EllaFi.webp)

Coin-operated WiFi hotspot portal for ESP32, built for TP-Link Omada controller networks. Clients insert physical coins to purchase internet access — the ESP32 handles coin counting, session management, pause/resume, and communicates with the Omada controller to grant and revoke access.

## Flash & Provision

**[→ Open the EllaFi Provisioner](https://paulkilroy.github.io/EllaFi/flasher.html)** — browser-based tool (Chrome/Edge) to flash firmware and configure a new node over USB. No Python or esptool install required.

---

## Features

- **Coin-operated access** — physical coin acceptor triggers ISR; 10s window accumulates coins before finalizing
- **Real-time UI** — captive portal page communicates via WebSocket; session timer, progress bar, and coin insert countdown update live
- **Pause & resume** — clients can pause their session and resume later; remaining time is saved to flash
- **Multi-node support** — master/slave architecture over UDP broadcast; slave nodes accept coins and report to the master via LAN
- **Session reconstruction** — on reboot, the ESP32 queries the Omada controller to restore active sessions without requiring re-authentication
- **Error handling** — structured error subtypes (`no_session`, `refundable`) with permanent UI popups for unrecoverable states
- **Captive portal** — integrates with Omada's External Portal Server auth flow; works with EAP603 and similar APs

---

## Hardware

- ESP32 (tested on ESP32-S3 DevKitC-1)
- Coin acceptor wired to GPIO4 via PC817 optocoupler (GPIO0 / BOOT button also works for testing)
- TP-Link Omada controller with hotspot operator account
- Omada EAP access points on the same LAN

### Coin Acceptor Wiring

Coin acceptor pinout reference: https://www.cable-tester.com/coin-acceptor-connector-pin-out/

```
                    P6KE6.8A (TVS)
Coin acceptor SIG ──┬── (across signal to GND) ── GND
                    │
                    └── PC817 PIN2 (Cathode)

3.3V ── 220Ω ─────── PC817 PIN1 (Anode)
                     PC817 PIN3 (Emitter) ──── GND
                     PC817 PIN4 (Collector) ── 10kΩ ──── GPIO4

Coin acceptor GND ──────────────────────────────────── GND (shared)
```

- **PC817** — optocoupler for galvanic isolation between coin acceptor and 3.3V ESP32
- **220Ω** (input side, R2) — limits PC817 LED current from 3.3V; (3.3V − 1.2V Vf) / 220Ω ≈ 9.5mA
- **10kΩ** (output side, R1) — series resistor between PC817 collector and GPIO4; GPIO4 uses ESP32 internal pull-up (INPUT_PULLUP). When PC817 conducts, GPIO4 is pulled low through R1; when off, internal pull-up holds GPIO4 high
- **P6KE6.8A TVS diode** — clamps piezo lighter spikes (10-20kV) to GND before they reach the PC817; bidirectional, wired across signal line to GND; rated 600W peak pulse, effectively unlimited lifetime at 1 spike/second. With 3.3V drive, the signal line idles at ~2.1V — well below the 6.8V clamp — so TVS only fires on actual spikes, not at idle
- **Anti-pitik (software)** — ISR triggers on CHANGE (both edges) on both GPIO4 (coin slot) and GPIO0 (boot button); measures pulse width, rejects anything under 5ms. Piezo spikes attenuated by PC817 to ~5µs on GPIO4, and couple electromagnetically into GPIO0 — both paths filtered by the same threshold

### PCB Wiring (Full Circuit)

Within each net, order doesn't matter — wire items in whatever sequence is convenient for your layout.

**+12V net**
- J3 pin 1 (barrel jack +)
- C1 pin 1 (input cap +)
- U3 pin 1 (LM2596 VIN)
- J1 pin 1 (coin acceptor power)
- J2 pin 1 (LED ring + power)

**GND net**
- J3 pin 2 (barrel jack GND)
- C1 pin 2 (input cap −)
- U3 pin 3 (LM2596 GND)
- U3 pin 5 (LM2596 ON/OFF — tie to GND = always on)
- D2 pin 2 (freewheeling diode anode)
- C2 pin 2 (output cap −)
- Q1 pin 3 (MOSFET source)
- U1 pin 3 (PC817 emitter)
- U2 pins 22, 23, 24, 44 (ESP32 GND)
- D1 pin 1 (TVS anode — system GND directly, not switched)

** More documentation on the LM2596 here: https://www.ti.com/lit/ds/symlink/lm2596.pdf
**Switching node** (short wires — keep these close together)
- U3 pin 2 (LM2596 OUT)
- D2 pin 1 (freewheeling diode cathode)
- L1 pin 1

**+5V net**
- L1 pin 2 (inductor output)
- C2 pin 1 (output cap +)
- U3 pin 4 (LM2596 FB)
- U2 pin 21 (ESP32 5V)

**+3.3V net**
- U2 pin 2 (ESP32 3V3 output)
- R2 pin 1 (PC817 LED current limiter, 220Ω)

**Switched GND node** — controlled by Q1 (MOSFET); HIGH on GPIO14 = coin slot on
- Q1 pin 2 (MOSFET drain)
- J1 pin 3 (coin acceptor GND)
- J2 pin 2 (activate signal — hardwired to pin 3; always on when switched power is on)
- J2 pin 3 (LED ring GND −)

J2 is a 3-pin screw terminal (2.54mm or 5.08mm pitch). 2-wire LED rings: use pins 1 and 3. 3-wire LED rings (with activate signal): use all 3 pins — pins 2 and 3 are both on switched GND, so activate is always asserted.

**Coin signal node**
- J1 pin 2 (coin acceptor open-collector output — LOW = coin pulse)
- U1 pin 2 (PC817 LED cathode)
- D1 pin 2 (TVS cathode — clamps spikes; orient banded end toward this node)

**R2 → PC817 LED**
- R2 pin 2
- U1 pin 1 (PC817 LED anode)

**PC817 output → ESP32 coin pulse (GPIO4)** — series path, not a single net
- U1 pin 4 (PC817 collector) → R1 (10kΩ) → U2 pin 4 (GPIO4, COINSLOT_PIN)
- R1 limits collector current; GPIO4 pulled high by ESP32 internal pull-up

**Q1 gate — coin slot enable (GPIO14)**
- Q1 pin 1 (MOSFET gate)
- U2 pin 20 (GPIO14, COINSLOT_POWER_PIN)

J1 pin 4 — NC (leave unconnected)

---

## Bill of Materials (DigiKey)

| Ref          | Description                                          | DigiKey #              | Qty | Confidence | Verify |
|--------------|------------------------------------------------------|------------------------|-----|------------|--------|
| R1           | 10kΩ 0.25W axial THT (DIN0207)                      | `MF0207FRE52-10K-ND`   | 1   | 90% | DigiKey confirmed body 6.3mm ✓, dia 2.4mm (spec 2.5mm) — check fits through pads; lead pitch not confirmed |
| R2, R4, R5   | 220Ω 0.25W axial THT                                | `220QBK-ND`            | 3   | 90% | Same series as R1 — same caveats |
| R3           | 330Ω 0.25W axial THT                                | `330QBK-ND`            | 1   | 90% | Same series as R1 — same caveats |
| C1, C2       | 100uF 25V radial electrolytic 8mm/3.5mm             | `493-14973-ND`         | 2   | 99% | DigiKey confirmed 8.0mm dia ✓, 3.5mm pitch ✓ |
| D1           | P6KE6.8A TVS diode DO-15                            | `P6KE6.8ALFCT-ND`      | 1   | 90% | Footprint needs P10.16mm lead pitch — confirm on DigiKey "Lead Spacing" spec |
| D2           | 1N5822 Schottky diode DO-201AD                      | `1N5822-E3/54GICT-ND`  | 1   | 90% | Footprint needs P15.24mm lead pitch — confirm on DigiKey "Lead Spacing" spec |
| D3           | 3mm right-angle LED red                             | `754-1298-ND`          | 1   | 90% | Footprint is `O1.27mm_Z2.0mm` — confirm horizontal offset and height on DigiKey dimensional drawing |
| D4           | 3mm right-angle LED yellow                          | `754-1300-ND`          | 1   | 90% | Same series as D3 — same caveats |
| D5           | 3mm right-angle LED green                           | `754-1297-ND`          | 1   | 90% | Same series as D3 — same caveats |
| Q1           | IRF540N N-channel MOSFET TO-220-3                   | `IRF540NPBF-ND`        | 1   | 90% | Footprint is vertical TO-220-3 — confirm part is vertical mount not tab-down horizontal |
| SW1          | OS102011MS2QN1 SPDT slide switch                    | `CKN9565-ND`           | 1   | 99% | Exact C&K part number — KiCad footprint named after this exact part |
| U1           | FOD817 optocoupler DIP-4 (PC817 compatible)         | `FOD817-ND`            | 1   | 95% | Standard DIP-4 7.62mm row spacing — confirm FOD817 matches PC817 pin layout |
| U3           | LM2596T-5.0 buck regulator TO-220-5                 | `LM2596T-5.0/NOPB-ND`  | 1   | 90% | TO-220-5 stagger confirmed; 1.70mm lead pitch × 2 = 3.4mm span matches `P3.4x3.7mm_StaggerEven` ✓ |
| L1           | 100uH radial inductor 9.5mm dia / 5mm pitch         | `732-3081-ND`          | 1   | 90% | DigiKey confirmed 9.5mm dia ✓; 5mm pitch from Würth series spec, not directly on DigiKey page |
| J1           | 1×4 female socket 2.54mm                           | `S7037-ND`             | 1   | 95% | Standard 2.54mm pitch — Sullins exact part, KiCad footprint named to spec |
| J2           | 3-pos terminal block 5.08mm pitch                  | `277-1976-ND`          | 1   | 95% | Phoenix Contact exact part — 5.08mm pitch confirmed |
| J3           | CUI PJ-002AH barrel jack 2.1mm ID / 5.5mm OD       | `CP-002AH-ND`          | 1   | 75% | Center-to-sleeve 3.0mm confirmed ✓; mounting tab offset ~3.0mm but KiCad footprint uses 3.5mm — tab holes may not align, verify before routing |
| J4, J5       | 1×22 female socket 2.54mm (ESP32 DevKitC-1)        | `S7055-ND`             | 2   | 95% | Standard 2.54mm pitch — Sullins exact part |

---

## How It Works

1. Client connects to WiFi → Omada redirects to captive portal
2. Portal page opens via WebSocket; ESP32 pushes session status
3. Client clicks **Insert Coins** → coin insert mode activates
4. Physical coins trigger GPIO pulses (debounced ISR)
5. After 10s of inactivity, ESP32 calls Omada API to authenticate or extend the session
6. Client is redirected to the internet; session timer counts down live
7. Client can **pause** (disconnects from Omada, saves remaining time) and **resume** (re-authenticates with saved duration)

---

## Configuration

Copy `data/config.sample.json` to `data/config.json` and fill in:

```json
{
  "wifi_ssid": "YourWiFiNetwork",          // SSID of the WiFi network the ESP32 joins
  "wifi_password": "...",                  // WiFi password
  "omada_url": "https://...",              // Omada controller URL (Settings → Controller → URL)
  "omada_controller_id": "...",            // Omada controller ID (visible in the controller URL path)
  "omada_operator_username": "...",        // Hotspot operator account (Hotspot → Operator Accounts)
  "omada_operator_password": "...",        //   ↑ same account
  "omada_username": "...",                 // Omada admin account (Settings → Administrators)
  "omada_password": "...",                 //   ↑ same account
  "master_mac": "AA:BB:CC:DD:EE:FF",      // WiFi MAC of the master ESP32 node (printed on boot)
  "network_key": 12345678                  // Shared secret for UDP broadcast between nodes (any number)
}
```

`data/config.json` is gitignored and never committed.

---

## Build & Flash

**Easiest:** use the [browser-based provisioner](https://paulkilroy.github.io/EllaFi/flasher.html) — no tools to install.

**From source** — requires [PlatformIO](https://platformio.org/):

```bash
# Build and flash firmware
pio run --target upload

# Upload web files (captive portal HTML) to LittleFS
pio run --target uploadfs

# Monitor serial output
pio device monitor
```

Tag a release to trigger a GitHub Actions build and publish firmware binaries:

```bash
git tag v1.0.0 && git push --tags
```

---

## Testing

See [test/TESTING.md](test/TESTING.md) for full details.

```bash
# Run all automated tests against a live ESP32
python3 test/run_all_tests.py --host <ESP32_IP>

# UI state walkthrough (paste into browser console on data/index.html)
# Use ← → arrow keys to step through all UI states
test/ui_state_test.js
```

---

## Project Structure

```
src/
  main.cpp        # Firmware: captive portal, coin handling, WebSocket, session logic
  omada.cpp/.h    # Omada controller API client (auth, extend, disconnect, session cache)
  network.cpp/.h  # UDP broadcast, master/slave role detection
data/
  index.html      # Captive portal UI (single-file, no dependencies)
  config.sample.json
test/
  mock_omada.py             # Local Omada controller mock for testing
  stress_test_happy_path.py # Full coin insert → auth flow
  stress_test_race_conditions.py
  stress_test_chaos.py
  ui_state_test.js          # Browser console UI walkthrough
```
