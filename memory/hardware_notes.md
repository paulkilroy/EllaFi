# Hardware Notes

## PCBway assembly — ESP32 socket

Note to provide PCBway when ordering assembled boards:

> **Assembly notes — ESP32 socket**
> - **Solder the two 1×22 female header sockets** (J4, J5 — through-hole, 2.54 mm) to the board.
> - The **ESP32 module is not supplied and not populated** — it is plugged into those sockets by hand.
> - Populate all other components per the BOM.

**As-built representation (verified 2026-07-23 against the schematic):**
- **J4 + J5 = the sockets** — `Conn_01x22_Socket`, footprint `PinSocket_1x22_P2.54mm`, both
  `in_bom yes` / `on_board yes`. These are the real BOM line (2× 1×22 female header) and carry all wiring.
- **U2 (`ESP32-S3-DevKitC`) is drawing-only** — `in_bom no` / `on_board no`, and it has **zero net
  connections**. It exists solely to show the pin names/row spacing. It is *not* DNP — it is excluded
  entirely, so it never reaches the BOM or the PCB.
- The ESP module itself is therefore **not a BOM line at all**; Paul plugs in his own
  ESP32-S3-DevKitC-1U-N16R8 (male pins already soldered to it), replaceable by unplug/replug.
- Two separate 1×22 parts (rather than one 2×22) so the BOM shows a clean qty-2 line.

## ▶ READY TO IMPLEMENT — rev-C schematic state + remaining work (verified 2026-07-23)

Everything below was read back out of `EllaFi-PCB.kicad_sch` (KiCad 10, saved Jul 21) via a fresh
netlist + ERC — not from memory. **ERC = 15 violations: the 5 `pin_not_connected` listed below, plus 10
`pin_to_pin` "Unspecified/Passive" notes that are benign connector-pin-type noise.**

### ✅ DONE — buck is complete and matches the locked reference exactly
Designator map (the reference text above uses generic names; **as-built** designators differ):

| Reference doc | As-built | Value | Verified net |
|---|---|---|---|
| U1 (module) | **U3** | MPM3620AGQV-Z, QFN-20 | — |
| C1 input | **C1** | 10 µF 0805 | IN node (with F1.2, D6.2, R9.1) |
| C2 output | **C2** | 22 µF 0805 | OUT node (U3.7/8/9) |
| R1 FB-top | **R7** | 100 kΩ 0402 | OUT ↔ FB |
| R2 FB-bot | **R8** | 19.1 kΩ 0402 | FB ↔ GND |
| R3 EN pull-up | **R9** | 100 kΩ 0402 | VIN ↔ EN(17) |

Confirmed wired: IN(16), EN(17), FB(1), OUT(7,8,9), AGND(3)+PGND(12,13,14)→GND.
Confirmed **floating as specified**: PG(18), NC(10,15,19,20), VCC(2), BST(11), SW(4,5,6) — none appear in
the netlist. **Vout = 0.798 × (1 + 100/19.1) = 4.98 V.**

### ⚠️ J4/J5 ↔ ESP pin mapping — J5 IS REVERSED (read before wiring or placing)
The `ESP32-S3-DevKitC` footprint numbers **counterclockwise**: left row pads 1→22 run **top→bottom**
(pad 1 at y=2.54, pad 21 at y=53.34); right row pads 23→44 run **bottom→top** (pad 23 at y=50.80,
pad 43 at y=0.00). So:

- **J4 = left row, straight:** `J4.n = ESP pin n`
- **J5 = right row, reversed:** `J5.n = ESP pin (45 − n)`

This is not a guess — the four already-wired GND pins only land on real ESP grounds under the reversed
mapping: J5.1=pin 44 (GND), J5.21=pin 24 (GND), J5.22=pin 23 (GND), plus J4.22=pin 22 (GND). Under a
straight mapping, J5.21 would be **pin 43 = GPIO43/U0TXD shorted to ground** — so the existing wiring is
correct *as long as the placement matches*.

