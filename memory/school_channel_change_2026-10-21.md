# School 2.4 GHz channel change — 2026-10-21 ~13:30 Daram

Omada's WLAN optimization skips mesh APs entirely, so this was manual. Mesh backhaul is
5 GHz ch 157 on all three school APs — 2.4 changes cannot touch it (verified after change).

## Change applied
- BES: 2.4 ch 11 → **1**, width 40 → **20 MHz**
- BNHS: width 40 → **20 MHz**, channel fixed to **11** (auto had followed BES to 1 at first).
- FINAL VERIFIED STATE (~14:30 Daram): Negra 6 / BES 1 / BNHS 11, all 20 MHz, all channels
  FIXED not auto (auto re-picks at will and had re-collided them within minutes).
- Negra, all 5 GHz radios: untouched.

## Why (evidence)
- BES Top-AP retry 138% (avg frame transmitted 2.4x); BES+BNHS both ch 11 @ 40 MHz, 63 m LOS.
- BES rfScan 13:18: loudest co-channel = own EllaFi radios (-64/-68) + ZEPH piso (-68) on 11;
  ch 1 worst occupant Lady Piso -72. Competitor on ch 8 smears 6+11.

## BEFORE baseline (school hours 07:00–12:00 Daram, pre-change)
| AP | airtime avg/max (rx) | path-to-router | client PHY median (14d) |
|---|---|---|---|
| BES | 84% / 88% (rx 63) | 12.4 ms | 51 Mbps |
| BNHS | 78% / 92% (rx 62) | 12.6 ms | 68 Mbps |
| Negra | 79% / 92% (rx 62) | 7.7 ms | 25 Mbps |

Also: BES retry 138%, BNHS 43%, Negra 22% (Omada Top-APs widget, day of change).

## Success criteria (first test = next school morning, recess 9:00 / lunch 11:00)
- BES airtime out of the 80s; retry% collapses; 9–11h bars amber/green at same bar HEIGHT.
- BNHS improves some (BES vacated its channel) once back on 11.
- Judge via dashboard Topology tab day pills (hover = preview, click = pin) vs "avg".

## Band steering: Balance → Prefer 5G/6G (site-wide), 2026-10-22 18:44 UTC (Oct 23 02:44 Daram, inside the 02:00–06:00 radio blackout — first live effect 06:00)
Balance had been holding ~50% of clients on 2.4 GHz at Darna/Grandma/Grandama (even client
counts per band) — on the band that is 50–70% competitor airtime. Per-device steering
thresholds left at default (one variable at a time).
Baseline (3 days before, Balance mode): 5G % of traffic Darna 55 · Grandma 59 · Negra 79 ·
House Big 56 · House AP 15 · Grandama House 78; 2.4 busy Darna 77 · Grandma 81 · Negra 73.
Canaries (indoor, no usable 5G): House AP 3.1 disconnects/h, Grandama House 2.9/h.
Verdict: `tools/tq tools/steer_check.py <change-time> 3` after 2–3 days. Success = 5G share of
clients ~50% → 65–75% outdoors, 2.4 busy down. Roll back / tune the indoor APs' thresholds if
canary disconnects rise by half or more.

## PtP point-to-multipoint plan + capacity baseline (2026-10-23)
- The EAP215-Bridge supports 1-to-4 multi-bridge (PtMP). The existing house server can feed a
  2nd client at Negra's pole: bearing 164.6° vs grandma 173.5° (9° apart), 121 m. No new
  server, no house port needed.
- Negra then becomes the school's wired mesh root (BES/BNHS mesh one hop to it) and the school
  mesh can move off 157 (e.g. ch 44). Negra's 5G stays double-duty until BES/BNHS are wired.
- Two-device injector chain works: PtP client injector LAN↔LAN Negra injector (no switch).
  It can't take a third device (BES) — that needs the SG2005P-PD.
- Capacity (ptp_load.py, Oct 23): PtP today avg 6.1 / p95 15.8 / max 21.1 Mbps; Negra hop
  avg 5.8 / p95 14.1 / max 22.4; combined avg 12.5 / p95 22.7 / max 26.2 Mbps vs ~300–400
  usable (866 PHY, min seen 117). <10% of capacity.
