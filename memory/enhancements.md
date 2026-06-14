# Enhancements / Future Work

Tracked ideas. Newest at top.

---

## Simplify session tracking: kill the 60s refresh loop, go on-demand (+ open field-cut question)

**Goal:** drop `refreshHotspotSessionCache` (the 60s two-call merge of `hotspot/clients` +
`sites/clients`) and the IP-keyed Omada-state cache. Fetch what we need at the moment we act.

**Why it's possible:** the only field the loop exists to maintain is **`clientId`** (needed by
extend + disconnect). Everything else is fresh from the **portal redirect** (`handleRoot`:
clientMac/apMac/ssidName/radioId/ip) or on **flash** (`pausedRemaining`). The customer countdown is
just `auth_time + duration_bought`, computable locally. So `clientId` can be looked up on demand.

**Conservative refactor (do this — doesn't depend on the open question below):**
- `lookupOmadaSession(mac)` → one `hotspot/clients` GET, row by MAC → `{clientId, end}`.
  - **finalize/extend:** lookup → active? `extendOmadaClient(clientId)` : `authenticateOmadaClient`.
  - **pause:** lookup → `disconnectOmadaClient(clientId)`.
- **resume:** re-auth from portal-captured `apMac/ssid/radioId` (a roamed client's stored copy is
  stale, so fetch fresh at resume if needed) + flash `pausedRemaining`.
- Cache shrinks to a lightweight local map (`ip↔mac`, `socketFd`, portal `apMac/ssid/radio`,
  `pausedRemaining` mirror), filled only by `handleRoot`.
- Admin dashboard active-session list + `VOUCHER_ACTIVE_COUNT`: on-demand fetch when viewed.
- **Bonus:** dissolves the C2 re-IP problem (look up by MAC on demand, always current) — the open
  re-IP test in `memory/test_plan.md` becomes moot.

**OPEN QUESTION (parked) — can we drop `apMac`/`radioId` from auth entirely?** Omada docs say
they're required; the mock auths fine without them. Built `test/auth_field_probe.py` (`--cloud`,
reuses `mock_omada._do_cloud_auth`) to settle it against the live controller. **Result so far:
inconclusive** — couldn't get a clean read:
- Pending client (`authStatus==1`, see [[sites-clients]] for the enum) on the prod **"EllaFi Voucher"
  SSID** rejects `authType:4` (external portal) auth **with `-41501` regardless of apMac/radioId**.
  The SSID is a **voucher** portal, so it refuses external-portal auth outright — the field question
  is untestable there.
