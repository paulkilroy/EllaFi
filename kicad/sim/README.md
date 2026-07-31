# Buck input-spike simulations (ngspice)

The same VIN-spike test that condemned the old LM2596 buck, run on both designs.

| file | design | VIN peak / low |
|---|---|---|
| `ng_ref_rail.cir` | LM2596 (external switch loop) | **30.3 V / −7.3 V** — the spike that killed the chips |
| `ng_mpm3620.cir` | MPM3620A module (switch loop on-die) | **12.1 V / 11.8 V** — no spike |

Run: `ngspice -b ng_mpm3620.cir` (prints the `meas` results).

The MPM3620A integrates the switch, inductor, and input cap, so the external IN pin never sees the
switching di/dt — the 30 V ring (which exceeds the module's 28 V abs-max) simply doesn't happen. The
MPM3620A model is behavioral (internal cap ~4.7 µF, switch current ~1.2 A estimated); the result is
structural, not sensitive to those values. Output = FB divider: 0.798×(1+100k/19.1k) = 4.98 V.

Full sim workspace (all the LM2596 layout/parasitic experiments, plus `.raw` outputs) lives outside the
repo at `~/Documents/ella-sim/`. See `memory/hardware_notes.md` → "Spike test — VALIDATED".