- KEY FINDING: PtP ch 153 @80 MHz and mesh ch 157 @80 MHz are the SAME 80 MHz block (149–161).
  PtP radio busy 35–38% evenings (max 63%) while carrying ~21 Mbps = mostly school mesh air.
  Moving the school mesh off 149–161 should cut PtP busy % even as it carries more.
- Bridge speed tests (controller), heavy rain, live traffic: Oct 23 21:35 Daram ↑349.6 / ↓247.4 Mbps;
  21:38 ↑281.2 / ↓234.2 Mbps (avg ↑315 / ↓241)
  — ~10x peak demand. Re-run after the point-to-multipoint change; expect higher once the
  school mesh leaves the 149–161 block.
- Oct 23 evening incident: video calls failed. Cause = Starlink stalling in heavy rain (app
  throughput dropping to ~0 every 20–30 s, 20:36–20:44 Daram). LAN/PtP/mesh healthy (zero loss);
  the controller stayed up (6.4 days uptime). The iPhone app's "controller offline" and "PtP
  server no downlink" were the app losing its cloud view during the stalls.

## First results (Oct 23, rainy, confounded)
- Steering moved traffic to 5G: BES 67→86%, Darna 55→68, Negra 76→86, Grandama 79→86, House AP
  16→23. Indoor canaries fine (House AP disc/h 3.6→2.3, Grandama 3.1→3.5). 2.4 busy did NOT drop
  (competitor airtime floor).
- School-hours path latency BES 21→85 ms, BNHS 21→57 (Darna wired control flat at 0.9). BUT Oct 23
  was rainy with +30% school load (Negra mesh hop 28.4 GB 07–12 vs 21–22 GB). Can't attribute to
  steering vs load yet. Verdict: dry school day, Mon Oct 26+.
- Either way: the school mesh is near saturation (+30% load → 4× latency). Strengthens the backbone case.

## Oct 24 upstream drop investigation (Daram times) — raw results, no conclusions
Path facts: Starlink router → cable → POE170S injector (LAN) → injector PoE → ~1–3 m cable →
House switch port 5 (PD power-in). OC220 on switch port 4; PtP server on the OC220's 2nd port.
Router sees one wired link only (Network → Devices: iPhone, mxchip, OC220, SG2005P-PD).
Pings run FROM the house switch (Omada device ping):
- → 8.8.8.8: 11% loss, one block of 11 (time not noted)
- ~02:30 → dish 192.168.100.1 (TTL 63, reachable): 29% loss, blocks of 5/12/6/6, slow first reply after block
- ~02:50 → router 192.168.1.1: 13 of 45 lost, blocks of 7 and 6
- ~03:00 → controller 192.168.1.83 (port 4): 0% (control)
- ~03:10 → router: 23% loss, blocks of 6/7/10
- 03:27 port 5 read STP "Blocking" once (Forwarding at ~02:45); loop detect 0
- 03:35–03:45 remote API to controller failing
- ~04:55 → router: 0% and 0% (two runs; nothing changed on site)
- PORT 5 LINK SPEED (port_poll): 1 Gbps steadily Oct 21 02:19 → Oct 24 04:15 Daram; 100 Mbps
  from the 04:30 sample. ALL failing runs were at 1 Gbps, BOTH clean runs at 100 Mbps.
  Gigabit uses all 4 pairs, 100M only 2 → pattern fits a bad pair 3/4 (cable, injector port,
  switch port 5, or router adapter). Not proven. Test: lock port 5 at 100M-full for a day;
  after each of Ella's swaps, return to auto and see whether gigabit holds.
Cable test 02:15: port 5 "ok" reads 13 m though the cable is 1–3 m (test may span the injector).
Starlink app: dish ping ~99.96–99.98%; router→PoP 100%; router→Cloudflare dropped 02:36–02:39
(separate upstream issue). Ella's plan: baseline test → router restart → swap injector→router
cable → swap switch→injector cable → router into switch port 2 (PoE off), test after each.
Only meaningful while drops are occurring.

