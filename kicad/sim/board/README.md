# Whole-board validation suite (pre-order gate)

Run everything: `./run_all.sh` — exits non-zero if any check fails.

Three layers, because they catch different failure classes:

1. **`check_polarity.py`** — for every polarized part, asserts which net the
   physical pad-1 (band / polarity mark) must land on, straight from the
   datasheet's intent. This is the check that ERC/DRC/parity structurally
   cannot do (they only verify internal consistency) — it's what caught D1
   installed backwards after it had survived every other review.
2. **`check_sim_sync.py`** — exports a fresh netlist from the schematic and
   verifies `board.inc` (the hand-maintained SPICE copy) touches identical
   nets, component by component. Kills silent sim/schematic drift. If it
   fails after a schematic edit: fix board.inc + NETMAP, then trust the sim.
3. **ngspice scenario benches** — behavioral models tuned to datasheet numbers
   (`models.inc`), stimuli per scenario.

## Benches

| bench | scenario | key results |
|---|---|---|
| `tb_powerup` | 12V step, soft-start, WiFi bursts, solenoid kick | 5V=4.96, 3V3=3.29, 0.63A worst draw vs 1.5A fuse hold |
| `tb_coin_pulse` | three 50ms coin pulses | GPIO4 0.16/3.29V, opto LED 8.3mA, width preserved |
| `tb_coin_fault` | noise burst on coin wire; 12V hard short | no ghost low; D1 clamps 8.8V/7.2A; GPIO4 unmoved |
| `tb_enable_channel` | Q1 session on/off, lamp+acceptor+coin | Vds(on) 20mV; D4 5.6mA, 0V reverse |
| `tb_inductive` | relay coil kick, with/without acceptor | 20V / 44V avalanche 73mA (~80µJ, trivial) |
| `tb_hotplug` | live 12V plug-in, lead-inductance ring | rail peaks 19.3V vs 24V buck rating |
| `tb_reverse` | adapter backwards (3A-limited brick) | rail clamps −0.79V, F1 trips (D6=1.5SMC18A) |
| `tb_fuse_trip` | sustained coin-wire short, 1.2s window | F1 trips at 0.61s; D1 current → 0 after |
| `tb_opto_aging` | coin pulse at CTR 100/50/30% | clean low at all three (post R1/R5 change) |
| `tb_panel3wire` | 3-wire panel on J2.2: NPN-input + 12V-pullup styles | pin peaks 2.06V in boot (safe); see notes |
| `tb_montecarlo` | 60 trials: R tolerances 1%, CTR 0.5–2.0 | 5V solid [4.90..5.04]; all coin lows clean |

## Closed findings (fixed in schematic/PCB, 2026-08-06/07)

- **Opto CTR margin** — R1 1k→**2.2k**, R5 220→**1k** (values + Yageo PNs
  updated in schematic and PCB; no routing change). Before: the opto had to
  sink ~8.3mA (pullup + green LED) and bottom-of-bin (CTR 50%) or aged FOD817s
  left GPIO4 at 0.83–0.88V — above the ESP32's 0.825V logic-low ceiling (~5%
  of new builds per Monte Carlo). After: sink ~2.6mA → 0.05V low at CTR 50%,
  0.20V at CTR 30%. Why not 4.7k: a larger pullup recovers too slowly from
  coupled noise bursts (dip 0.40V = false-low territory); 2.2k keeps the
  noise dip at 0.88V, above threshold, with the firmware pulse-width check as
  the second layer. Green LED 5mA→1.1mA, still visible.

- **D4 reverse voltage** — moved with R4 to the gate side; 0V reverse in all
  scenarios (was 7V continuous / 15V kick vs a 5V rating).
- **Reverse-polarity protection** — D6 swapped 1.5SMC18CA → **1.5SMC18A**
  (unidirectional), cathode band toward the 12V rail; verified on PCB pads.
- **D1 installed backwards** — cathode now on the coin line; caught by the
  pad-level polarity check, now automated in `check_polarity.py`.

## Notes from the panel bench (closes the globals.h J2.2 TODO)

- 12V-pullup-style panel: during the ESP32 boot window (pin high-impedance)
  the pullup would push GPIO14 toward 6V — but D4 in its new gate-side spot
  clamps the node at ~2.1V. **Safe, but the safety now depends on D4 being
  present**; Q1 partially conducts during boot (acceptor/lamp briefly
  energized — harmless, firmware isn't counting yet).
- NPN-input panel with 10k series resistance: 3.3V drive is only marginal
  (transistor active, not saturated). Panels with ~1k series input are fine.
  Measure the actual panel's input before relying on J2.2 logic drive.
- Suggest updating the comment at src/globals.h:141 with this verdict.

## Model honesty / limits

- Buck: averaged model, ~40kHz loop crossover → realistic ~50mV droop per
  300mA WiFi step (2MHz switching-level input behavior: `../ng_mpm3620.cir`).
- Polyfuse: thermal I²t model calibrated to MINISMDC150F curve shape
  (1.5A never, 7A ≈ 0.4–0.6s, 70A ≈ ms). Trip is modeled, re-latch is not.
- In a *sustained* coin-line fault D1 averages ~45W until F1 trips at ~0.6s —
  beyond its 5W steady rating: D1 may sacrifice itself (TVS fail-short →
  faster F1 trip, GPIO stays protected). Acceptable: requires a wiring fault.
- Opto CTR is settable via `.param FOD817_CTR` (aging/bin sweeps).
- Not covered: layout parasitics beyond lead/harness inductance, thermals,
  RF, ESP32 digital behavior (firmware tests on hardware own that).
