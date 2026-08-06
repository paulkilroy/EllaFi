#!/usr/bin/env python3
"""Polarity checker — the D1 lesson, automated.

Consistency checks (ERC/DRC/parity) can't catch a polarized part installed
backwards when the schematic symbol carries no direction: schematic and board
agree with each other and are both wrong versus the datasheet. This script
closes the loop to the outside world: for every polarized part it asserts which
NET the physical pin-1 pad (the end the assembler aligns the band / polarity
mark to) must land on.

Conventions encoded (KiCad library):
  diode footprints: pad 1 = cathode (band)
  LED footprints:   pad 1 = cathode
  polarized caps:   pad 1 = positive
  SOP-4 opto:       pad 1 = LED anode
  TO-252 FET:       pad 1 = gate

If the schematic is ever rewired on purpose, update INTENT to match the new
intent — the point is that intent lives HERE, reviewed by a human, not
implicitly in pad numbering.
"""
import re, sys, os

PCB = os.path.join(os.path.dirname(__file__), '..', '..', 'EllaFi-PCB.kicad_pcb')

# ref -> { pad : set of acceptable net names }
# Net names may drift when KiCad renames autonets; keep in sync deliberately.
RAIL12 = 'Net-(D6-A1)'   # fused 12V rail
COIN   = 'Net-(D1-A1)'   # coin pulse line
INTENT = {
    # D1 P6SMB6.8A: clamps the coin line -> cathode (band) faces the coin line
    'D1': {'1': {COIN}, '2': {'GND'}},
    # D3 red power LED: 5V -> R3 -> anode; cathode to GND
    'D3': {'1': {'GND'}},
    # D4 yellow enable LED (gate side): anode on GPIO14, cathode toward R4
    'D4': {'1': {'Net-(D4-K)'}, '2': {'/GPIO14'}},
    # D5 green coin LED: anode via R5 from 3V3, cathode on GPIO4
    'D5': {'1': {'/GPIO4'}},
    # D6 1.5SMC18A: 12V rail clamp + reverse-polarity diode -> cathode on rail
    'D6': {'1': {RAIL12}, '2': {'GND'}},
    # C1/C2 polarized: positive terminal on the supply rail
    'C1': {'1': {RAIL12}, '2': {'GND'}},
    'C2': {'1': {'/5V'}, '2': {'GND'}},
    # U1 FOD817S: 1=LED anode (from R2), 2=LED cathode (coin line),
    # 3=emitter (GND), 4=collector (GPIO4)
    'U1': {'1': {'Net-(R2-Pad2)'}, '2': {COIN}, '3': {'GND'}, '4': {'/GPIO4'}},
    # Q1 AOD4184A TO-252: 1=gate, 2=drain (tab), 3=source
    'Q1': {'1': {'/GPIO14'}, '2': {'Net-(J1-Pin_3)'}, '3': {'GND'}},
}

def balanced(s, i):
    d = 0
    for j in range(i, len(s)):
        if s[j] == '(': d += 1
        elif s[j] == ')':
            d -= 1
            if d == 0: return s[i:j+1]
    return s[i:]

def main():
    s = open(PCB).read()
    found = {}
    for m in re.finditer(r'\(footprint\s+"[^"]+"', s):
        blk = balanced(s, m.start())
        r = re.search(r'\(property "Reference" "([^"]+)"', blk)
        if not r or r.group(1) not in INTENT: continue
        pads = {}
        for pm in re.finditer(r'\(pad\s+"([^"]*)"', blk):
            pblk = balanced(blk, pm.start())
            net = re.search(r'\(net\s+"([^"]+)"\)', pblk)
            if net and pm.group(1): pads[pm.group(1)] = net.group(1)
        found[r.group(1)] = pads

    fails = 0
    for ref, want in sorted(INTENT.items()):
        if ref not in found:
            print(f"  FAIL {ref}: footprint not found on board"); fails += 1; continue
        for pad, nets in sorted(want.items()):
            got = found[ref].get(pad)
            if got in nets:
                print(f"  PASS {ref} pad{pad} -> {got}")
            else:
                print(f"  FAIL {ref} pad{pad} -> {got}  (expected {'/'.join(sorted(nets))})")
                fails += 1
    print(f"polarity: {'ALL PASS' if fails == 0 else f'{fails} FAILURES'}")
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
