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
