# EllaFi — TODO / Roadmap

Single source of truth for outstanding work. Design **specs** live in `memory/*_design.md`; the
release **test checklist** is `memory/test_plan.md`; the Omada **API catalog** is
`memory/api_captures/`. This file is the index of *what's left*; those are the *details*.

Legend: 🔴 near-term / pre-launch · 🟡 planned · 🟢 docs/tooling · ✅ done this cycle (bottom).

---

## 🔴 Near-term (next up)

- *(Clear — v1.3.11 fixed the last known crash. Root cause: `setupOmada()` doubles as the 30s
  controller-down retry in `loop()` and re-created `CREDS_MUTEX` on every call; a task holding the old
  semaphore while the global was swapped to a fresh one let a second task into the critical section →
  `assert failed: xQueueGenericSend queue.c:832`. The v1.3.10 "offload TLS off the httpd thread" was a
  window-narrowing **mask**, not the cause.)*

## 🟡 Resilience / ops

- **Migrate outbound HTTPS off Arduino `WiFiClientSecure` → `ESP_SSLClient` (BearSSL) + `ArduinoHttpClient` (Route A).**
  v1.3.12 ships a *vendored, patched* copy of `WiFiClientSecure` to fix the `close(0)` fd bug (`ssl_client.cpp`
  `stop_ssl_socket` memsets `socket` to 0, so a failed-connect double-stop does `close(0)` → frees stdin →
  LittleFS recycles fd 0 → next `close(0)` corrupts LittleFS). Real fix = stop using that client. Move the 5
  callers — Omada (`omada.cpp`, ~10 fns), Healthchecks ping (`main.cpp`), GitHub OTA check+download (`web.cpp`,
  **the download needs manual 302-redirect follow**) — onto `ESP_SSLClient`+`ArduinoHttpClient`; slave→master
  `/fw.bin` is plain HTTP (no TLS) and can stay. Then **delete `lib/WiFiClientSecure`** and drop the mbedTLS dup.
  Cookies/CSRF stay hand-built (already are). Needs **live-controller** testing (auth/extend/disconnect/vouchers)
  before release. `setBufferSizes()` must fit Omada's largest JSON (full client list).
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
  Use Espressif `WiFiProv` (BLE + SoftAP), armed on sustained join-failure, gated. Apps: "ESP BLE
  Provisioning" / "ESP SoftAP Provisioning". Cheap companion: backup-SSID list in config.

## 🟡 Features

- **Omada config helper + analyzer** (NEW). Help operators set up Omada, and have the firmware
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
- **LittleFS multi-task writes (C5).** Relies on `esp_littlefs`'s internal lock — confirm under load.
- **Coin-acceptor power relay has no single owner** — written from `ledTask`/`ledHalt`/`setup`/
  `handleProgram`. Design note, not a known bug.
- TLS `setInsecure()` on the self-signed controller — **accepted** tradeoff.

## 🔧 Hardware — next board revision (rev B)

From the PCBWay BOM quote (Product No. **T-3P14W1063745A**, 6 units @ $211.34). The current order is
fine to ship **as-is**; these apply to the next spin. Detail → `memory/hardware_notes.md`.

Now **verified against the KiCad netlist + PCB** (`~/Documents/EllaFi-PCB/`); full review in `memory/hardware_notes.md`.
The schematic checks out (gate pulldown, LM2596 ON/OFF, opto level, TVS placement all correct). Rev-B items:

- **Widen the power-path traces.** *All 146 tracks are 0.2 mm (8 mil)* — including +12V, 5V, and the
  switch node — but it's a 3A supply. Size +12V/5V/switch node to ≥0.5 mm (≥1.5 mm for full 3A).
- **Stitch the ground planes.** GND pours on both layers but **0 vias** — add ground-stitching vias,
  especially around U3 (switcher).
- **Protect GPIO14 on J2.2 (3-wire mode).** It exposes the bare GPIO (10k pulldown only) to the panel's
  enable input; a 12V-referenced panel back-drives 12V into GPIO14 → **kills the ESP**. Verify the panel
  enable is 3.3V-logic-safe, else add series R + 3V3 clamp (or buffer it).
- **Add input protection.** No **fuse** and no **reverse-polarity protection** on J2/J3 — a reversed jack
  or a field-power fault has nothing to stop it. Add a fuse holder + series Schottky or P-FET ideal-diode.
- **U3 thermal:** vertical TO-220 on 2-layer — needs a clip-on heatsink above ~1.5A. Measure full-load
  trace/U3 temps first. Verify **L1 Isat ≥ ~3.5A**; consider **C2 220µF** (vs 100µF) per the datasheet.
- **Value-engineer the cost outliers** (the 5V supply is ~$18 of the ~$30/board component cost). With
  no design change, functional-equivalent swaps cut ~$40 across 6 boards:
  - **C1,C2 100µF 25V** — $2.06/ea is ~6–10× normal; generic low-ESR ≈ $0.30 (~$20 saved). *Worst offender.*
  - **J2 Phoenix 1715857** — premium; generic 5.08mm 3-pos block ≈ $0.50–1 (~$15 saved).
  - **L1 Würth 100µH** — generic 100µH ≥3A shielded ≈ $1–1.50 (~$10 saved).
  - **U3 LM2596T-5.0** — PCBWay's $10.25 is ~2× typical; ask them to re-source (or revisit a buck module).
- **Verify (could be wrong as drawn):**
  - **C2 output cap = 100µF** — LM2596 5V datasheet wants ~220µF low-ESR for clean ripple/stability at load.
  - **J4/J5 = 1×22/side** — confirm the actual ESP32-S3-DevKitC-1 is 22 pins/side (some S3 devkits are 20);
    and confirm 6 **customer-supplied modules** are on hand (U2 is DNP, not in the quote).
  - **D1 P6KE6.8A** — 6.8V standoff is marginal directly across a 5V rail; confirm which node it clamps.

## 🟢 Docs / tooling

- **README rewrite (NEW).** Make it so a stranger/newbie understands what EllaFi is, and can
  **install, flash, configure, and test** it end-to-end. Currently assumes too much.
- **Calibrate `mock_omada.py` DELAYS.** Replace estimates with measured p50/p90 from real device
  serial logs across a full coin auth / extend / pause / resume against the live cloud controller.

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
under the hammer, LittleFS moved off fd 0. Real fix is the BearSSL migration above (drops this vendored lib).

## ✗ Cancelled

- **Alt UI** (`index_alt3.html`) — dropped.
- **Seller self-heal** — abandoned (voucher group names are immutable; too problematic). The node
  nickname/commission storage above is the surviving controller-backed-storage feature.
