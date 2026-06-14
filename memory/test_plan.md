# v1.3.5 (build 45) + follow-ons — Pre-commit Test Plan

Covers the whole uncommitted stack: HMAC mesh, C2 cache, admin login, graceful degradation,
voucher/seller UI, logging, SSID tripwire. Check items off as you verify on hardware.

Legend: 🔴 high-risk / must-pass · 🟡 medium · ⚪ low / nice-to-have · ✅ already verified

---

## 0 · Flashing (flag day)
- [x] ✅ 🔴 Serial-flash **all nodes** to build 45 (HMAC changes the wire format — old/new nodes can't talk, and OTA can't bridge the gap). Verified 2026-06-12.
- [x] ✅ Each node boots, joins WiFi; master `/admin` shows **build 45** for every node. Verified 2026-06-12.

## 1 · HMAC mesh  🔴 (highest risk — crypto + flag day)
- [x] ✅ Master sees every slave's heartbeat (build 45) — no `bad HMAC` / `stale` spam between own nodes. Verified 2026-06-12.
- [x] ✅ 🔴 **Adversary rejection** (`test/hmac_inject.py`): garbage → `bad packet`; forged-tag → `rejected: bad HMAC` (no coin); replayed/stale → `rejected: stale`. Verified 2026-06-12.
- [x] ✅ Slave coin pulse → master **credits it** under the slave's MAC (no `replay`). First coin ~1-2s (500ms master boot-suppress + acceptor warm-up), rest fast. Verified 2026-06-12.
- [x] ✅ Session start/end propagate master→slaves (slave logs accepting/disabled + LED toggles). Verified 2026-06-12.
- [x] ✅ Bump to build 46 → **slave OTA works**: master serves `/fw.bin`, slave downloads/flashes/reboots to 46 (watch slave serial — no `/admin` on slaves). Confirmed working.

## 2 · Graceful degradation / out-of-service  🟡
- [x] ✅ Wrong/unreachable `omada_url` at boot → `/admin` loads, LED red, banner shows reason, coins refused.
- [x] ✅ Fix URL in Settings → reboot → recovers (LED solid, banner gone, coins work).
- [ ] No-internet boot → banner reads **"clock not set"**, out of service.
- [ ] Controller dies mid-run → within ~60 s: banner + red LED + coins refused. Restore it → recovers within ~30 s **without a reboot**.

## 3 · Admin auth + reboot  🟡
- [ ] `/admin` shows the login overlay; correct password logs in; dashboard data loads.
- [x] ✅ Wrong password rejected; **5 wrong tries → "Too many attempts — wait a minute."** cooldown. Verified 2026-06-13.
- [x] ✅ 🔴 **Reboot the node with the admin tab open** → "Reconnecting…" overlay during the down window, watchdog reconnects, login pops cleanly, no console spam; re-login repopulates. Verified 2026-06-12.
- [x] ✅ "Log out" button → returns to login cleanly (no empty-dashboard flash). Verified 2026-06-12.

## 4 · Core coin flow + session cache (C2)  🔴
- [x] ✅ Insert coins → session authenticates, client gets WiFi. Verified 2026-06-12.
- [x] ✅ **Pause → resume** preserves remaining time to the ms (1431699ms carried across the disconnect/re-auth). IP correctly populated post-fix. Verified 2026-06-12 (same-IP).
- [ ] ⚠️ **Re-IP** specifically: client changes IP between pause and resume → cache re-points by MAC (`MAC … moved IP:` log), resume found from the new IP. *Not yet exercised — client kept 10.0.0.20 throughout.*
- [ ] Extended run (an hour of normal sessions) → **no crashes / unexpected reboots**.
- [ ] Dashboard vendo/voucher active counts look right.

## 5 · Vouchers + sellers UI  🟡
- [x] ✅ Generate a batch → Omada shows **"By Usage"** timing; PDF prints right on **phone and desktop**. Verified 2026-06-12.
- [x] ✅ Vouchers tab: **"Used" column = used/total (X/Y)**; batches sorted **newest-first**. Verified 2026-06-12.
- [ ] Self-heal: with `sellers.json` missing/wiped, the **Sellers list repopulates names** from existing voucher groups.
- [ ] Hit the voucher cap → "no more vouchers" error shows **in the UI and in the error log** (not just serial).

## 6 · Logging + error log  ⚪
- [x] ✅ Force a controller error → it appears in the **Error Log** (W/E). Fixed an unbounded-log bug: read now renders last 200, file capped to 500 at boot. Verified 2026-06-13.
- [x] ✅ Logs tab **"Clear"** button → confirm → error log empties. Verified 2026-06-12.

## 7 · SSID tripwire  ⚪
- [x] ✅ With customers on a separate SSID from the nodes: dashboard **"Mgmt SSID Isolation: Clear"**. Verified 2026-06-12.
- [x] ✅ (If it ever trips) a customer on the management SSID → dashboard **breach** + error-log entry (`SECURITY: hotspot client … on the management SSID 'MomDad'`). Verified 2026-06-12.

## 8 · Mock server (dev only)  ⚪
- [x] ✅ `mock_omada.py --cloud` prints `← HTTP <status> … errorCode=… msg=…` on every response (direct + cloud-proxied). Verified 2026-06-12.

---

### Sign-off
- [ ] All 🔴 items pass → safe to commit + tag **v1.3.6** (pull `--rebase` first; push master before tagging).
- **Open test (deferred):** C2 re-IP re-point (#4) — verify on live controller or in `handleRoot` code; same-IP pause/resume already proven.
- Deferred (not in this release): signed OTA (S2), persist admin sessions across reboot.
