# Shaper lab — results so far and parked work (2026-11-10)

Goal: replace Omada's per-client 2/1 limit (a policer: drops everything over a 2-second bucket) with a box that
**queues** instead, so the data that already crossed Starlink isn't thrown away and re-sent.

## Lab setup (fixed roles — don't reassign)
- **Pi 4** (192.168.1.174, AT&T router) = download server: lsquic QUIC server `quic-server.service` (:4433, `-A`
  picks the sending method: 1 Cubic, 2 BBRv1, 5 BBRv3) + HTTP `farm-server` (:8080).
- **pi02w** (192.168.0.105–.107, Mahal Wi-Fi) = 3 fake phones (p1 wlan0, p3/p4 VRFs on the USB dongle).
  Fake phone = `lab/farm/scroll3.py` (Reels-like, deterministic, built from Paul's iPhone measurements).
- **shaper** mini PC (192.168.0.109, ORIGIMAGIC "Comet", Ryzen 5 3500U, 4 GB, 256 GB NVMe, 2 × Realtek
  RTL8111) = transparent bridge br0 (eno1 router side, enp4s0 AP side) between the ER605 and the EAP603.
  Code: `lab/shaper/shaper.py` (HTB lane per MAC + fq_codel inside; learns MACs every 2 s).
- TestLab controller = cloud Essentials; scripts use the OLD login from `data/config.json.bak`.
- **Left at 2026-11-10:** shaper DOWN (bridge passes unshaped), Mahal AP limit OFF, gateway rule "Lab 2/1" off.
- **VLAN trunk (2026-11-14):** ER605 network "Lab VLAN 98" (192.168.98.1/24, DHCP .100–.199) on all 4 LAN ports +
  SSID "EllaFi Lab98" (WPA2, VLAN 98, no portal/limit). Paul's phone got 192.168.98.100; every frame crossed the
  bridge tagged 802.1Q vlan 98 (both ports), Linux bridge passes tags untouched. Mahal stays untagged (VLAN 1).
  Plan: shaper caps only customer VLANs → Mahal never capped, no MAC list, no controller polling.
  Adding an SSID restarts the AP radios: pi02w's built-in wlan0 (p1 + the SSH path) did NOT rejoin by itself
  (needs a power cycle); the USB dongle phones did.

## Results (all counts measured, with packet headers)
| run | lost (server sent vs phone got) | notes |
|---|---|---|
| Omada AP 2/1, Pi phones (3 repeats) | 32–33% | worst wait 7.5 s, 6–8 freezes/run |
| Gateway (ER605) 2/1, Pi phones | 37% | ER605 dropped ~90 MB before the box; no better than the AP |
| Shaper 2/1, fq_codel target 10 / 20 / 50 / 100 / 200 ms | 10 / 9 / 6 / 4 / 1% | 0 freezes; 50 ms repeated 3× at 6% |
| Shaper with Cubic senders | 14% (Omada 39%) | |
| **Real iPhone, FB Reels, shaper 200 ms, 3 s swipes, cold start** | **0.5%** (62.1 MB in, 61.8 out) | Meta arrived at ~2 Mbps from the start (behind Omada it pushes 9–11). Ping while scrolling avg 16 ms. Paul: ~6 of the first ~20 videos 1–2 s late, then smooth |
Raw: `lab/farm/results/` (ab3-*, overnight-20261109-2242, iphone-20261110-*). Memory: shaper_lab_results.md.

Why the early slow starts: on a cold start the app downloads many videos at once and they share the 2 Mbps lane
equally, so the next video's first 2.5 s takes ~6 s instead of 1.25 s. The lane was full (2.0 Mbps) the first
40 s. Not waste — capacity.

## Parked to-dos (in order)
1. ~~**Fix the fake-phone hang.**~~ DONE Nov 13 (commit 8357430): every wait stops at the end time (`"stuck"`,
   `"stuck_at"`, `"busy_when_stuck"`), a piece with no data for `--stall-s` (20 s) is stopped, logged in
   `"stall_log"` (video, piece, connection) and refetched. Tested locally only (lab unreachable): copy scroll3.py
   to pi02w `~/farm/` before the next run, and read `stall_log` after it to see which downloads stall.
2. **Re-run what the overnight missed** (`lab/farm/overnight3.sh` + gateway/pass 2 of overnight2.sh): weak/bad
   phones (netem), overall ceiling 4 Mbps, 256 lanes (memory/CPU), repeats. Watch-10-s freezes (shaper 33 vs
   Omada 18) — probably the 8 downloads sharing the lane equally; confirm.
3. **Speed bank** (fix the early slow starts without raising the average): first test single-layer
   `--burst-kb 10240` (no code change) with the iPhone procedure; if waste or ping jumps when the bank runs out,
   build the two-layer lane in `lane_add` (outer rate=ceil=2, burst=cburst=bank; inner rate 8kbit ceil=peak,
   leaf on inner, class id n+0x4000; update lane_del/stats; flags `--bank-mb`, `--peak`).
4. **iPhone comparisons, same procedure** (timer page artifact WTSXBivJTCuXEHPUYHYgBt; `iphone_setup.sh`,
   `iphone_record.sh <label> <HH:MM:SS> [secs]`, `iphone_timeline.py`): Omada 2/1 shaper off (the fair
   comparison), shaper 2.5/1 and 3/1, bank. Measure Mbps crossing Starlink, % lost, ping, Paul's pause count.
5. **Priority class for Paul's devices / Teams** (`--priority MAC…`): guaranteed rate, served first, short wait
   (5–20 ms). Only works with an overall ceiling just under Starlink's real speed (`--total-down/--total-up`),
   so the line forms in the box, not in the dish. Lab test: ceiling 6/3, Pi phones fill it, Paul on a Teams call.
6. **Load test the box** at Bakhaw speeds: 100–300 Mbps through ~250 lanes overnight (Realtek cards are the risk;
   fallback = 2-port Intel i226 mini PC ~$150–200). Needs a wired computer on the AP side. Install lm-sensors,
   watch temperature.
7. **Make it install-ready:** proper boot service with saved settings (today it's started by hand — after a
   reboot the bridge comes up UNSHAPED), BIOS power-on after AC loss, Tailscale for remote access, on the router's
   backup power, fail-open plan (labeled spare cable or bypass switch).
8. **Monitoring on the same box:** Starlink dish status (gRPC 192.168.100.1 — check reachable at Bakhaw),
   internet pings, per-lane delivered/dropped bytes by MAC → ella-usage-tracker every 15 min; measured Starlink
   speed feeds the overall ceiling. Fenced services (memory/CPU limits, capped logs, upload timeouts + buffer to
   disk) so monitoring can never take the bridge down. First check which machine runs today's collector.
9. **Bakhaw switchover plan (needs Paul's approval):** shaper on and Omada per-client limits off in ONE step (both
   = two drop points). Keep in Omada: airtime fairness, band steering, WMM, portal/vouchers. Decide on a loose
   Omada backup limit (above the bank peak, e.g. 8/4) vs none.
10. **TP-Link reply (case #350801)** — draft exists; add these results; don't send until Paul approves.
11. Later/maybe: CAKE leaf (`--leaf cake`, needs rtt ~4 s for a 200 ms wait) only if upload becomes a problem;
    LibreQoS rejected for now (needs a customer list by IP, multi-queue Intel NICs; our box has single-queue
    Realtek); measure competitors' limiters with wifi_compare.py (MikroTik PCQ default line ≈ 0.2 s at 2 Mbps,
    so "no lag" sellers may queue too); TikTok/Shorts iPhone captures; Darna AP in the collector.
