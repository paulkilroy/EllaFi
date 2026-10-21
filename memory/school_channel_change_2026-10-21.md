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
