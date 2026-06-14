# EllaFi — TODO / Roadmap

Single source of truth for outstanding work. Design **specs** live in `memory/*_design.md`; the
release **test checklist** is `memory/test_plan.md`; the Omada **API catalog** is
`memory/api_captures/`. This file is the index of *what's left*; those are the *details*.

Legend: 🔴 near-term / pre-launch · 🟡 planned · 🟢 docs/tooling · ✅ done this cycle (bottom).

---

## 🔴 Near-term / pre-launch

- **3am reboot fix → ship it.** Fixed (uses `localtime`, was firing 3am UTC = 11am PHT). Uncommitted.
  Commit → tag `v1.3.7` → OTA to the field nodes (they're still rebooting at peak until updated).
- **Confirm/create an External-Portal SSID for the vendos.** The prod voucher SSID rejects the
  firmware's `authType:4` auth (`-41501`) — coin machines can't authorize anyone without an
  External-Portal-typed portal. (The Omada-analyzer feature below would auto-detect/fix this.)
- **Finish the test plan** → `memory/test_plan.md`: no-internet boot + controller-dies-mid-run
  recovery; re-IP re-point / hour soak / dashboard counts; seller self-heal; voucher-cap error.
- ~~Place PCB order~~ — DONE (6 boards, PCBWay turnkey).

## 🟡 Resilience / ops

- **Auto master election + failover/takeover.** Static master = single point of failure. Full design
  (gossip HELLO 300ms, lowest-MAC self-promote after ~2s of no master heartbeat, uptime-weighted) →
  **`memory/master_election_design.md`** (status: not yet implemented — deploy config-master first).
- **Healthchecks.io ping (master dead-man's switch).** Master POSTs `healthchecks_url` every 60s.
  - Add `healthchecks_url` to `config.sample.json` (empty = disabled); load in `files.cpp`.
  - `HEALTHCHECK_TIMER` 60s repeating, created in master startup.
  - Payload `{"mac":...,"uptime_s":...,"active_sessions":...}`; dashboard Period=1min, Grace=5min.
  - Independent of the election refactor.
- **Field WiFi re-provisioning** (change SSID/pw without serial when the node falls off the network).
  Use Espressif `WiFiProv` (BLE + SoftAP), armed on sustained join-failure, gated. Apps: "ESP BLE
  Provisioning" / "ESP SoftAP Provisioning". Cheap companion: backup-SSID list in config.
- **Out-of-service banner: auto-recover the admin view.** Banner is passive (only updates on reload).
  Poll `/status` every ~10s while it's showing and clear it when `serviceReady` flips; + a Retry link.

## 🟡 Features

- **Omada config helper + analyzer** (NEW). Help operators set up Omada, and have the firmware
  **inspect the controller config and report/repair** whether it's deployment-ready:
  - detect whether there's a **router** and that **VLAN segmentation** is in place (customers off the
    management VLAN/SSID);
  - detect **controller type/model**;
  - create the needed **hotspot portals** — a **voucher** portal *and* an **External-Portal (vendo)**
    portal — and the **SSIDs** to enforce segmentation;
  - flag/fix anything else needed for a working deployment.
  - Folds in audit **S1** (bind admin to a management VLAN/SSID) and resolves the External-Portal-SSID
    deployment flag above. Pairs with the existing SSID-isolation tripwire (detect) by adding *repair*.
- **Web config editor.** Edit `config.json` (rates, creds, network) from the browser, no reflash.
  Hard part: must authenticate securely on a **public/customer WiFi** — needs a real auth story.
- **Analytics dashboard.** Payments by day — bar of unique clients + line of pesos received.
  Write to a file (crash-safe re-read); background worker to keep writes off the main thread;
  **public, no-auth** page. Find a nice, simple JS charting lib.
- **Session-tracking refactor.** Kill the 60s two-call refresh; look up `clientId` on-demand at
  extend/pause; dissolves the C2 re-IP problem. **Open question:** can we drop `apMac`/`radioId`
  from auth? — untestable on the voucher SSID (`-41501`); needs a pending client on an
  External-Portal SSID. See `test/auth_field_probe.py`.
- **Alt UI** (`index_alt3.html`) — the alt3 design you liked, WIP, not production-ready.

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
file cap 500) + Clear button · voucher "Used X/Y" column (newest-first) · seller self-heal · mock
response-code logging · SSID-isolation tripwire (detect) · WS terminal-error socket close · flasher
"Read device MAC" button · merge reads live WiFi IP (pause `IP=null` fix) · v1.3.6/build-46 bump.
