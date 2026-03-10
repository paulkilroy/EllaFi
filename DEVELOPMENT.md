# Development Guide

## Project Structure

```
EllaFi/
├── data/               # SPIFFS filesystem files
│   ├── index.html     # Captive portal UI (can edit separately!)
│   └── README.md      # Instructions for SPIFFS
├── src/
│   └── main.cpp       # Main ESP32 code
├── platformio.ini     # Build configuration
└── DEVELOPMENT.md     # This file
```

## Quick Start

### 1. Build and Upload Code
```bash
pio run --target upload
```

### 2. Upload Web Files to SPIFFS
```bash
pio run --target uploadfs
```

### 3. Monitor Serial Output
```bash
pio device monitor
```

## Testing the Web Interface

### Option 1: Test on ESP32
1. Upload code and filesystem
2. Connect to "CoinWiFi" network
3. Navigate to http://192.168.4.1

### Option 2: Test in Browser (Development)
1. Navigate to `data/` directory
2. Start local web server:
   ```bash
   python3 -m http.server 8000
   ```
3. Open http://localhost:8000
4. To simulate the portal, add query parameters:
   ```
   http://localhost:8000/index.html?clientMac=AA:BB:CC:DD:EE:FF&ip=192.168.1.100
   ```

### Mock API for Local Testing
You can mock the API endpoints by adding this to the HTML:
```javascript
// Add before checkStatus() function
const MOCK_MODE = true;

function checkStatus() {
  if (MOCK_MODE) {
    updateUI({
      coins: 0,
      sessionActive: false,
      timeLeft: 0,
      accessGranted: false,
      minutes: 0,
      isAuthenticated: false,
      isPaused: false,
      remainingMinutes: 0,
      expirationTime: "Not Available",
      dataUp: "0 MB",
      dataDown: "0 MB",
      redirectUrl: "http://google.com"
    });
    return;
  }
  // ... rest of function
}
```

## Editing the Portal

### To modify the UI:
1. Edit `data/index.html`
2. Test locally in browser (see above)
3. When satisfied, upload to ESP32:
   ```bash
   pio run --target uploadfs
   ```
4. **No need to recompile C++ code!**

### To modify behavior:
- Edit `src/main.cpp`
- Rebuild and upload:
  ```bash
  pio run --target upload
  ```

## API Endpoints

See the header documentation in `src/main.cpp` for complete API docs.

Quick reference:
- `GET  /` - Portal page
- `GET  /status?mac=<MAC>` - Check auth status
- `POST /insert?mac=<MAC>` - Start coin session
- `POST /pause?mac=<MAC>` - Pause session
- `POST /unpause?mac=<MAC>` - Resume session

## Configuration

Edit these constants in `src/main.cpp`:

```cpp
// WiFi AP
const char* ap_ssid = "CoinWiFi";
const char* ap_password = "";

// Omada Controller
const char* CONTROLLER_IP = "192.168.1.100";
const int CONTROLLER_PORT = 8043;
const char* CONTROLLER_ID = "...";
const char* OPERATOR_USERNAME = "portal_operator";
const char* OPERATOR_PASSWORD = "...";

// Coin Settings
const int COIN_PIN = 4;
const int MINUTES_PER_COIN = 5;
const unsigned long COIN_TIMEOUT = 10000; // 10 seconds
```

## Troubleshooting

### SPIFFS Won't Mount
- Make sure `platformio.ini` has `board_build.filesystem = littlefs`
- Try uploading filesystem: `pio run --target uploadfs`
- Check serial monitor for error messages

### HTML Not Loading
- Verify file exists in SPIFFS: Check serial output on boot
- File must be named exactly `/index.html` (case-sensitive)
- Upload filesystem before testing

### Can't Connect to WiFi
- ESP32 creates AP named "CoinWiFi"
- Default IP: 192.168.4.1
- Check serial monitor for startup messages

### API Calls Fail in Browser
- CORS is not an issue when loaded from ESP32
- If testing locally, use same origin or disable CORS
- Check browser console for errors
