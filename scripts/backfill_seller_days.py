#!/usr/bin/env python3
"""Backfill the sales report's per-seller daily history (the Leader column) from voucher codes.

Every code stores startTime (first use). Under redeemed-as-sold, that IS the sale moment — so the
whole history is reconstructable: archive.json (all groups ever seen incl. deleted, with full code
lists) → day × seller × pesos → POST /admin/money/import {"sellerDays": [...]}.

Only days BEFORE today are sent: the device's nightly rollup journal owns today onward, and the
firmware skips any day already journaled, so re-running is always safe.

Usage: ./backfill_seller_days.py            (refreshes the archive first via voucher_backup.py)
       ./backfill_seller_days.py --no-refresh
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

import requests

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARCHIVE = os.path.join(REPO, "voucher_backups", "archive.json")
TZ_OFFSET = 8 * 3600   # Asia/Manila, no DST — matches the device's controller-local day math
PYTHON = os.path.expanduser("~/.platformio/penv/bin/python3")


def day_of(epoch_sec):
    return int((epoch_sec + TZ_OFFSET) // 86400)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-refresh", action="store_true", help="Skip the voucher_backup.py archive refresh")
    ap.add_argument("--host", default="192.168.1.3")
    args = ap.parse_args()

    if not args.no_refresh:
        print("[1/4] refreshing archive (voucher_backup.py)…")
        subprocess.run([PYTHON, os.path.join(REPO, "scripts", "voucher_backup.py")],
                       check=True, capture_output=True, text=True)

    archive = json.load(open(ARCHIVE))
    groups = archive["groups"] if isinstance(archive.get("groups"), dict) else archive
    today = day_of(time.time())

    seller_days = {}   # day -> seller -> pesos
    n_codes = 0
    for g in groups.values():
        name = g.get("name", "")
        if not name.startswith("AUTOGEN: "):
            continue
        rest = name[9:]
        seller = rest.split(" ", 1)[1] if " " in rest else rest
        try:
            price = json.loads(g.get("description", "{}")).get("price", 5)
        except (ValueError, AttributeError):
            price = 5
        codes = g.get("codes") or g.get("data") or []
        if isinstance(codes, dict):
            codes = codes.values()   # archive keys codes by id; snapshots keep them as lists
        for code in codes:
            st = code.get("startTime") or 0
            if st <= 0:
                continue
            day = day_of(st / 1000)
            if day >= today:
                continue   # today onward belongs to the device's own rollup journal
            seller_days.setdefault(day, {}).setdefault(seller, 0)
            seller_days[day][seller] += price
            n_codes += 1

    days = sorted(seller_days)
    if not days:
        sys.exit("nothing to backfill")
    span = f"{time.strftime('%Y-%m-%d', time.gmtime(days[0]*86400))} .. {time.strftime('%Y-%m-%d', time.gmtime(days[-1]*86400))}"
    print(f"[2/4] {n_codes:,} redeemed codes -> {len(days)} days ({span})")

    cfg = json.load(open(os.path.join(REPO, "data", "config.json")))
    env = cfg.get("environments", {}).get(cfg.get("active_env", ""), {})
    merged = dict(cfg)
    merged.update(env)
    s = requests.Session()
    base = f"http://{args.host}"
    nonce = s.get(f"{base}/admin/challenge", timeout=8).json()["nonce"]
    digest = hashlib.sha256(f"{nonce}:{merged['omada_password']}".encode()).hexdigest()
    r = s.post(f"{base}/admin/login", json={"nonce": nonce, "digest": digest}, timeout=8)
    if not r.json().get("ok"):
        sys.exit(f"login failed: {r.text[:120]}")
    print(f"[3/4] logged in to {base}")

    payload = {"sellerDays": [{"day": d, "sellers": seller_days[d]} for d in days]}
    r = s.post(f"{base}/admin/money/import", json=payload, timeout=30)
    j = r.json()
    if "error" in j:
        sys.exit(f"import failed: {r.text[:200]}")
    print(f"[4/4] device accepted: {j.get('sellerDaysAdded')} day(s) added (already-journaled days skipped)")


if __name__ == "__main__":
    main()