## Oct 24 — school moved onto the PtP (point-to-multipoint), ~11:15 Daram (03:15 UTC)
Facts (controller reads):
- Spare bridge = EAP215-Bridge(US) **v2.0** (3-port, 24 V passive PoE only — house switch can't
  power it; own injector). fw 1.0.0 → 1.1.2. Linked to the v3.0 PtP Server via Bridge → Rescan.
  Named "Negra PtP Client"; Voucher/Piso/Test SSIDs off (Mahal still on, like the other bridges).
- Link at Negra's: −53…−56 dBm, 866/866 Mbps on ch 153 (Grandma PtP Client: −60, 866/780).
- Injector LAN↔LAN cable in → Negra kept its mesh uplink and listed the bridge as a downlink
  (two paths); **Negra reboot** → Negra wired (controller shows wired uplink "House Swich" port 4,
  1 Gbps — the PtP Server sits behind the controller on port 4), mesh dropped.
- Negra's 5 GHz went to its SAVED channel **40** (80 MHz); BES and BNHS followed, meshed to Negra
  on 40 (BNHS −67, BES −69). Grandma AP is also on 40 (Negra doesn't hear it; BNHS −89).
  Planned: set Negra to 52 (80 MHz) — not yet saved at the time of this note.
- Before (school hours 07–17, Oct 22 / Oct 23): path to router Negra 28/37 ms, BES 19/53,
  BNHS 20/48; House Big 5G busy 44/45%; PtP Server busy 32/32% (max 59). Expect Negra ~7–15 ms.
- Same day: Negra AP's antennas found pointed toward the sea (horizontal) — explains its
  ~18 dB-weak 2.4 GHz; fix (point up) pending. Monday's school check compares against this new
  setup, not Oct 22.

## Band-steering verdict (steer_check.py 2026-10-22T18:44 3; run Oct 25 ~16:00 Daram, 68 h after)
- 5G share of CLIENTS (outdoor): BES 58→60, BNHS 41→31, Darna 48→53, Grandma 50→55, House Big 42→40,
  Negra 59→67 — did NOT reach 65–75% (only Negra close). 5G share of TRAFFIC rose: BES 67→84,
  Darna 55→67, House Big 57→72. 2.4 busy flat (75→78, 79→76, 77→78, 81→82, 68→67, 73→74) — the
  competitor-airtime floor. Disconnects/h fell nearly everywhere (BES 19→7.7, BNHS 9.1→3.7).
- Canaries: House AP 3.6→2.3/h (down), Grandama House 3.1→3.7/h (+19%, under the +50% bar) → keep, no tuning.
- Confounded: after-window includes the Oct 24 PtP switchover, Maridel going live, a weekend, and rain.
- Verdict: keep "Prefer 5G". It moved heavy users' traffic to 5G; it can't move phones that can't hold 5G
  at the edge, and it can't lower 2.4 busy (that's competitors).

## PtP point-to-multipoint — first comparison (Oct 24 03:30 UTC → Oct 25, weekend only)
- Before (Oct 21–24): Grandma bridge avg 6.8 / p95 16.1 / max 21.1; school via Negra mesh 6.3 / 15.0 / 25.1;
  combined 13.9 / 23.1 / 30.1 Mbps. PtP Server 5G busy avg 22%, evening 32%, max 63%.
- After (Sat–Sun, no school): Grandma bridge 9.1 / 18.3 / 25.5; Negra bridge 2.8 / 7.3 / 10.7; combined
  11.9 / 25.1 / 31.8 Mbps. PtP Server busy avg 33%, evening 51%, max 67% — UP ~half at similar carried load.
- Likely causes (unproven): the server now takes turns with two clients; House Big 5G moved by the optimizer
  to ch 149 (Oct 24 15:30) inside the PtP's 80 MHz block (149–161); PtP itself moved 153→149 Oct 25 06:30.
  Capacity still ~10× peak demand. Re-run on a school day (Mon Oct 26+) for the real school comparison.
