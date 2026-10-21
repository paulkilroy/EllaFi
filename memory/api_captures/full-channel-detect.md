# Full Channel Interference Detection (per-AP neighbor/contention list)

Source: HAR capture omada-html/interference_detection.har (2026-10-22), verified live.

## Read a detection result (per AP, per radio)
GET openapi/v1/{cid}/sites/{site}/aps/{mac}/full-channel-detect/wlan-interference
    ?page=1&pageSize=100&filters.radioId=0        # radioId 0 = 2.4 GHz, 1 = 5 GHz
-> result.data[]: {bssid, ssid, radioId, channel, mode, security, beacon (ms),
   signal (dBm), lastSeen, bandWidth}
GOTCHA: errorCode -39378 = "no detection data" for that AP. Data exists only where the
full-channel detection has RUN — the controller runs it on AUTO-channel radios; fixed-channel
APs (the schools since 2026-10-21) return -39378 until a scan is triggered for them.

## Related (openapi/v1, same {cid}/sites/{site} prefix — from the endpoint directory)
- full-channel-detect-start           # TRIGGER site-wide scan (radios go off-channel ~1 min —
                                      # service blip; run during the 02:00-06:00 blackout)
- full-channel-detect/status, /batch/info
- full-channel-detect/{historyId} + /ap-list + /channel-load + /grid/interference
- files/sites/{site}/aps/{mac}/full-channel-detect/export

Signal %: the UI's % column is derived from dBm (-64 ≈ 100% … -102 ≈ 0%).