- To answer it: run the probe against a **pending client on an External-Portal-typed SSID** (watch
  the `ssid=` column in the probe's list output). If minimal body → `authStatus 1→2`, drop the
  fields. Until then, **keep them** (conservative branch above).

**⚠ DEPLOYMENT FLAG (separate, possibly a launch blocker):** the firmware's own auth is `authType:4`,
and it got `-41501` on a pending client on the production voucher SSID. **The vendo/coin machines
won't authorize anyone until there's an SSID configured as *External Portal* in Omada.** Confirm
that portal exists before relying on the coin side in production.

## Out-of-service banner: auto-recover the admin view (poll / refresh link)

When the controller is down, the admin dashboard shows the red out-of-service banner
(`#service-banner` / `updateServiceBanner`), but it's **passive** — it reflects whatever the last
`/status` said and doesn't update until the user manually reloads. So after the controller comes
back, the banner can sit red even though service has recovered.

**Want:** while the banner is showing, the admin page should **auto-poll** `/status` (e.g. every
~10s) and clear the banner the moment `serviceReady` flips true — and/or put a **"Retry / refresh"
link in the banner** so the user can force a re-check without a full reload. Mirror the client-side
pattern: the client portal already gets live state over `/ws` + `/status`; the admin out-of-service
state should be just as live. (Recovery itself is already automatic firmware-side — `loop()` retries
NTP/Omada every 30s — this is purely making the **admin UI** reflect it without a manual reload.)

Note: the client portal's out-of-service is live (per [[feedback_ws_close_aggressively]] notes), so
this is specifically the **admin** banner that's stale.

## Voucher batches: make the "Vouchers" column specific + consistent

In the Vouchers tab batch table ([admin.html:960](web/admin.html#L960)) the count is ambiguous and
inconsistent:
- Fetched batches show `<usedCount> / <totalCount>` (e.g. `22 / 650` = 22 **used**), but it's
  **unlabeled** — looks like it could be used, unused, or remaining.
- A just-generated batch (before refresh) shows `<pages> pages` instead — a different format.
- Header shows total **pages** (`5 batches · 25 pages`).

**Want:** decide the most useful figure (likely **unused/remaining** — "how many does this seller
still have to sell" — `totalCount - usedCount`) and **label it** clearly; make new vs fetched
batches render consistently. Data is already available: `handleAdminVouchers` returns
`usedCount`, `unusedCount`, `totalCount`, `pages` ([web.cpp:909-911](src/web.cpp#L909)).

## Mock server: log the response code + error message

When debugging voucher/controller failures (e.g. the "no more vouchers left" 502), the firmware
logs `createVoucherGroup: HTTP <code> body=…`, but the **mock** (`test/mock_omada.py`) is silent
about what it returned. Add logging so the mock prints the **HTTP status + any error message** it
sends back (and, in `--cloud` mode, the real controller's). Makes mock-side failures visible
instead of having to infer them from the device log.

---

## Sign the GitHub OTA firmware (security — before this is load-bearing)

The one-tap "Update from GitHub" (v1.3.1, `/admin/update-github`) fetches `firmware.bin` over
HTTPS with `setInsecure()` — no cert check, so a MITM on the path to GitHub could serve
malicious firmware to a field node that auto-flashes it. This is audit finding **S2**.

**Fix:** sign the firmware image at build/release time and verify the signature on-device
against an embedded public key **before `Update.end()`** (reject if it doesn't match). Options:
ESP-IDF Secure Boot / signed app images, or a detached signature published next to `firmware.bin`
(e.g. `firmware.bin.sig`) that the device checks. Same applies to the slave OTA pull from master.
Until then the auto-update is convenient but not authenticated.

---

## ✅ SHIPPED — Flasher: "Read device MAC" button (with a master-confirmation guard)

Implemented in `docs/flasher.html` (read-MAC button next to Master Node MAC + confirm
popup + Linux serial troubleshooting `<details>`). Kept here for the rationale behind the
confirmation guard:

**What:** A button in `docs/flasher.html` that reads the connected chip's MAC over USB
(esptool-js exposes it — `await loader.chip.readMac(loader)` after `loader.main()`), so a
non-technical user (e.g. Ella in the field) can get a node's MAC without a serial console.

**Why it's useful:** self-service MAC retrieval during the flash they're already doing; also
the easiest way to recover/confirm any node's MAC for `master_mac`.

**Why it's DANGEROUS — and the required guard:**
- The chip MAC is only the correct `master_mac` **if this specific device is the master**.
- `master_mac` is written to **every** node; only the master's MAC belongs there. Blindly
  auto-filling `master_mac` from whatever board is plugged in could designate the wrong node
  as master (or create two masters / no master).
- **Requirement:** reading the MAC is fine to show freely, but using it to fill `master_mac`
  must be a deliberate **button + confirmation popup** — e.g. *"Set this device as the MASTER?
  Its MAC (AC:A7:04:26:4A:D4) will be written as master_mac on every node you flash. Only do
  this for the one master node."* Never silently auto-fill `master_mac`.
- Display the MAC **colon-separated** (`AA:BB:CC:DD:EE:FF`). Omada shows MACs dash-separated;
  the firmware's `isMaster()` does a plain string compare against `WiFi.macAddress()` (colons),
  so a dash-formatted `master_mac` silently makes the node a slave.
