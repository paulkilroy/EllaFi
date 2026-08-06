#!/usr/bin/env python3
"""Sim-sync checker — board.inc must never silently drift from the schematic.

Exports a fresh netlist from the schematic (kicad-cli) and verifies that every
on-board component modeled in board.inc touches exactly the same nets, using
the net-name mapping below. Connectivity only — orientation of polarized parts
is check_polarity.py's job.

If this fails after a schematic edit: update board.inc (and NETMAP if nets were
renamed), re-run the suite, THEN trust the sim again.
"""
import re, subprocess, sys, os, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCH = os.path.join(HERE, '..', '..', 'EllaFi-PCB.kicad_sch')
KICAD_CLI = '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli'

# KiCad net name -> sim node name in board.inc
NETMAP = {
    '+12V': 'vjack', 'Net-(D6-A1)': 'rail12', '/5V': 'p5v', '/3V3': 'p3v3',
    'GND': '0', '/GPIO4': 'gpio4', '/GPIO14': 'gpio14',
    'Net-(D1-A1)': 'coin', 'Net-(D4-K)': 'nd4k', 'Net-(J1-Pin_3)': 'blink',
    'Net-(J2-Pin_3)': 'j2ret', 'Net-(D3-A)': 'nd3a', 'Net-(D5-A)': 'nd5a',
    'Net-(R2-Pad2)': 'nu1a', 'Net-(U3-EN)': 'en', 'Net-(U3-FB)': 'fb',
}

# schematic ref -> sim element name in board.inc; nets the model deliberately
# omits (subckt-internal grounds, unmodeled switch throw) listed as 'allow_missing'
SIMMAP = {
    'R1': 'r1', 'R2': 'r2', 'R3': 'r3', 'R4': 'r4', 'R5': 'r5', 'R6': 'r6',
    'R7': 'r7', 'R8': 'r8', 'R9': 'r9', 'C1': 'c1', 'C2': 'c2',
    'D1': 'dtv1', 'D3': 'd3', 'D4': 'd4', 'D5': 'd5', 'D6': 'dtv6',
    'F1': 'rf1', 'Q1': 'mq1', 'U1': 'xu1',
    'U3': ('xu3', {'0'}),      # module PGND/AGND are subckt-internal
    'SW1': ('rsw1', {'0'}),    # modeled in the firmware (B-A) position only
}

def sim_elements(path):
    els = {}
    for line in open(path):
        line = line.split(';')[0].strip().lower()
        if not line or line.startswith(('*', '.')): continue
        t = line.split()
        name = t[0]
        if name.startswith(('r', 'c', 'l')) and len(t) >= 3:
            els[name] = set(t[1:3])
        elif name.startswith('d') and len(t) >= 3:
            els[name] = set(t[1:3])
        elif name.startswith('m') and len(t) >= 4:
            els[name] = set(t[1:4])          # D G S (bulk=source)
        elif name.startswith('x'):
            els[name] = set(t[1:-1])         # ports (last token = subckt name)
        elif name.startswith(('b', 's', 'v', 'i')) and len(t) >= 3:
            els[name] = set(t[1:3])
    return els

def main():
    with tempfile.NamedTemporaryFile(suffix='.net', delete=False) as f:
        out = f.name
    r = subprocess.run([KICAD_CLI, 'sch', 'export', 'netlist', '--output', out, SCH],
                       capture_output=True, text=True)
    if not os.path.exists(out) or os.path.getsize(out) == 0:
        print(f"  FAIL netlist export: {r.stderr.strip()[:200]}"); return 1
    s = open(out).read().split('(nets')[1]

    ref_nets = {}
    unmapped = set()
    for block in re.split(r'\n\t\t\(net\b', s)[1:]:
        name = re.search(r'\(name "([^"]+)"\)', block).group(1)
        for ref, pin in re.findall(r'\(ref "([^"]+)"\)\s*\(pin "([^"]+)"\)', block):
            if ref not in SIMMAP: continue
            if name.startswith('unconnected-'): continue
            if name not in NETMAP:
                unmapped.add(name); continue
            ref_nets.setdefault(ref, set()).add(NETMAP[name])

    els = sim_elements(os.path.join(HERE, 'board.inc'))
    fails = 0
    if unmapped:
        print(f"  FAIL unmapped nets touching modeled parts: {sorted(unmapped)}")
        fails += 1
    for ref in sorted(SIMMAP):
        entry = SIMMAP[ref]
        el, allow = (entry, set()) if isinstance(entry, str) else entry
        want = ref_nets.get(ref, set()) - allow
        got = els.get(el)
        if got is None:
            print(f"  FAIL {ref}: element '{el}' not found in board.inc"); fails += 1
        elif got - allow != want:
            print(f"  FAIL {ref}/{el}: sim {sorted(got)} vs schematic {sorted(want)}")
            fails += 1
        else:
            print(f"  PASS {ref}/{el}: {sorted(want)}")
    os.unlink(out)
    print(f"sim-sync: {'ALL PASS' if fails == 0 else f'{fails} FAILURES'}")
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
