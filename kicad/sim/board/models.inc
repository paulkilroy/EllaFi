* Behavioral part models for the whole-board sim
* Everything here is a stand-in for a real part, tuned to its datasheet numbers.

* ---------- LEDs (Vf ~1.8/2.0/2.1 V at 5-10 mA) ----------
.model DLED_RED D(IS=1e-20 N=1.69 RS=2)
.model DLED_YEL D(IS=1e-20 N=1.87 RS=2)
.model DLED_GRN D(IS=1e-20 N=1.97 RS=2)

* ---------- TVS diodes ----------
* P6SMB6.8A (D1, coin line): unidirectional, standoff 6.8V, breakdown ~7.0V
.model TVS68 D(IS=1e-12 N=1 BV=7.0 IBV=1m RS=0.25)
* 1.5SMC18A (D6, 12V rail): UNIdirectional, standoff 18V, breakdown ~20V,
* forward-conducts on reversed input (that's the reverse-polarity protection)
.model TVS18UNI D(IS=1e-12 N=1 BV=20 IBV=1m RS=0.02)

* ---------- Q1 AOD4184A (datasheet: Vgs(th) 1.1-2.1V, Rds(on) 8mR @10V / 10.5mR @4.5V) ----------
.model AOD4184A VDMOS(Rg=2 Vto=1.7 Kp=45 Lambda=0.005 Rd=5m Rs=2m
+ Cgdmax=1.5n Cgdmin=0.1n Cgs=2.5n Cjo=1n Is=5e-10 mtriode=0.4)
* Drain-source avalanche clamp (VDMOS has no breakdown): BVdss min 40V, real parts ~44V
.model DAVAL D(IS=1e-14 BV=44 IBV=1m RS=0.2)

* ---------- U1 EL357N(B)(TA)-G optocoupler (A K C E), ~3us response ----------
* Replaces the FOD817 (2026-08-31): compact SOP-4, identical pinout (1=A 2=K
* 3=E 4=C per datasheet), LED reverse rating 6V (same as 817 — coin-fault margin
* unchanged), LED Vf ~1.2V (same GaAs IR emitter). The (B) CTR bin is 100-200%
* (1.0-2.0 here) vs the 817's wide 50-600% — HIGHER, so the coin logic-low only
* gets stronger. Subckt name kept as FOD817 (drop-in); FOD817_CTR is the swept CTR.
.param FOD817_CTR=1.5
.subckt FOD817 A K C E
DLED A x DIRLED
VSENS x K 0
* first-order lag ~3us models the phototransistor response
BLAG nl 0 V = {i(VSENS)}
RLAG nl nf 1k
CLAG nf 0 3n
* output: Ic = CTR * If, saturating like Vce(sat) ~0.2V at 10mA drive
BOUT C E I = {v(C,E) > 0 ? min(FOD817_CTR*max(v(nf),0), v(C,E)/20) : 0}
CCE C E 10p
.model DIRLED D(IS=1e-20 N=1.12 RS=0.6)
.ends

* ---------- U3 MPM3620A, averaged model (no 2MHz switching; loop BW ~25kHz) ----------
* The switching-level input-spike behavior is covered separately by ../ng_mpm3620.cir.
* Regulates FB to 0.798V, soft-start ~1ms, draws constant-power input current at 90% eff.
.subckt MPM3620A_AVG VIN EN VOUT FB
* enable comparator -> soft-start ramp (RC, ~1ms to full reference)
BEN ssin 0 V = {v(EN) > 1.2 ? 0.85 : 0}
RSS ssin ssr 300
CSS ssr 0 1u
* error amp: gain 200, dominant pole 125Hz -> loop crossover ~40kHz with 22uF out,
* giving realistic ~50mV droop on a 300mA step (was tuned unrealistically stiff)
BEA ea 0 V = {200*(min(v(ssr),0.798) - v(FB))}
REA ea eaf 1k
CEA eaf 0 1.27u
* output stage: x10, clamped between 0 and 98% duty of VIN; 20mR effective output R
BOUTV nx 0 V = {min(max(v(eaf)*10, 0), max(v(VIN),0)*0.98)}
RO nx VOUT 0.02
* constant-power input draw at 90% efficiency (positive current drains VIN)
GIN VIN 0 cur = {max(0, v(VOUT)*v(nx,VOUT)/0.02) / (max(v(VIN),2)*0.90)}
.ends

* ---------- ESP32-S3 devkit in the socket: onboard LDO (5V->3V3) + idle draw ----------
* WiFi TX burst current is injected by the testbench on P3V3.
.subckt ESP32MOD VIN5 V33
BLDO nld 0 V = {min(3.3, max(v(VIN5)-0.25, 0))}
RLDO nld V33 0.1
CV33 V33 0 10u
* ~80mA idle (fades out below 2V so startup converges)
BIDLE V33 0 I = {v(V33) > 2 ? 80m : v(V33)*40m}
* LDO input current mirrored onto the 5V pin
BLIN VIN5 0 I = {max(v(nld,V33)/0.1, 0)}
.ends
