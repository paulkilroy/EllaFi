# SPIFFS Data Files

This directory contains files that need to be uploaded to the ESP32's SPIFFS filesystem.

## Files

- **index.html** - The captive portal web interface

## Uploading to ESP32

### Using PlatformIO

1. Make sure your `platformio.ini` includes:
   ```ini
   board_build.filesystem = littlefs
   ```

2. Upload the filesystem:
   ```bash
   pio run --target uploadfs
   ```

### Using Arduino IDE

1. Install the **ESP32 Sketch Data Upload** tool
2. Place files in the `data` folder in your sketch directory
3. Tools → ESP32 Sketch Data Upload

## Testing the HTML Locally

You can open `index.html` directly in a browser to test the UI, but you'll need to:

1. Comment out or mock the API calls to `/status`, `/insert`, `/pause`, `/unpause`
2. Provide test data for the status endpoint
3. Use a local web server to avoid CORS issues:
   ```bash
   cd data
   python3 -m http.server 8000
   ```
   Then open http://localhost:8000

## Modifying the Portal

To update the captive portal:

1. Edit `data/index.html`
2. Test your changes locally
3. Re-upload to ESP32 using `pio run --target uploadfs`
4. No need to recompile the C++ code!

## Dynamic Configuration

The C++ code dynamically replaces this line in the JavaScript:
```javascript
const MINUTES_PER_COIN = 5;
```

With the actual value from the `MINUTES_PER_COIN` constant in `main.cpp`.