> **PCB placement rule:** place J5 so **its pin 1 sits at the same end as J4.1** (i.e. J5's numbering runs
> opposite to J4's down the board). Place it the other way round and every row-2 signal is mirrored.

### 📐 J4/J5 PLACEMENT DIMENSIONS — must match the ESP module exactly or it won't seat
Measured from the `ESP32-S3-DevKitC` footprint (44 pads, 22 per row), 2026-07-30. **U2 gets deleted, so
these numbers ARE the only spec left — place J4/J5 to these or the DevKitC-1U pins won't line up:**

| Dimension | Value |
|---|---|
| **Row-to-row spacing** (J4 row ↔ J5 row, centre-to-centre) | **22.86 mm (0.900")** |
| **Pin pitch** (within a row) | **2.54 mm (0.100")** |
| **Pins per row** | **22** |
| **Row length** (pin 1 ↔ pin 22, centre-to-centre) | **53.34 mm** (= 21 × 2.54) |

Place the two 1×22 sockets as a parallel pair: rows 22.86 mm apart in X, pins on a 2.54 mm grid in Y,
both rows spanning the same 53.34 mm — and remember the **J5 reversal** (J5 pin 1 at the same end as J4
pin 1). J4/J5 are `PinSocket_1x22_P2.54mm` (female); the module's male header seats into them. Since the
module is hand-inserted, keep the two rows on the same 2.54 mm grid origin so a straight DevKitC-1U drops in.

All remaining pins carry deliberate no-connect flags — that is why ERC otherwise only reports the socket
signal pins.

### ✅ DONE — the 5 ESP-socket signal connections (wired 2026-07-30 as net labels; ERC clean)

Added via labels placed exactly on the pin endpoints (pin coords computed + self-validated against J4's
ERC coordinates). Verified net membership in the netlist:

| Net | ESP signal | Members (verified) |
|---|---|---|
| **/5V** | J4.21 (powers the ESP) | J4.21, C2.1, R7.1, R3.1, R4.2, U3.7/8/9 (buck output) |
| **/3V3** | J4.1 + J4.2 (ESP LDO out → coin-sense supply) | J4.1, J4.2, R1.1, R2.1, R5.1 |
| **/GPIO4** | J4.4 = `COINSLOT_PIN` (active-LOW coin pulse) | J4.4, U1.4, D5.1, R1.2 (opto collector) |
| **/GPIO14** | J4.20 = `COINSLOT_POWER_PIN` (acceptor enable) | J4.20, Q1.1, J2.2, R6.2 (Q1 gate) |

### 🔍 Two dangling supply nets ERC will NOT flag
Both are fed by the 3V3 wire above; ERC calls them "connected" because each has ≥1 wire, so they are
invisible in the error list:
- **`Net-(R1-Pad1)`** — only `R1.1` on it. R1 (1 k) is the pull-up for the GPIO4 coin-sense node.
- **`Net-(R2-Pad1)`** — only `R2.1` + `R5.1`, no supply. R2 (220 Ω) drives the opto LED; R5 (220 Ω) drives
  the D5 green indicator.

Matches the documented pulse loop `3V3→R2→opto→J1.2→acceptor OC→J1.3→Q1→GND` and "opto input level
(3V3/220 Ω)" in the review below. Wiring J4.1/J4.2 to 3V3 closes all three.

### Order of work
1. ~~Wire the 5 connections (schematic) → ERC 0 `pin_not_connected`.~~ **DONE 2026-07-30 — ERC clean.**
2. **← NEXT:** Update PCB from schematic (F8). Place **J4/J5 on U2's two hole rows** — mind the J5 reversal rule.
3. Then the existing PCB punch list below: C1 tight to IN, SW copper pour, star 12 V rail, widen
   `Net-(D4-K)` (Q1 drain — highest-current switched node, still 0.2 mm), pours, DRC.

### Regenerate this analysis
```
cd kicad          # KiCad project now lives IN the repo at EllaFi/kicad/
kicad-cli sch erc --output /tmp/erc.rpt --severity-error --severity-warning EllaFi-PCB.kicad_sch
kicad-cli sch export netlist --format kicadsexpr --output /tmp/net.net EllaFi-PCB.kicad_sch
```

## Design review (2026-06, verified against KiCad netlist + PCB)

KiCad project lives **in the repo at `kicad/`** (moved from `~/Documents/EllaFi-PCB/` on 2026-07-30; source files only — gerbers/backups gitignored, regenerate on demand). Regenerate a fresh netlist with:
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

### Full ranked audit (2026-07, rev-B punch list) — supersedes the earlier 6-item list
The topology is correct BUT the board has ~5 CRITICAL issues, not one. A netlist check verified
connectivity; it did NOT catch component values, placement, or datasheet-layout rules — which is where
the real failures live. Placement/value evidence below is from the actual KiCad files.

**🔴 CRITICAL — kills the board/chips**
- **C1. Scattered buck layout (confirmed U3 killer).** Input cap **C1 is 65mm from U3's VIN**, D2 23mm,
  C2 53mm — on a ~97mm board. Huge high-di/dt loops → VIN/switch spikes exceed LM2596 ratings → dead
  regulators (2 of 6 lost at bring-up). FIX: cluster C1(+ceramic)/U3/D2/L1/C2 within mm; minimize the
  hot loop; follow the LM2596 datasheet layout / EVM.
- **C2. No HF ceramic bypass anywhere** (only 100µF electrolytics). Datasheet requires a ceramic at
  VIN→GND. FIX: 1µF+10µF X7R at VIN-GND and VOUT-GND, right at the pins.
- **C3. Zero ground-stitching vias** (via count = 0); pours join only through leads → ground bounce,
  switcher instability. FIX: stitch grounds, esp. around U3/D2/C1.
- **C4. No input protection** — no fuse, no reverse-polarity, no input TVS. This is what left every board
  exposed to the hot-plug that killed two. FIX: fuse + TVS (e.g. SMBJ15A) + reverse-polarity P-FET.
- **C5. 3-wire exposes GPIO14 on J2.2** (`Net-(J2-Pin_2)` = J2.2 + Q1 gate + GPIO14, only a 10k pulldown).
  A 12V-referenced 3-wire panel back-drives 12V into an ESP GPIO → dead ESP. FIX: series R + 3V3 clamp on
  J2.2, or buffer via a transistor.

**🟠 MAJOR — reliability/marginal**
- **M1. All power traces 0.2mm** (+12V/5V/switch/Q1-drain) on a 3A design → widen to ≥0.5mm (≥1.5mm for 3A).
- **M2. Undersized caps:** C1 100µF (datasheet ~470µF low-ESR), C2 100µF (≥220µF). Keep aluminum
  electrolytic *type* (LM2596 needs ESR) — just bigger.
- **M3. L1 Isat unverified** (Würth 7447452101, 100µH) — confirm Isat ≥ ~3.5A or it saturates under load →
  current spikes kill U3 in the field. The one value not confirmable without the inductor datasheet.
- **M4. FB sense is a ~53mm thin trace** near the switch node → jitter/poor regulation (fixed by C1).
- **M5. U3 thermal:** vertical TO-220, 2-layer, no heatsink — fine ≤~1A, heatsink toward 3A. Measure.
- **M6. No flyback/clamp on Q1 drain** (`Net-(D4-K)`) for the acceptor's switched ground — add diode
  drain→+12V if the acceptor has a solenoid.

**🟡 MINOR — cleanup/verify**
- **m1. ESP socket = U2's footprint. J4/J5 DELETED (RESOLVED 2026-07-20).**
  The ESP32-S3-DevKitC-1 socket **IS U2's footprint** — 2×22 through-holes, rows **22.86 mm (0.9")** apart,
  2.54 mm pitch, 53.34 mm long. **PCBway solders 2× 1×22 female headers into U2's holes** (one per row).
  So U2's BOM entry = the **female socket**, marked **populated (NOT DNP)**; the ESP32 module itself is
  **not a BOM part** — Paul supplies his own **ESP32-S3-DevKitC-1U-N16R8** (male header pins already
  soldered on it) and plugs it into the sockets. Replaceable = unplug/replug, no board or design change
  (any DevKitC-1 pinout fits). **J4/J5 were a redundant SECOND copy of the same 2×22 holes** (off-board,
  wired to nothing) — two footprints can't share the holes, so they were deleted; U2 is the one socket.
  (History: J4/J5 caused long confusion because U2 looked like "an IC" while J4/J5 looked like "the sockets"
  — but U2's THT footprint *is* the socket, already wired + pin-labeled. Don't re-split them.)
- **m2. Opto not galvanically isolated** (shares board 3V3/GND) — band-limiting only; don't call it isolation.
- **m3. Green LED (D5) in the opto-collector pull-up path** — works (GPIO4 internal pull-up does the work).
  D1 (P6KE6.8A) is correctly on the ~3.3V opto side, so its rating is FINE (earlier worry retracted).
- **m4. J1.4 (acceptor NC) unconnected** — correct.

**Can't verify statically (needs hardware):** transient amplitudes (scope), thermal under load (thermal
cam/load test), L1 saturation, EMI.

### Note: opto is NOT galvanically isolated
U1's LED anode ties to board 3V3 and its emitter to board GND — both sides share the board domain, and the
acceptor runs off board 12V/GND, so there is **no isolation** (only spike band-limiting, which is the real
value). Fine for a same-supply acceptor; would need the LED driven inside the acceptor's own floating loop
with split grounds for true isolation. Firmware comments corrected accordingly.

## Bring-up failures (2026-07) — root cause + debug trail

Rev-A boards: **2 of 6 lost U3 (LM2596)** at bring-up; the debugged board's U3 was dead (VIN=12.3V,
enabled pin5=0, grounded, D2 good, output not shorted, but **not switching** = dead chip).

Systematically **exonerated**: schematic topology (correct), part authenticity (genuine TI
`LM2596T-5.0/NOPB`, verified from the marking photo — correct P/N + `P+` suffix + TI logo), supply
(12.3V, center-positive **matches** the board's J3), and the GND joint (had continuity).

**Root cause = the scattered buck layout (audit C1)** — input cap 65mm from VIN + no HF ceramic + no
ground stitching + no input protection, all fed by a **hot-plugged wall brick on long leads**. Nothing
absorbs the switching/hot-plug transients on VIN → repeated overstress kills U3. Not the part, not
polarity, not the design's *connectivity* — the *physical layout*.

**Confirmation tests (do on the next board):** tack **1µF+10µF X7R across U3 VIN→GND** — if boards stop
dying, layout/decoupling confirmed. Or scope VIN (pin1→GND) during switching/hot-plug for over-spec spikes.

**Debug lessons captured as feedback:** voltmeter reads 0V even across a cold joint (use continuity for
joints); a genuine marking argues against counterfeit; and a netlist/topology check does NOT clear a
switcher — you must check **placement of C1/D2/L1 vs the IC** and the datasheet layout section.

## Bring-up procedure (safe order — use for the remaining 4 boards)
1. **Visual:** joints (esp. U3's, which wick heat to the pour), bridges, cap stripe / diode band orientation.
2. **Power-OFF meter:** rails not shorted (+12V→GND, 5V→GND finite); every U3 pin continuous to its net;
   no bridges between adjacent U3 pins (only pin3↔pin5 should be continuous, both GND); Q1 gate→GND ≈10k
   (R6); diodes conduct one way. **Use continuity, not voltage, for joints.**
3. **Bring up the logic side on a bench/breadboard 5V** injected at the 5V rail (C2+/ESP 5V pin), ESP out,
   **no 12V connected** — red LED lights, then socket ESP, flash. Sidesteps U3 entirely.
4. **Only then test U3:** 12V via a **current-limited bench supply (12.0V, 0.3–0.5A), connect-then-ramp**,
   ESP OUT. Confirm **C2 ≈ 5V** (NOT 12V) before ever inserting the ESP.
5. **Never hot-plug the live brick** — plug the barrel in first, then power the brick at the wall so its
   soft-start ramps. A buck at no-load (no ESP) is safe; testing ESP-out first protects the module from a
   shorted U3.

## Prevention — how to catch this class of issue before ordering a batch
- **SPICE (LTspice / TINA-TI) validates VALUES, not LAYOUT** — loop stability, ripple, inductor current.
  It uses ideal wires, so it would NOT have caught C1 (the killer) unless you hand-add trace parasitics.
- **TI WEBENCH Power Designer** (free): enter Vin/Vout/Iout → it selects L/Cin/Cout AND gives a reference
  layout. Catches M2/M3/C2 and shows the tight loop that prevents C1. Best single tool here.
- **Follow the LM2596 datasheet "PCB Layout Guidelines" / EVM** — catches C1/C2/C3 directly.
- **Switcher layout-review checklist:** input cap within a few mm of VIN/GND; minimize hot-loop area; HF
  ceramic; stitch grounds; input fuse/TVS/reverse. Catches C1–C4 + M1 on paper.
- **PCBWay/JLC DFM does NOT catch switcher layout** — only footprints/DRC. That's why it passed and failed.
- **Prototype ONE board and scope it before ordering N.** Building 6 before validating 1 turned a $50
  lesson into $300. This is the cheapest prevention.

## Misc

- ESP32-S3 does **not** need BOOT held during serial upload (auto-reset).

## BUCK — FINAL DECISION: MPM3620A integrated module (2026-07, SUPERSEDES the LM2596 discrete design below)

Switched the whole buck from the discrete LM2596 to an **MPS MPM3620A** integrated power module (inductor +
input/output/BST/VCC caps + synchronous FETs all inside a QFN-20, 3×5×1.6mm). Reason: **deletes the
layout-spike problem that killed the LM2596 chips** — the manufacturer already solved the internal layout;
tiny, cheap, JLC-stocked. Part = **MPM3620AGQV-Z = JLC C529064** (375 in stock @ $2.35 when chosen).

**Spec:** 4.5–24 V in (**28 V abs-max** — vs LM2596's 45 V, but fine: internal caps keep its VIN ~clean 12 V,
no external spike to fight), 2 A, 2 MHz, integrated 1 µH inductor, output adjustable 0.8–5.5 V, VFB=0.798 V.

**LOCKED reference — 12 V→5 V (MPM3620A datasheet Fig 10 + Table 1 row 12/5); exact caps from EVM3620 BOM:**
- U1 = MPM3620A (C529064), QFN-20
- C1 input = **10 µF 0805 25 V X5R** (muRata GRM21BR61E106KA73L), tight to IN(16)/PGND, short+wide
- C2 output = **22 µF 0805 16 V X5R** (muRata GRM219R61C226ME15L), at OUT(7,8,9)
- R1 FB-top = **100 kΩ** 0402 1% (OUT↔FB); R2 FB-bot = **19.1 kΩ** 0402 1% (FB↔AGND, at FB, no vias)
  → Vout = 0.798×(1+100/19.1) = **4.98 V**
- R3 EN pull-up = **100 kΩ** 0402 (VIN↔EN17, always-on; internal 6.5 V zener clamp + 870 k pulldown)
- Cf = **none** (NS for 5 V)
- **PG(18): LEAVE FLOATING** (skip pull-up) — PG would tell the ESP "5 V good," but the ESP is powered BY
  that 5 V so it can't report the rail's own failure. Useless here.
- VCC(2), BST(11): **NO external cap** (both integrated). SW(4,5,6): large copper plane (thermal), no part
  (optional 5.6 Ω+330 pF snubber for EMI — skip). NC(10,15,19,20): **MUST float**. AGND(3)+PGND(12,13,14)→GND+vias.
- **Whole buck = module + 5 parts** (C1,C2,R1,R2,R3).

**⚠️ VARIANT TRAP:** the **EVM3620-Q-00B eval board is the MPM3620 (non-A)** — its **pin 18 = AAM** (power-save
select, 56 k to GND). Our **MPM3620A pin 18 = PG**. Use the EVM only for the exact cap P/Ns + layout; follow
the MPM3620**A** datasheet Fig 10 for pin 18 and the FB resistors. Do NOT copy the EVM's 56 k-to-GND on pin 18.

**Still applies from the LM2596 work:** the **star-rail rule** — coin acceptor (J1) + LED panel (J2) branch off
the bulk/fuse node, NOT the module's IN pin (physics is chip-independent; see the shared-12V-rail note below).

### ✅ Spike test — VALIDATED (ngspice, 2026-07-31): the redesign kills the input spike
Ran the SAME VIN-spike test that condemned the LM2596, on both designs (sources in `kicad/sim/`):

| at the switcher IN pin | LM2596 (old — killed chips) | MPM3620A (new) |
|---|---|---|
| peak | **30.3 V** | **12.1 V** |
| low  | **−7.3 V** | **11.8 V** |
| swing | 37 V ring | **0.24 V ripple** |

The LM2596 rang 30 V → −7 V every switching cycle — and 30 V already exceeds the MPM3620A's **28 V
abs-max**, so that loop would kill the module too. But the MPM3620A IN pin just sits at a clean 12 V:
the switch, inductor, and input cap are all **on-die**, so the external pin never sees the switching
di/dt. The failure mechanism is gone; C1's ~2 mm placement is more than adequate.

Behavioral model (internal input cap ~4.7 µF, switch current ~1.2 A estimated) — but the result is
**structural**, not value-dependent: an on-die input cap absorbs the di/dt regardless. Output voltage is
just the divider: 0.798×(1+100/19.1) = **4.98 V**. Files: `kicad/sim/ng_ref_rail.cir` (old baseline),
`kicad/sim/ng_mpm3620.cir` (new). Run: `ngspice -b <file>` (full sim workspace: `~/Documents/ella-sim/`).

### ✅ Whole-board validation suite (2026-08-07) — `kicad/sim/board/`, run `./run_all.sh`
Three layers, all must pass before ordering:
1. **`check_polarity.py`** — pad-1 net vs datasheet intent for every polarized part. This is the check
   ERC/DRC/parity structurally can't do (they only compare project files to each other) — it caught **D1
   installed backwards** (band on GND → would have clamped the coin line at 0.7 V forever; coin input dead).
2. **`check_sim_sync.py`** — fresh netlist export vs the SPICE board model, component-by-component. Kills
   silent sim/schematic drift. If it fails after a schematic edit: fix `board.inc` + NETMAP first.
3. **11 ngspice benches + 60-trial Monte Carlo** — power-up/WiFi bursts, coin pulse, coin-wire noise+short,
   Q1 enable channel, inductive kick, hot-plug ring, reverse polarity, polyfuse trip, opto aging, 3-wire
   panel, tolerances. Models: averaged MPM3620A (~40 kHz loop), thermal-trip polyfuse, CTR-settable FOD817.

**Defects found by the suite and FIXED (schematic + PCB, no routing changes beyond D4/R4 move):**
- **D4 reverse voltage**: 7 V continuous across a 5 V-max LED whenever Q1 off → moved D4+R4 to the gate side
  (GPIO14→D4→R4→GND). Bonus: D4 now clamps GPIO14 to ~2.1 V during boot if a 3-wire panel with a 12 V pullup
  is attached — the pin's safety against such panels DEPENDS on D4 being present.
- **Reverse polarity killed the buck**: old bidirectional D6 (1.5SMC18CA) did nothing until ±20 V → −12 V on
  the MPM3620A VIN. Swapped to **unidirectional 1.5SMC18A** (band toward the 12 V rail): clamps at ~−0.8 V,
  F1 trips (~2.7 A). BOM fields updated (Bourns, `1.5SMC18ABB-ND`).
- **D1 backwards** (see above) — flipped; cathode band now on the coin line.
- **Opto CTR margin**: opto had to sink pullup + green LED ≈ 8.3 mA; bottom-of-bin (CTR 50 %) or aged FOD817s
  left GPIO4 at 0.83–0.88 V > the 0.825 V logic-low ceiling → ~5 % flaky new builds (Monte Carlo). Fix:
  **R1 1 k→2.2 k, R5 220→1 k** → clean 0.05 V low at CTR 50 %, 0.20 V at CTR 30 %. NOT 4.7 k: larger pullup
  recovers too slowly from coupled noise bursts (dip 0.40 V = false-low). Lesson: the checked-in benches
  (0.1 µs step) are the truth — a coarse-step what-if "validated" 4.7 k by aliasing the 2 MHz burst.
- **J2.2 3-wire panel verdict** (closes the globals.h TODO): 3.3 V-logic-safe incl. 12 V-pullup panels
  (D4 clamp); but 3.3 V drive is only marginal into a 10 k-series NPN panel input (active, not saturated) —
  fine into ~1 k inputs. Measure the panel before relying on J2.2 logic drive.
- Sustained coin-wire 12 V short: F1 trips at ~0.6 s; D1 runs ~45 W meanwhile and may sacrifice itself
  (TVS fails short → trips F1 faster; GPIO stays protected). Acceptable — requires a wiring fault.

### Library artifacts (in `kicad/`, verified present 2026-07-23)
| File | State |
|---|---|
| `MPM3620A.kicad_sym` | symbol, **all 20 pins** at distinct positions (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20) |
| `MPM3620A.pretty/QFN-20_L5.0-W3.0-TL-MPM3630GQV-Z.kicad_mod` | footprint — matches what the schematic references |
| `MPM3620A.3dshapes/…​.step` + `.wrl` | 3D model |
| `sym-lib-table` / `fp-lib-table` | both registered as lib **`MPM3620A`** via `${KIPRJMOD}/…` (project-relative → portable) |

The easyeda2kicad import produced a **broken symbol** first time (SW 4,5,6 and OUT 7,8,9 stacked/missing);
the current `.kicad_sym` is the hand-corrected 20-pin version. **If regenerating, re-check every pin** —
`easyeda2kicad --full --lcsc_id=C529064`. Filename says `MPM3630` (upstream EasyEDA naming) — the part is
correct MPM3620A/C529064; don't "fix" the filename or the schematic's footprint link breaks.

### Schematic = DONE. PCB = NOT YET UPDATED (this is the remaining buck work)
As-built designators are **U3, C1, C2, R7, R8, R9** (see the rev-C section near the top of this file for the
full map + verified nets). The schematic buck is wired and ERC-clean.

The **PCB still holds the old LM2596 layout** — it contains `U3 = LM2596T-5.0/NOPB` plus the discrete parts
`C3`, `D2`, `L1`, and has no `R7/R8/R9`. So **Update PCB from Schematic (F8)** will:
- **swap U3's footprint** → QFN-20 MPM3620A
- **delete C3, D2, L1** (module integrates the inductor + catch diode — they no longer exist in the schematic)
- **add R7, R8, R9** (FB divider + EN pull-up)

Then place/route per the rules above: C1 tight to IN(16)/PGND short+wide, SW(4,5,6) as a copper thermal
plane, AGND/PGND vias down, star 12 V rail.

## LM2596 buck — spec limits, TI reference, SMD redesign & sims (2026-07)

### Spec limits (TI LM2596 datasheet SNVS124G)
- **VIN absolute max = 45 V** (§7.1) — HARD ceiling. Footnote: exceeding may cause permanent damage;
  there is **no transient/brief-spike carve-out**, so the *peak* of the switching spike must stay < 45 V.
- **Recommended operating supply = 4.5–40 V** (§7.3). FB pin abs max 25 V; SD/SS 6 V; Tj max 150 °C.
- **Pass/fail for the whole buck-input design = VIN spike peak < 45 V (ideally steady rail < 40 V).**

### TI reference design (fixed 5 V; §9 + Fig 9-16)
- CIN 470–680 µF/50 V low-ESR alu; COUT 220–330 µF/35 V; L 33–47 µH; D 1N5824/1N5825 (5 A Schottky).
  FB→Vout; ON/OFF→GND. This is a **3 A** reference — scale parts down for lighter loads (below).
- Layout rules: input cap within a few mm of VIN/GND; **HF ceramic right at the pin**; catch diode tight
  to OUT(pin2)/GND(pin3); short wide high-current traces; ground plane; **switch-node copper kept SMALL**
  (the one "don't pour" exception).
- **Load-scaling (documented, for lighter loads):** input-cap RMS rating ≈ ½ load current (§9.2.2.6);
  input-cap voltage ≥ 1.5×Vin (→ 25 V fine for 12 V in); catch diode ≥ 1.3×load (§9.2.2.5); inductor from
  the nomograph by load current; output cap by OUTPUT VOLTAGE (Table 9-6), acceptable range 82–820 µF.

### This board's final SMD buck (chosen for ~1 A load)
- U3 = LM2596**S**-5 (TO-263, tab=GND=heatsink→needs GND pour under it); D2 = SS54 (5 A/40 V, SMC);
  L1 = 47 µH (SMD 12×12); C1 = 220 µF/25 V (in); C2 = 220 µF/25 V (out); C3 = 10 µF X7R 1210 (ceramic at
  VIN). **Whole board converted to SMD** except the ESP socket (J4/J5, see [[ellafi-j4j5-esp-socket]]) + U2.
  SW1 → 1×03 SMD header + jumper.

### Simulation results (ngspice behavioral LM2596; files in ~/Documents/ella-sim)
- OLD scattered board (65 mm input cap, no ceramic): **VIN ≈ 80 V > 45 V** → over abs-max → matches the
  2-of-6 dead chips.
- NEW SMD reference (ceramic at pin ~1.5 nH): **VIN ≈ 30 V** — under 45 V AND under the 40 V recommended;
  ~15 V margin.
- **Ceramic-to-VIN inductance is THE dominant lever:** 0.5 nH→22 V, 1.5 nH→30 V, 7 nH→46 V (over!). So C3
  MUST sit tight to U3 pin1/pin3, routed short+wide. Cap value / bulk-cap distance / inductor value barely move it.
- **Absolute volts run PESSIMISTIC** — a *working* eBay module models at 64–70 V yet survives for years;
  the "14 V eBay-with-ceramic" was an idealization (ceramic at 0 nH; a real 1210 has ~1–1.5 nH internal ESL,
  floor ~22 V). **Read sims as RELATIVE, not absolute; the real board is likely lower.** A scope on VIN is
  the only true proof. (Also: hot-plug at 12 V is benign — bulk cap absorbs it, VIN stays ~12 V.)

### Shared 12-V-rail rule (sim-validated) — protects the coin acceptor + LED panel
- J1 (coin acceptor +12 V) and J2 (LED panel +12 V) sit on the SAME net as U3 VIN (`Net-(D6-A2)`).
- The 30 V spike is **local to U3's VIN pin** (it's the switching-current source). The bulk cap C1 traps it —
  the rail AT C1 is clean (~12 V, ~1 Vpp).
- **STAR-route the 12 V rail at the bulk-cap / fuse node:** branch J1/J2 off C1/fuse, **NOT** off U3's VIN pin.
  Sim: star → acceptor sees ~1 Vpp; daisy (tapped at VIN) → 7.4 Vpp (8.4–15.9 V) which can sag/glitch the
  acceptor. (Cable inductance keeps it off the full 30 V either way; the daisy *sag to 8 V* is the real risk.)
- **U3's VIN can't be cleaned like a passive node — it's the source of the pulses.** Only lever = low-L
  ceramic at the pin (contain it), then keep every other 12 V load at the quiet star node.
- Status LEDs D3/D4/D5 run off the **5 V output** (filtered by L1/C2) — they never see the VIN spike.
- **Fuse (F1) position is order-free** (series DC overcurrent protection; just be upstream) — that's a
  DIFFERENT axis from the cap/tap-order (AC-noise) rule above. Don't conflate them.
