#!/usr/bin/env python3
"""
EllaFi coin-board cost comparison — PCBWay (turnkey) vs DigiKey (DIY), with the tariff
asymmetry handled honestly. Fill in the three unknowns at the top and run.

    python3 docs/bom_cost.py
    python3 docs/bom_cost.py --qty 10 --duty 0.25          # DDU: 25% US duty on the PCBA
    python3 docs/bom_cost.py --qty 10 --ddp                # DDP: PCBWay prepays duty

Two things that decide the winner (ask PCBWay):
  1. DDP vs DDU — does the quote include US duties/taxes (DDP), or do you pay on arrival (DDU)?
  2. If DDU, the HTS duty RATE for an *assembled PCBA* from China (don't guess — it moves).
"""
import argparse

# ── INPUTS (edit, or override on the CLI) ────────────────────────────────────
QTY        = 6        # boards in the run (PCBWay assembly is flat up to 10)
PCBA_DUTY  = 0.00     # US import duty rate on the finished PCBWay board, e.g. 0.25 (DDU only)
DDP        = False    # True = PCBWay quote already includes duty → ignore PCBA_DUTY
PCB_FLAT   = True     # True = the $5 PCB charge is flat up to 10; False = scales per 6

ASSEMBLY_FLAT = 29.00     # PCBWay assembly, flat up to 10 boards
PCB_CHARGE    = 5.00      # PCBWay bare-PCB charge (per the quote, for ≤6)

# ── BOM ──────────────────────────────────────────────────────────────────────
# (ref, description, qty/board, pcbway_unit, digikey_unit, digikey_tariff_unit)
# ⚠ EDIT rows = the new-design swaps (IRLZ44N / R1→1k / +R6) — confirm live prices.
BOM = [
    ("C1,C2", "100µF 25V",          2, 2.062, 1.03, 0.00),
    ("D1",    "P6KE6.8A TVS",       1, 0.147, 0.57, 0.1995),
    ("D2",    "1N5822 Schottky",    1, 0.657, 0.91, 0.182),
    ("D3",    "LED red 3mm",        1, 0.444, 0.18, 0.09),
    ("D4",    "LED yellow 3mm",     1, 0.444, 0.18, 0.09),
    ("D5",    "LED green 3mm",      1, 0.431, 0.17, 0.085),
    ("J1",    "4-pos socket",       1, 1.006, 0.43, 0.043),
    ("J2",    "Terminal block 3P",  1, 3.629, 1.99, 0.199),
    ("J3",    "Barrel jack",        1, 1.644, 0.70, 0.245),
    ("J4,J5", "22-pos socket",      2, 1.327, 1.41, 0.141),
    ("L1",    "100µH inductor",     1, 3.301, 1.82, 0.91),
    ("Q1",    "IRLZ44N  ⚠EDIT",     1, 0.964, 1.04, 0.10),   # was IRF540N — confirm IRLZ44N price
    ("R1",    "1k  ⚠EDIT",          1, 0.101, 0.10, 0.018),  # was 10k MF0207 — now 1k CFR
    ("R6",    "10k pulldown ⚠NEW",  1, 0.101, 0.10, 0.035),  # NEW gate pulldown
    ("R2,R4,R5", "220Ω",            3, 0.067, 0.10, 0.023),
    ("R3",    "330Ω",               1, 0.101, 0.10, 0.018),
    ("SW1",   "Slide switch SPDT",  1, 0.798, 0.80, 0.28),
    ("U1",    "FOD817 opto",        1, 0.427, 0.40, 0.04),
    ("U3",    "LM2596-5",           1, 4.058, 5.67, 0.567),
]


def per_board():
    pcbway = sum(q * pw for _, _, q, pw, _, _ in BOM)
    dk_base = sum(q * dk for _, _, q, _, dk, _ in BOM)
    dk_tar  = sum(q * t for _, _, q, _, _, t in BOM)
    return pcbway, dk_base, dk_tar


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qty",  type=int,   default=QTY)
    ap.add_argument("--duty", type=float, default=PCBA_DUTY, help="PCBA US duty rate, e.g. 0.25")
    ap.add_argument("--ddp",  action="store_true", default=DDP, help="PCBWay prepays duty")
    a = ap.parse_args()
    n = a.qty
    duty = 0.0 if a.ddp else a.duty

    pcbway_c, dk_base_c, dk_tar_c = per_board()

    # PCBWay: components scale, assembly flat (≤10), PCB flat-ish; duty on the whole landed value.
    pcb = PCB_CHARGE if PCB_FLAT else PCB_CHARGE * (n / 6)
    pcbway_product = pcbway_c * n + ASSEMBLY_FLAT + pcb
    pcbway_landed  = pcbway_product * (1 + duty)

    # DigiKey: parts only, tariff already paid at checkout; you assemble + buy bare PCBs.
    dk_landed = (dk_base_c + dk_tar_c) * n

    print(f"\nEllaFi coin board — {n} boards"
          f"   (duty {'DDP/incl' if a.ddp else f'{duty:.0%}'}, PCB {'flat' if PCB_FLAT else 'scaled'})\n")
    print(f"  per-board components:  PCBWay ${pcbway_c:6.2f}   DigiKey ${dk_base_c:6.2f} + ${dk_tar_c:4.2f} tariff = ${dk_base_c+dk_tar_c:6.2f}\n")
    print(f"  {'':22} {'TOTAL':>10} {'/board':>10}")
    print(f"  PCBWay components      {pcbway_c*n:10.2f} {pcbway_c:10.2f}")
    print(f"  PCBWay assembly (flat) {ASSEMBLY_FLAT:10.2f} {ASSEMBLY_FLAT/n:10.2f}")
    print(f"  PCBWay PCB             {pcb:10.2f} {pcb/n:10.2f}")
    print(f"  PCBWay product         {pcbway_product:10.2f} {pcbway_product/n:10.2f}")
    print(f"  PCBWay + duty (LANDED) {pcbway_landed:10.2f} {pcbway_landed/n:10.2f}   <-- turnkey, assembled")
    print(f"  DigiKey parts (LANDED) {dk_landed:10.2f} {dk_landed/n:10.2f}   <-- DIY: + bare PCBs + your labor")
    delta = pcbway_landed - dk_landed
    print(f"\n  PCBWay is ${abs(delta):.2f} {'MORE' if delta>0 else 'LESS'} than DigiKey "
          f"(${abs(delta)/n:.2f}/board) — for that you get all {n} boards fully assembled.\n")


if __name__ == "__main__":
    main()
