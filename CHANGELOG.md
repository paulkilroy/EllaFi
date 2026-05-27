# EllaFi Changelog

## Build 14
- Fixed OTA cascade: `masterSendOtaToNode()` was called inside `networkLoop()` while the UDP socket was still mid-receive; calling `udp.beginPacket()` on the same object mid-receive silently fails on the ESP32 lwIP stack. Deferred the send via `pendingOtaMac` / `networkFlushPending()` — now sent from `loop()` after `networkLoop()` returns and the socket is idle.
- Added `otaSentCount` and `lastOtaSentMs` to `NodeInfo` — `/admin/nodes` now shows how many OTA_AVAILABLE packets master has sent to each slave
- Added `sys.fwSize` and `sys.firmwareBuild` to `/admin/nodes` response
- Added heartbeat TX log — visible on slave serial, confirms slave is alive and what build it broadcasts

## Build 13
- Added diagnostic logs for OTA cascade debugging: heartbeat handler logs `fw_size` and `FIRMWARE_BUILD`, OTA_AVAILABLE handler logs packet source and target MAC comparison
- Added OTA_IN_PROGRESS stuck-state timeout (2 min) with retry — if `xTaskCreate` fails, `OTA_IN_PROGRESS` is cleared immediately

## Build 12
- Incremented build for OTA cascade testing (slave→12, trigger cascade to master 13)

## Build 11
- Confirmed OTA cascade protocol working end-to-end: slave received OTA_AVAILABLE, MAC matched, HTTP fetch started (1167KB)

## Build 9
- Fixed `OTA_IN_PROGRESS` stuck `true` when `xTaskCreate` failed — was set before create; now only set on success, cleared on failure
- Added 2-minute OTA_IN_PROGRESS timeout so a stalled attempt auto-retries on next OTA_AVAILABLE

## Build 8
- `/fs.bin` route now returns 404 when no filesystem image is available — slave OTA silently skips the FS step instead of logging a spurious error
- WiFi "SSID not found" during startup downgraded from error to warning — no longer appears in the errors dashboard or flashes the LED red (transient scan miss on boot)
- Dashboard Errors and Refunds cards are now clickable — navigates to the Logs tab

## Build 7
- Verified OTA cascade end-to-end: slave auto-updates from master on next heartbeat (~15s)

## Build 6
- Fixed OTA cascade: slave MAC truncated in heartbeat due to `toCharArray(buf, sizeof(buf) - 1)` — the last character of the MAC was dropped, causing the master's targetMac to never match the slave's own MAC. Changed to `sizeof(buf)` for both MAC and IP fields in `HeartbeatBody`
- Fixed duplicate slave entry in node health dashboard: master was receiving its own UDP broadcast heartbeat and inserting itself into `nodeMap`. Added early-return when heartbeat MAC matches own MAC
- `/fs.bin` and `/fw.bin` routes registered before `serveStatic` to prevent static fallback from intercepting them

## Build 5
- Serve firmware for slave OTA from OTA flash partition instead of LittleFS (`/fw.bin` → `handleFwBin`, reads via `esp_partition_read`)
- Store `fw_size` in NVS Preferences after firmware OTA so slave cascade trigger survives FS updates
- Slave OTA trigger reads `fw_size` from Preferences (was checking `LittleFS.exists("/fw.bin")`)
- Filesystem OTA preserves runtime files across FS flash: backs up `/config.json`, `/sellers.json`, `/errors.log`, `/refunds.log` to PSRAM before wiping, restores after remount
- Added `[env:esp32-s3-devkitc-1-serial]` PlatformIO environment for USB serial flash without the web OTA hook
- Deleted `data/assets/hawak_mo.mp3` (3.8 MB, unreferenced) — was exceeding the 3.4 MB LittleFS partition

## Build 4
- Introduced `OtaCmdBody { char targetMac[18] }` — slave only acts on OTA_AVAILABLE if targetMac matches its own MAC
- Added `NET_MSG_OTA_AVAILABLE` UDP message: master broadcasts after firmware upload, slave spawns `slaveOtaTask` to fetch `/fw.bin` and `/fs.bin` via HTTP

## Earlier
- Multi-node UDP broadcast architecture (port 4210): master + slave roles, `NET_MSG_COIN_INSERTED`, `NET_MSG_SESSION_START`, `NET_MSG_SESSION_END`, `NET_MSG_HEARTBEAT`
- PlatformIO Upload button hook (`scripts/upload_hook.py`) runs `buildfs` then `scripts/deploy.py`
- Web OTA deploy script (`scripts/deploy.py`): FS first, then firmware, with retry logic and settle delay
