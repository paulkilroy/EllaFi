# Development Guide

## Project Structure

```
EllaFi/
├── data/
│   ├── index.html          # Captive portal UI
│   ├── unidentified.html   # Served when client MAC cannot be identified
│   ├── EllaFi.webp         # Logo
│   ├── config.json         # Credentials — NOT committed (see config.sample.json)
│   └── config.sample.json  # Template — copy to config.json and fill in
├── src/
│   ├── main.cpp            # setup(), loop(), global definitions
│   ├── globals.h           # Shared extern declarations and constexpr constants
│   ├── omada.h / omada.cpp # Omada controller API client
│   ├── coin.h  / coin.cpp  # Coin ISR, timer, pulse task, finalize task
│   ├── web.h   / web.cpp   # HTTP + WebSocket handlers, WS job tasks
│   ├── files.h / files.cpp # Config loading, pause files, error/refund logs
│   ├── network.h / network.cpp # UDP broadcast (multi-node master/slave)
│   ├── led.h   / led.cpp   # RGB LED task
│   └── logger.h            # ESP_LOGE override — auto-appends to /errors.log
├── test/
│   ├── ui_state_test.js        # Browser console: cycle through all UI states
│   ├── authenticated_flow_test.js
│   ├── mock_omada.py           # Local mock of Omada controller API
│   ├── stress_test_happy_path.py
│   ├── stress_test_race_conditions.py
│   ├── stress_test_chaos.py
│   ├── run_all_tests.py
│   ├── omada_clients.py        # CLI tool: list/extend/disconnect hotspot clients
│   └── omada_auth_test.sh      # curl test for extPortal/auth endpoint
├── platformio.ini
└── DEVELOPMENT.md
```

## Quick Start

### 1. Configure credentials
```bash
cp data/config.sample.json data/config.json
# edit data/config.json with WiFi, Omada, and network_key values
```

### 2. Build and upload firmware
```bash
pio run --target upload
```

### 3. Upload filesystem (HTML, images, config)
```bash
pio run --target uploadfs
```

### 4. Monitor serial output
```bash
pio device monitor
```

## Configuration

All configuration lives in `data/config.json` (not committed). Copy from `data/config.sample.json`:

```json
{
  "wifi_ssid": "YourNetwork",
  "wifi_password": "...",
  "omada_url": "https://use1-api-omada-...",
  "omada_controller_id": "...",
  "omada_operator_username": "hotspotadmin",
  "omada_operator_password": "...",
  "omada_username": "admin",
  "omada_password": "...",
  "master_mac": "AA:BB:CC:DD:EE:FF",
  "network_key": 12345678
}
```

- `master_mac` — WiFi MAC of the master node; slaves determined at runtime by comparison
- `network_key` — shared uint64 for UDP broadcast authentication (all nodes must match)
- `omada_site_id` is **not** required — fetched automatically via the admin API at startup

## Modifying the UI

Edit `data/index.html`, test in browser, then upload filesystem:
```bash
pio run --target uploadfs
```
No firmware recompile needed for UI-only changes.

**UI state test:** Open `data/index.html` as a static file in Chrome, paste `test/ui_state_test.js` into the browser console. Use arrow keys to step through all UI states.

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Captive portal page (Omada redirects here) |
| WS | `/ws` | Real-time status and coin insert flow |
| GET | `/refunds` | Refund log (JSON Lines, operator use) |
| GET | `/errors` | Error log (JSON Lines, operator use) |

### WebSocket Protocol

**Client → Server:**
```json
{"action": "getStatus"}
{"action": "startCoinInsert"}
{"action": "pause"}
{"action": "resume"}
{"action": "testInsertCoin"}   // TEST_MODE only
```

**Server → Client:**
```json
{"type": "status", "coinCount": 0, "coinInsertTimeLeft": 0, "sessionStartMillis": 0, "sessionEndMillis": 0, "pausedRemainingMillis": 0, "clientMac": "...", "clientIp": "...", "minutesPerCoin": 5}
{"type": "authenticated"}
{"type": "paused", ...}
{"type": "resumed", ...}
{"type": "error", "subtype": "refundable", "message": "...", "detail": "...", "refundCode": "4X9K2A"}
{"type": "error", "subtype": "no_session", "message": "..."}
```

## Multi-Node Deployment

Three ESP32 nodes (one master, two slaves) connected to three Omada EAP603 APs on the same LAN.

- Single firmware build — role determined at runtime by comparing WiFi MAC to `master_mac` in config
- Master runs the HTTP/WS server and handles all Omada API calls
- Slaves accept coin pulses and forward to master via UDP broadcast (port 4210)
- Master broadcasts session start/end to slaves via UDP

## Log Files

Two log files are maintained on LittleFS, purged of entries older than 24 hours at startup:

- `/errors.log` — all `ESP_LOGE` calls (JSON Lines: `{"ts":...,"tag":"...","msg":"..."}`)
- `/refunds.log` — refundable auth failures (JSON Lines: `{"ts":...,"mac":"...","coins":...,"minutes":...,"code":"..."}`)

Access at `http://<device-ip>/errors` and `http://<device-ip>/refunds` (master only, LAN only).

## Troubleshooting

### LittleFS won't mount
- Check `platformio.ini` has `board_build.filesystem = littlefs`
- Upload filesystem: `pio run --target uploadfs`
- Fast red LED blink = unrecoverable startup failure; check serial for last error

### Config errors
- Fast red LED blink at startup = config missing or invalid
- Check serial output for which field is missing
- Verify `data/config.json` exists and was included in filesystem upload

### Omada API failures
- Check `/errors` endpoint for recent error log
- Verify `omada_url` does not include trailing `:443`
- Admin credentials (`omada_username`/`omada_password`) are required for site discovery and WiFi client lookup

### Running tests locally
```bash
# Start mock Omada server
python3 test/mock_omada.py

# Run all stress tests against mock
python3 test/run_all_tests.py
```
