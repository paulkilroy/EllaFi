* EllaFi carrier board — whole-board SPICE netlist
* Hand-translated 1:1 from the exported KiCad netlist (scratchpad ellafi.net, 2026-08-05).
* Node names follow the KiCad nets:
*   VJACK  = J3 barrel-jack pin (testbench drives this)
*   RAIL12 = Net-(D6-A2), the fused 12V rail
*   P5V/P3V3/COIN/GPIO4/GPIO14/BLINK/J2RET = /5V, /3V3, Net-(D1-A2), /GPIO4, /GPIO14,
*   Net-(D4-K), Net-(J2-Pin_3)
* Testbench must provide: VJACK source, GPIO14 drive, COINCTL/SOLCTL control voltages.

* ---- power input: J3 -> F1 polyfuse -> 12V rail ----
* F1 MINISMDC150F thermal model: 0.11R cold; I^2*R heats Cth (1F=J), Rth 4 cools.
* v(f1th) is stored joules; resistance ramps 0.11R -> ~1kR as it crosses ~2.1J.
* Calibrated: 1.5A hold never trips (0.99J steady), 3A trips in seconds,
* 7A in ~0.4s, 70A in ~4ms — matches the MINISMDC150F/16 curve shape.
RF1 VJACK RAIL12 r = {0.11 + 1000*min(max((v(f1th)-2.0)*10, 0), 1)}
BFH 0 f1th I = {pwr(v(VJACK,RAIL12),2) / (0.11 + 1000*min(max((v(f1th)-2.0)*10, 0), 1))}
CFTH f1th 0 1
RFTH f1th 0 4
* D6 = 1.5SMC18A, UNIdirectional (swapped from 18CA after tb_reverse finding)
DTV6 0 RAIL12 TVS18UNI
C1 RAIL12 0 10u

* ---- buck: R9 enable pullup, U3, FB divider R7/R8, C2 out ----
R9 RAIL12 EN 100k
XU3 RAIL12 EN P5V FB MPM3620A_AVG
R7 P5V FB 100k
R8 FB 0 19.1k
C2 P5V 0 22u

* ---- ESP32 module in socket (J4): 5V in pin 21, makes 3V3 on pins 1/2 ----
XESP P5V P3V3 ESP32MOD

* ---- power LED: 5V -> R3 330 -> D3 red -> GND ----
R3 P5V nd3a 330
D3 nd3a 0 DLED_RED

* ---- coin pulse input: 3V3 -> R2 220 -> opto LED -> COIN line ----
R2 P3V3 nu1a 220
XU1 nu1a COIN GPIO4 0 FOD817
* D1 P6SMB6.8A clamps the coin line (anode GND, cathode COIN)
DTV1 0 COIN TVS68
* opto output: R1 2.2k pullup to 3V3, green activity LED R5/D5
* (R1 1k->2.2k + R5 220->1k, 2026-08-07: low-CTR opto margin without giving up
* noise immunity — 4.7k recovers too slowly from coupled bursts; see README)
R1 GPIO4 P3V3 2.2k
R5 P3V3 nd5a 1k
D5 nd5a GPIO4 DLED_GRN
* ESP32 GPIO4 input pin capacitance
CG4 GPIO4 0 5p

* ---- blinker / coin-enable channel: GPIO14 -> Q1 low-side switch ----
R6 GPIO14 0 10k
MQ1 BLINK GPIO14 0 0 AOD4184A
DAV1 0 BLINK DAVAL
* yellow indicator, gate side (moved off the 12V node): GPIO14 -> D4 -> R4 220 -> GND
D4 GPIO14 nd4k DLED_YEL
R4 nd4k 0 220
* SW1 in "firmware" position: J2RET (screw terminal) -> BLINK (Q1 drain)
RSW1 J2RET BLINK 0.02

* ---- deployment loads ----
* blinker lamp on J2: 12V 1W LED lamp (resistive), screw terminals RAIL12 / J2RET
RLAMP RAIL12 J2RET {LAMPR}
* coin acceptor on J1 (CH-926 class): 12V on pin1, ground RETURN through pin3 = BLINK.
* ~60mA idle, +300mA solenoid kick on accept, open-collector coin output.
* 30cm harness wires modeled as 300nH each.
LW0 RAIL12 accv 300n
LW3 accgnd BLINK 300n
RACC accv accgnd 200
BSOL accv accgnd I = {v(SOLCTL) > 0.5 ? 0.3 : 0}
SCOIN nacoin accgnd COINCTL 0 SWOC
LW2 nacoin COIN 300n
.model SWOC SW(Ron=30 Roff=1e9 Vt=0.5 Vh=0.1)
