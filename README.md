# EllaFi

![EllaFi](data/EllaFi.png)

Coin-operated WiFi hotspot portal for ESP32, built for TP-Link Omada controller networks. Clients insert physical coins to purchase internet access — the ESP32 handles coin counting, session management, pause/resume, and communicates with the Omada controller to grant and revoke access.

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
- Coin acceptor wired to GPIO5 via PC817 optocoupler (GPIO0 / BOOT button also works for testing)
- TP-Link Omada controller with hotspot operator account
- Omada EAP access points on the same LAN

### Coin Acceptor Wiring

```
                    P6KE6.8A
Coin acceptor SIG ──┬── (Cathode up, Anode to GND) ── GND
                    │
                    └── PC817 PIN2 (Cathode)

3.3V ─────────────── PC817 PIN1 (Anode)
                     PC817 PIN3 (Emitter) ──── GND
                     PC817 PIN4 (Collector) ── 10kΩ ──── GPIO4

Coin acceptor GND ──────────────────────────────────── GND (shared)
```

- **PC817** — optocoupler for galvanic isolation between coin acceptor and 3.3V ESP32
- **10kΩ** (output side) — current limiting on collector; GPIO4 uses internal pull-up
- **P6KE6.8A TVS diode** — clamps piezo lighter spikes (10-20kV) to GND before they reach the PC817; cathode to SIG line, anode to GND; rated 600W peak pulse, effectively unlimited lifetime at 1 spike/second
- **Anti-pitik (software)** — ISR triggers on CHANGE (both edges) on both GPIO4 (coin slot) and GPIO0 (boot button); measures pulse width, rejects anything under 5ms. Piezo spikes attenuated by PC817 to ~5µs on GPIO4, and couple electromagnetically into GPIO0 — both paths filtered by the same threshold

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

Requires [PlatformIO](https://platformio.org/).

```bash
# Build and flash firmware
pio run --target upload

# Upload web files (captive portal HTML) to LittleFS
pio run --target uploadfs

# Monitor serial output
pio device monitor
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
