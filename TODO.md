# EllaFi — TODO / Roadmap

Single source of truth for outstanding work. Design **specs** live in `memory/*_design.md`; the
release **test checklist** is `memory/test_plan.md`; the Omada **API catalog** is
`memory/api_captures/`. This file is the index of *what's left*; those are the *details*.

Legend: 🔴 near-term / pre-launch · 🟡 planned · 🟢 docs/tooling · ✅ done this cycle (bottom).

---

## 🔴 Near-term (next up)

- **🔴 BUG: SSL `-32512` (memory allocation failed) → repeated OUT OF SERVICE.** `refreshHotspotSessionCache()`
  (omada.cpp ~618) holds **three** big JsonDocuments — `hotspotDoc` (63 KB) + `allDoc` (49 KB) + `merged` —
  live for the whole function, then opens **new TLS connections** (`refreshControllerStatus`/`refreshDeviceStatus`,
  lines 701-702) while they're still alive. ArduinoJson docs + mbedTLS both use **internal DRAM** (not the 8 MB
  PSRAM on the N16R8), so the handshake can't get its ~40 KB → `-32512`. Fix: (a) scope the docs so they free
  before the SSL calls, and/or (b) allocate the big docs from **PSRAM** (custom ArduinoJson allocator, scales as
  the client list grows); consider stream+filter parse + one reused TLS conn. Confirmed from serial log 2026-07.
