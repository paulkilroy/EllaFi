# Hardware Notes

## PCBway assembly — ESP32 socket

Note to provide PCBway when ordering assembled boards:

> **Assembly notes — ESP32 socket**
> - **Solder the female header sockets** (through-hole) for the ESP32 footprint to the board.
> - **Do NOT populate the ESP32 module** — marked **DNP** in the BOM; installed by hand after delivery.
> - Populate all other components per the BOM.

- ESP32 module is **socketed, not soldered** — user inserts it themselves.
- In the BOM: ESP32 module = **DNP**; the female header sockets = **include/populate** (call them out
  separately so PCBway doesn't skip the sockets along with the DNP module).
- Board is the ESP32-S3 (DevKitC-1 class) per firmware target.

## Design review (2026-06, verified against KiCad netlist + PCB)

KiCad project lives at `~/Documents/EllaFi-PCB/` (NOT in the repo). Regenerate a fresh netlist with:
`kicad-cli sch export netlist --format kicadsexpr -o /tmp/x.net EllaFi-PCB.kicad_sch`.
Board: **43.8 × 97.2 mm, 2-layer**, all-THT, ESP32-S3 DevKitC-1 socketed (U2, DNP).

### Topology (confirmed from nets)
- **Power:** J3 barrel / J2.1 / J1.1 = +12V → U3 LM2596-5.0 buck (L1 100µH, D2 1N5822 catch, C1 in /
  C2 out) → 5V → ESP. U3 ON/OFF(pin5)→GND (enabled), FB(pin4)→5V out. **Buck wired correctly.**
- **Coin enable (Q1 IRLZ44N, low-side):** GPIO14 → Q1 gate (with **R6 10k pulldown → off at boot** ✓).
- **Coin signal (opto U1):** coin pulse (J1.2) → U1 → GPIO4 via R1 1k; GPIO4 uses internal pull-up.
  Opto input LED fed from **3V3 via R2 220Ω** (~9.5mA, not over-driven). D1 P6KE6.8A TVS on the coin line.
- **LEDs (passive):** D3 red = 5V power (R3 330Ω); D4 yellow = coin-accept-enabled; D5 green = coin pulse.
  Firmware status LED is the onboard WS2812 (GPIO48), separate from these.

### J2 + SW1 — 2-wire vs 3-wire coin/LED panels (by design)
- **2-wire** (SW1.2→1): J2.3 ties to Q1 drain → panel **GND is low-side switched** by Q1 (active only when
  GPIO14 high). J2.2 unused.
- **3-wire** (SW1.2→3): J2.3 → constant GND; **J2.2 carries the GPIO14 enable** (active in coin-accept).

### Coin-pulse path (same in both modes)
The coin pulse always enters on **J1.2** (the 4-pin acceptor socket: +12V / pulse / switched-GND / NC),
→ opto → GPIO4. J2/SW1 only switch the **LED panel**, not the pulse. The acceptor's ground (J1.3) is the
**Q1 drain**, so the pulse loop (`3V3→R2→opto→J1.2→acceptor OC→J1.3→Q1→GND`) **only completes when GPIO14
enables Q1** — no phantom coins when disabled. Two consequences:
- **`Net-(D4-K)` (Q1 drain) is the most current-loaded switched node** — it sinks the *combined* ground
  return of the coin acceptor **and** (in 2-wire) the LED panel **and** the yellow LED — yet it's a 0.2mm
  trace. Reinforces the trace-widening item.
- **2-wire LED panel is powered only while coin-accept is enabled** (its ground is Q1-switched). If the
  panel should display at idle, confirm GPIO14 is normally high, or use 3-wire (constant power). J1.4 (NC)
  = if this acceptor model has an inhibit/counter pin there, it's unused (disable is via ground-switching).

### ✅ Verified correct
Q1 gate pulldown present; LM2596 ON/OFF grounded + FB to output; opto input level (3V3/220Ω); D1 TVS on
the coin line; control GPIOs avoid strapping pins (GPIO0 = BOOT/coin button used deliberately).

### 🔴 Issues for rev B
1. **All 146 tracks are 0.2 mm (8 mil)** — including +12V, 5V, and the switch node. Sized for ~0.5A but the
   supply is a 3A design. **Widen the power path** (+12V/5V/switch node) to ≥0.5mm (≥1.5mm for full 3A).
2. **GND pours on both layers but 0 stitching vias** — add ground vias, especially around U3 (switcher).
3. **3-wire mode exposes GPIO14 directly on J2.2** (only a 10k pulldown, no series R/clamp). If a 3-wire
   panel's enable is 12V-referenced it back-drives 12V into GPIO14 → **kills the ESP**. Verify the panel's
   enable is 3.3V-logic-safe, else add series R + 3V3 clamp on J2.2 (or buffer through a transistor).
4. **No input fuse, no reverse-polarity protection** on J2/J3.
5. **U3 thermal:** vertical TO-220, 2-layer — fine ≤~1A, needs a clip-on heatsink above ~1.5A. Measure
   full-load trace/U3 temps before trusting at higher current.
6. Verify **L1 Isat ≥ ~3.5A** and consider **C2 220µF** (vs 100µF) per the LM2596 datasheet.

### Note: opto is NOT galvanically isolated
U1's LED anode ties to board 3V3 and its emitter to board GND — both sides share the board domain, and the
acceptor runs off board 12V/GND, so there is **no isolation** (only spike band-limiting, which is the real
value). Fine for a same-supply acceptor; would need the LED driven inside the acceptor's own floating loop
with split grounds for true isolation. Firmware comments corrected accordingly.

## Misc

- ESP32-S3 does **not** need BOOT held during serial upload (auto-reset).
