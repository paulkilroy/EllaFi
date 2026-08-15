# omada-html — POC for the Omada-hosted portal

Diagnostic test page for the "option B" architecture (controller serves the portal, ESP32 becomes a
thin coin API). Answers the three open questions from `memory`/TODO with a real phone in the real
captive browser:

1. **Q1 cross-origin** — can the captive mini-browser reach the ESP at all, and read responses?
2. **Q2 mixed content** — if the controller serves the page over HTTPS, are http:// ESP calls blocked?
3. **Q3 grant model** — does a voucher submitted from a *custom* page authorize natively?

Pure diagnostics, zero styling effort. Everything is logged on-screen with timestamps because
captive browsers have no devtools.

## Test battery (runs automatically on load)

| # | What it does | What it proves |
|---|---|---|
| T1 | same-origin `POST /portal/getPortalPageSetting` | page is really being served by the controller (falls back to the bundled mock when previewed locally) |
| T2 | `<img>` from `http://<esp>/EllaFi.webp` | pure reachability — immune to CORS, only mixed-content/network can block it |
| T3 | `fetch` `mode:'no-cors'` → `/status` | network round-trip works (response opaque by design) |
| T4 | normal `fetch` → `/status` | response *readable* — passes only if the ESP sends `Access-Control-Allow-Origin` (it doesn't today; expected fail/warn) |
| T5 | `WebSocket` → `ws://<esp>/ws` | **the key test** — WS is exempt from CORS; if it opens, the page has a full two-way ESP channel with zero firmware changes |
| T6 | `fetch` → `https://<esp>/status` | documents the mixed-content dead end (expected fail — no TLS on ESP) |
| T7 | 5 × sequential no-cors `/status` | latency of option B's 1/s poll loop |

The verdict panel maps raw results onto Q1–Q3 in plain language. The voucher test (section 6) is
manual and consumes a real voucher code — it POSTs same-origin `/portal/auth` with the exact body
the stock Omada page sends (extracted from TP-Link's `demo.zip` sample).

## How to run it

**Local preview (sanity check only):** open `index.html` in a desktop browser. T1 uses the bundled
`getPortalPageSetting.json` mock; ESP tests run for real if you're on the same LAN.

**The real test:**
1. `./make_zip.sh` → `ellafi-poc.zip`
2. Controller UI → Settings → Authentication → Portal → Portal Customization → **Import Customized
   Page** → upload the zip (temporarily, on the test SSID/site — this replaces the live portal!)
3. Join the SSID with a real phone, let the captive screen pop, read the verdict panel.
4. Repeat on iPhone + Android, and once more with "Open in browser" vs the captive sheet — CNA
   behavior differs per OS.
5. Photograph/copy the raw log for anything unexpected.

Restore the previous portal page after testing.

## ESP endpoints used

Existing, unauthenticated, no firmware changes: `GET /status`, `GET /EllaFi.webp`, `WS /ws`.
New endpoints for the real coin flow are deliberately **not** designed until this POC answers
Q1/Q2 — see the discussion in the session notes / TODO.