- **🟡 Controller auto-discovery (local HW controller)** — kill the "controller IP moved, config stale" outages.
  mDNS is out (Omada doesn't advertise it). Options: read **DHCP Option 138** (an Omada gateway can serve it)
  via an lwIP custom-option hook, or a **subnet-scan fallback** on `setupOmada` failure — TCP-probe :443 across
  the device's own /24, confirm the hit by requesting `/<controller_id>/api/v2/…`, then rewrite `omada_url`.
  Zero-code stopgap: give the controller a **DHCP reservation** so it stops drifting.

- *Clear — no pre-launch blockers. The crash saga is closed (v1.3.11–13, see Done).*

## 🟡 Resilience / ops

- **Move blocking Omada TLS off the HTTP thread — responsiveness, NOT a crash fix.** The voucher handlers
  (`getVoucherGroupsJson` / `createVoucherGroup` / `getVoucherHistoryJson` / `getVoucherCodesJson`) make
  live Omada calls on the esp_http_server thread; during a controller outage each blocks the handler for
  the full TLS timeout, so the admin page stalls. Route them through a worker task the way `/admin/recheck`
  does. **No longer a corruption risk** — that was the `CREDS_MUTEX` re-creation, fixed in v1.3.11; this is
  now purely a latency/UX improvement.
- **Auto master election + failover/takeover.** Static master = single point of failure. Full design
  (gossip HELLO 300ms, lowest-MAC self-promote after ~2s of no master heartbeat, uptime-weighted) →
  **`memory/master_election_design.md`** (status: not yet implemented — deploy config-master first).
- **Field WiFi re-provisioning** (change SSID/pw without serial when the node falls off the network).
  Full design → **`memory/field_reprovisioning_design.md`** (status: agreed, not yet built). Summary:
  bespoke SoftAP (not BLE); solid-red LED on disconnect → hold internal GPIO0 boot button ~3s →
  SoftAP for a 5-min window; locked-box physical gate, so no PoP (security1/NULL — encrypted channel,
  optional `provisioning_pop`); creds → config.json → reboot. Cheap companion: backup-SSID list in config.

## 🟡 Features

- **AP coverage map (live up/down) on the screen.** Render a small, abstract per-village map (Bakhaw
  first; generalize to Daram/Samar) with a marker per Omada EAP, colored by live up/down status.
  - **Data source:** Omada admin API device/EAP status (we already hold an admin session) — NOT the
    coin-node `nodeMap` heartbeats (that's the 3 coin acceptors, not the EAPs). Need to find the EAP
    device-status endpoint; poll + push to the page over the existing `/ws` + `/status` (live, not cached).
  - **Rendering:** lightweight inline SVG traced from a real map — village outline / key roads, AP
    markers placed from lat/lon normalized into the SVG viewbox. No heavy map lib; only marker colors
    update live. Keep the response small (no whole-file String builds).
  - **Inputs needed from operator:** Google Map screenshot of each village (tracing ref), AP lat/lon,
    and the exact EAP names as they appear in Omada (to bind markers to status).
  - **Open:** which screen — customer-facing captive page vs. admin/ops view? (drives placement + auth)

- **Omada config helper + analyzer.** Help operators set up Omada, and have the firmware
  **inspect the controller config and report/repair** whether it's deployment-ready:
  - detect whether there's a **router** and that **VLAN segmentation** is in place (customers off the
    management VLAN/SSID);
  - detect **controller type/model**;
  - create the needed **hotspot portals** — a **voucher** portal *and* an **External-Portal (vendo)**
    portal — and the **SSIDs** to enforce segmentation;
  - flag/fix anything else needed for a working deployment.
  - Folds in audit **S1** (bind admin to a management VLAN/SSID). Pairs with the existing
    SSID-isolation tripwire (detect) by adding *repair*.
- **Omada client-cache optimization (session-tracking refactor).** Kill the 60s two-call refresh of
  `HOTSPOT_SESSION_CACHE` (hotspot/clients + all-clients); look up `clientId` on-demand at extend/pause
  instead; also dissolves the C2 re-IP problem. **Open question (now testable):** can we drop
  `apMac`/`radioId` from auth? — was blocked by the voucher SSID (`-41501`), but the **EllaFi PisoWiFi**
  External-Portal SSID on Bakhaw is now a clean target. Run `test/auth_field_probe.py` against a
  pending client there.
- **Coin-slot boot-glitch characterization endpoint.** Before touching `COINSLOT_BOOT_SUPPRESS_MILLIS`
  (currently a 500ms guess): admin test mode that cycles Q1 (acceptor power) N times and captures every
  raw pulse the ISR queues for 3s after each power-on — bypassing the suppress/width/interval filters,
  which stay untouched. Output: per-cycle (offset-from-power-on, width). Sets the window from data
  (~2× the latest glitch ever seen). Run with acceptor attached AND unplugged to separate acceptor boot
  noise from board switching noise. Refuse while a coin session is active.
- **Reconsider the fraud abort / bounce handling — ONLY if field data ever shows it.** The board co-sim
  (`kicad/sim/board/`, day-in-the-life bench) showed a *worn* acceptor with bouncing contacts generates
  2-3 sub-width fragments per coin → 3-strike fraud abort kills the paying customer's session. BUT: zero
  reports of this in the wild (JuanFi groups), and the width check has proven field value against pitik
  on the old board — so the check stays as-is. The admin "Rejects" column is the instrument: the bounce
  signature is rejects clustered around accepted coins. If that ever shows up, the analyzed fixes are
  ISR-level merge (gaps <1ms = same pulse, with flicker count as a health metric) or task-level
  forgiveness (don't count sub-width pulses within ~10ms of an accepted coin as fraud); either can be
  proven in `cosim_replay.py` against the existing waveforms before touching coin.cpp.
- **LittleFS browser (admin, read-only).** A "what's actually on the device" panel — would have caught
  the stale-`config.json` confusion instantly, and exposes `sellers.json`/history files we can't see today.
  Flat 6-file set (`config.json`, `sellers.json`, `errors.log`, `refunds.log`, `vendo_history.json`,
  `voucher_history.json`). `GET /admin/fs` (name + size + mtime) and `GET /admin/fs/read?path=` with
  **config.json routed through the existing password-masking** and **capped/streamed reads** (no whole-file
  String). **Read-only** — no delete/edit (config edits stay on the validated `/admin/config` path).

## 🟡 Security — open items from the 2026-06-10 audit

(Most of the audit is done — see bottom. These remain.)
- **Signed OTA (S2).** HMAC now stops a forged OTA *trigger*, but the firmware image itself is still
  flashed **without signature verification**. Sign at release, verify against an embedded key before
  `Update.end()` (master serve + slave pull). High value once OTA is load-bearing.
- **`/fw.bin` is unauthenticated (S5).** Accept (HMAC-gated trigger reduces risk) or gate it.
- **LittleFS multi-task writes (C5) — ✓ resolved.** The v1.3.12/13 soaks flooded `appendErrorLog` from
  httpd/WS/loop tasks for 10+ min with zero corruption; the LittleFS asserts we chased were the `close(0)`
  fd bug (fixed v1.3.12), not concurrency. `esp_littlefs`'s lock holds under load.
- **Coin-acceptor power relay has no single owner** — written from `ledTask`/`ledHalt`/`setup`/
  `handleProgram`. Design note, not a known bug.
- TLS `setInsecure()` on the self-signed controller — **accepted** tradeoff.

## 🔧 Hardware — current board (MPM3620A, in repo at `kicad/`)

*(Rewritten 2026-08-08 — the old rev-B list here described the LM2596 board and was a generation stale.
Most of its items are now DONE and sim-verified on the current design: input fuse F1 + reverse-polarity
protection (D6 = unidirectional 1.5SMC18A), GPIO14/J2.2 verified 3.3V-logic-safe (D4 gate-side clamp,
`tb_panel3wire`), D1 coin-line clamp node confirmed (and a backwards install caught + fixed), J4/J5
22-pin confirmed, LM2596 thermal/cost items obsolete with the MPM3620A. Validation gate:
`kicad/sim/board/run_all.sh` — polarity + sim-sync checkers, 13 benches, Monte Carlo, all green.
Detail → `memory/hardware_notes.md`.)*

Still open before/at ordering:
- **Re-check power-path trace widths + ground stitching vias on the CURRENT layout** (the old finding —
  all tracks 0.2mm, zero stitching vias — was against the LM2596 board; verify it didn't carry over).
- **Bring-up checklist for the first assembled board:** re-run the piezo "pitik" spark test (the new
  opto+TVS front end should stop it cold per `tb_attack_zoom` — if strays still get through, suspect
  the GPIO0 button path or grounding, not the coin line); scope the coin line through 20 Q1 power
  cycles to size `COINSLOT_BOOT_SUPPRESS_MILLIS` (see the characterization-endpoint TODO); scope VIN
  at the MPM3620A pin under load (sim says clean; a scope is the only true proof).
- **3-wire panel compatibility note:** 3.3V drive on J2.2 only marginally switches panels with ~10k
  series inputs (sim: transistor active, not saturated) — measure the actual panel's input before
  relying on it; ~1k inputs are fine.

## 🟢 Docs / tooling

- **README rewrite.** Make it so a stranger/newbie understands what EllaFi is, and can
  **install, flash, configure, and test** it end-to-end. Currently assumes too much.
- **Calibrate `mock_omada.py` DELAYS.** Replace estimates with measured p50/p90 from real device
  serial logs across a full coin auth / extend / pause / resume against the live cloud controller.
  **Tool ready:** `test/calibrate_delays.py` pairs each op's start/finish log lines and prints the
  p50/p90 `DELAYS` block. **Remaining (needs to be local + on the live controller):** `pio device monitor
  | tee live_omada.log` through a coin flow (auth/extend/pause/resume + a couple 60s refreshes), then
  `python3 test/calibrate_delays.py live_omada.log` → paste into `mock_omada.py`. (login/query can't be
  pulled remotely — the bench is on the down mock, so it sits idle.)

## Reference docs (kept separate — referenced above)

- `memory/master_election_design.md` — election / failover spec
- `memory/admin_design.md` — admin page design
- `memory/hardware_notes.md` — PCB / assembly notes (PCBWay)
- `memory/test_plan.md` — v1.3.x validation checklist
- `memory/api_captures/` — Omada API catalog (OpenAPI/Postman + per-endpoint notes)

---

## ✅ Done this cycle (so it isn't re-raised)

HMAC-authenticated mesh + replay/freshness (audit **S3**, kills the **S2** trigger forgery) ·
**C2** MAC-keyed session cache (use-after-free + re-IP) · **C4** UDP socket mutex · **S4** masked-
password brick fix + **C1**/**C3** (v1.3.0) · admin **challenge-response** auth + IP-bound token +
throttle (**S1** core — cleartext Basic-auth gone) · graceful degradation (web-first boot,
`isServiceReady`, out-of-service LED/banner, reconnect overlay) · error-log bounding (read last 200 +
file cap 500) + Clear button · voucher "Used X/Y" column (newest-first) · mock response-code logging ·
SSID-isolation tripwire (detect) · WS terminal-error socket close · flasher "Read device MAC" button ·
merge reads live WiFi IP (pause `IP=null` fix) ·
**node nickname+commission stored on the controller** (each node is a client; `name` = `"nickname|commission"`
via `setClientName` PATCH on save, read back from the all-clients list — survives a `sellers.json` wipe;
replaced the abandoned seller self-heal, voucher names being immutable) · test plan finished ·
**build-47 batch / v1.3.7 release:** 3am reboot → master `localtime` + slaves gated + "Reboot All"
(`NET_MSG_REBOOT`, HMAC'd), dashboard uptime fix, Device Log Clear-button removal + colored W/I/D filters ·
**v1.3.8** 3am reboot cascades to slaves · **v1.3.9** on-demand controller re-check (`/admin/recheck` +
dashboard forces a check on open + banner "Re-check now" link) — *out-of-service banner auto-recover, done* ·
**v1.3.10** Healthchecks.io master dead-man's-switch ping (60s, `healthchecks_url` config + admin Settings
field) · *attempted* `/admin/recheck` crash fix by offloading controller TLS off the httpd thread (turned
out to only narrow the race window — see v1.3.11) · silenced the spurious `sellers.json` VFS `[E]` noise ·
**v1.3.11** **real** fix for the heap-clean "double owner" mutex crash (`xQueueGenericSend queue.c:832`):
`setupOmada()` re-created `CREDS_MUTEX` on every 30s controller-down retry → create-once guard. Verified by
~10h of elimination (ruled out: mbedTLS, BearSSL, error-logging/LittleFS, stack overflow, cross-core PI,
heap OOB) + `test/crash_hammer.py` against a `TEST_MODE` `/diag/hammer` endpoint ·
**v1.3.12** the *second* outage crash — vendored+patched `WiFiClientSecure`: `stop_ssl_socket` memset left
`socket=0`, so a failed-connect double-stop did `close(0)` (= stdin → recycled LittleFS fd 0 → LittleFS
`lfs_mlist_isopen` assert). Caught by instrumenting `wc.fd()` (read 0, heap CLEAN); verified 6 min clean
under the hammer, LittleFS moved off fd 0. (BearSSL replacement attempted + rejected — see above.) ·
**v1.3.13** admin auth as a PsychicHttp **middleware** (`->addMiddleware(adminAuth)` on the 17 protected
routes; `isAuthorized()` unchanged — policy now visible in the route table, verified 401/200 on hardware) ·
node-health UI: offline nodes show `(down Xm)` + dashed RSSI, last-known version kept · dropped the unused
`ESP_SSLClient` dep · boot-order fix (queues created before the web server, closing a NULL-queue crash window) ·
platform pinned to `espressif32@6.5.0` + documented the vendored `WiFiClientSecure` patch.

## ✗ Cancelled

- **BearSSL migration** (`ESP_SSLClient` + `ArduinoHttpClient`, to drop the vendored `WiFiClientSecure`)
  — fully built, then **rejected**: it hung hard under concurrent failed connects (the controller-outage
  case mbedTLS handles fine). Keep the one-line vendored patch + pinned platform. Don't re-attempt without
  first solving the BearSSL/ArduinoHttpClient socket hang.
- **Alt UI** (`index_alt3.html`) — dropped.
- **Seller self-heal** — abandoned (voucher group names are immutable; too problematic). The node
  nickname/commission storage above is the surviving controller-backed-storage feature.
