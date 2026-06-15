#!/usr/bin/env python3
"""Per-board cost vs run size (1..10), PCBWay (turnkey) vs DigiKey (DIY).
Writes docs/bom_cost.svg (open in a browser) and prints an ASCII version.
Reuses the BOM in bom_cost.py so prices stay in one place."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bom_cost import per_board, ASSEMBLY_FLAT, PCB_CHARGE

PCBWAY_C, DK_BASE, DK_TAR = per_board()
DK = DK_BASE + DK_TAR                      # DigiKey landed /board (flat; tariff already in)
N = list(range(1, 11))

def pcbway(n, duty):                       # components scale; assembly+PCB flat; duty on the lot
    return (PCBWAY_C + (ASSEMBLY_FLAT + PCB_CHARGE) / n) * (1 + duty)

SERIES = [
    ("DigiKey DIY (parts, tariff in)", "#2ecc71", [DK] * len(N)),
    ("PCBWay turnkey — DDP (0% duty)", "#3498db", [pcbway(n, 0.00) for n in N]),
    ("PCBWay turnkey — DDU 25%",       "#e67e22", [pcbway(n, 0.25) for n in N]),
    ("PCBWay turnkey — DDU 50%",       "#e74c3c", [pcbway(n, 0.50) for n in N]),
]

# ── SVG ───────────────────────────────────────────────────────────────────────
W, H, ML, MR, MT, MB = 760, 460, 64, 250, 40, 50
x0, x1, y0, y1 = ML, W - MR, H - MB, MT
YMIN, YMAX = 20, max(max(v) for _, _, v in SERIES) * 1.04
def X(n): return x0 + (n - N[0]) / (N[-1] - N[0]) * (x1 - x0)
def Y(v): return y0 - (v - YMIN) / (YMAX - YMIN) * (y0 - y1)

s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" font-family="sans-serif">']
s.append(f'<rect width="{W}" height="{H}" fill="#0f1420"/>')
s.append(f'<text x="{ML}" y="24" fill="#fff" font-size="16" font-weight="bold">EllaFi coin board — landed cost per board vs run size</text>')
# y grid + labels
yt = 20
while yt <= YMAX:
    s.append(f'<line x1="{x0}" y1="{Y(yt):.1f}" x2="{x1}" y2="{Y(yt):.1f}" stroke="#2a3a55" stroke-width="1"/>')
    s.append(f'<text x="{x0-8}" y="{Y(yt)+4:.1f}" fill="#8aa" font-size="11" text-anchor="end">${yt}</text>')
    yt += 10
# x ticks
for n in N:
    s.append(f'<line x1="{X(n):.1f}" y1="{y0}" x2="{X(n):.1f}" y2="{y0+5}" stroke="#8aa"/>')
    s.append(f'<text x="{X(n):.1f}" y="{y0+20}" fill="#8aa" font-size="11" text-anchor="middle">{n}</text>')
s.append(f'<text x="{(x0+x1)/2:.0f}" y="{H-8}" fill="#cdd7e6" font-size="12" text-anchor="middle">boards in the run</text>')
# series
ly = MT + 4
for name, col, vals in SERIES:
    pts = " ".join(f"{X(n):.1f},{Y(v):.1f}" for n, v in zip(N, vals))
    s.append(f'<polyline points="{pts}" fill="none" stroke="{col}" stroke-width="2.5"/>')
    for n, v in zip(N, vals):
        s.append(f'<circle cx="{X(n):.1f}" cy="{Y(v):.1f}" r="2.5" fill="{col}"/>')
    s.append(f'<rect x="{x1+16}" y="{ly}" width="12" height="12" fill="{col}"/>')
    s.append(f'<text x="{x1+32}" y="{ly+11}" fill="#cdd7e6" font-size="12">{name}</text>')
    s.append(f'<text x="{x1+32}" y="{ly+26}" fill="#8aa" font-size="10">${vals[2]:.0f}/bd @3 · ${vals[-1]:.0f}/bd @10</text>')
    ly += 44
s.append('</svg>')
open(os.path.join(os.path.dirname(__file__), "bom_cost.svg"), "w").write("\n".join(s))
print("wrote docs/bom_cost.svg")

# ── ASCII ─────────────────────────────────────────────────────────────────────
print(f"\nper-board $ (components: PCBWay ${PCBWAY_C:.2f}, DigiKey ${DK:.2f} landed)\n")

# ASCII chart: D=DigiKey  o=PCBWay 0%  +=25%  x=50%
chars = "Do+x"
rows = 22
ymin, ymax = 20, max(max(v) for _, _, v in SERIES)
grid = [[" "] * (len(N) * 4) for _ in range(rows)]
for (name, col, vals), ch in zip(SERIES, chars):
    for j, v in enumerate(vals):
        r = int(round((ymax - v) / (ymax - ymin) * (rows - 1)))
        grid[r][j * 4 + 1] = ch
for r in range(rows):
    lbl = ymax - r / (rows - 1) * (ymax - ymin)
    bar = "".join(grid[r])
    print(f"${lbl:5.0f} |{bar}")
print("       +" + "-" * (len(N) * 4))
print("        " + "".join(f"{n:<4}" for n in N) + "  boards")
print("        D DigiKey   o PCBWay DDP(0%)   + DDU25%   x DDU50%")
