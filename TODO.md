# EllaFi TODO

## Refactor: split main.cpp
main.cpp is too large (~850 lines). Split along existing comment-header seams:
- `coin.cpp/h` — ISR, timer, coinPulseTask, updateClientTask, finalizeCoinTask, finalizeCoinInsert
- `web.cpp/h` — handleRoot, handleNotFound, handleWsOpen, handleWsRequest, handleWsClose, buildStatusJson, pushCoinUpdateToClient
- `session.cpp/h` — processPause, processResume, pause file helpers (macToFilename, savePausedSessionFile, etc.)
- `main.cpp` — globals, setup, loop, loadConfig, network callbacks
- `globals.h` — extern declarations for shared globals (HOTSPOT_SESSION_CACHE, COIN_INSERT_ACTIVE, COIN_COUNT, etc.)

## Refund code: full implementation
Client-side `showRefundable()` already displays `data.refundCode` but firmware never generates one.
Need to decide on generation strategy (e.g. deterministic from MAC + coin count + timestamp, or random short code)
and store/log it so operator can verify and issue a manual refund.

## Calibrate mock_omada.py DELAYS dict
Current values are estimates. Collect real timing data from serial logs on device:
- Add `ESP_LOGI` timestamps immediately before and after each Omada HTTPS call in omada.cpp
- Run a full coin auth, extend, pause, and resume session against the real cloud controller
- Update `DELAYS` in `test/mock_omada.py` with measured p50/p90 values per call type

## LED: blink at startup, solid when ready
Replace dim-init-color → bright-idle-color with:
- `ledSetup()`: start blinking red ↔ idle color (slow, ~500ms) to indicate booting
- `ledReady()`: stop blinking, go solid idle color
Needs a `LED_NOTIFY_INIT` value on ledTask (or reuse ledTask with a new startup-blink mode)
so the blink happens on the task without blocking setup().
