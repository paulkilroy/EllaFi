# EllaFi TODO

## Calibrate mock_omada.py DELAYS dict
Current values are estimates. Collect real timing data from serial logs on device:
- Run a full coin auth, extend, pause, and resume session against the real cloud controller
- Update `DELAYS` in `test/mock_omada.py` with measured p50/p90 values per call type

## TEST_MODE should be a build flag
`#define TEST_MODE true` is hardcoded in `omada.h`. Should be a PlatformIO build flag in
`platformio.ini` so release builds exclude test paths without a manual header edit.

## TLS certificate validation
All Omada HTTPS calls use `wifiClient.setInsecure()`. For a device handling financial
transactions, certificate pinning is worth adding once the controller cert is stable.

## Alt UI (index_alt3.html)
User liked the alt3 design. Work in progress — not yet production-ready.
