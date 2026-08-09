#!/usr/bin/env python3
"""
Incremental, never-overwrite backup of Omada voucher groups.

Why this exists: deleting a voucher group on the controller destroys its entire
per-voucher history (who redeemed what, when). A single full backup goes stale the
moment vouchers keep selling. This script keeps history safe two ways:

  1. SNAPSHOTS  - every run writes a NEW timestamped full snapshot under
                  voucher_backups/snapshots/. Nothing is ever overwritten, so each
                  point-in-time state (including groups you later delete) is kept.

  2. ARCHIVE    - voucher_backups/archive.json is rebuilt each run as the UNION of
                  every snapshot ever taken plus the original full backup. Groups
                  that vanish from the controller (deleted) STAY in the archive with
                  their last-seen per-code state. Per code, the most recent state
                  wins. This is the durable record that survives deletion.

Run it BEFORE deleting any group. It only reads the controller and writes local
files under voucher_backups/ (gitignored) - it never modifies the controller.

Usage:
  ~/.platformio/penv/bin/python3 scripts/voucher_backup.py [--env NAME] [--site NAME]
"""
import argparse
import glob
import json
import os
import sys
from datetime import datetime

import requests
from urllib3.exceptions import InsecureRequestWarning
requests.packages.urllib3.disable_warnings(InsecureRequestWarning)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BACKUP_DIR = os.path.join(REPO, "voucher_backups")
SNAP_DIR = os.path.join(BACKUP_DIR, "snapshots")
ARCHIVE = os.path.join(BACKUP_DIR, "archive.json")
PAGE = 1000  # codes per page when pulling group detail


def load_config(env_override=None):
    cfg = json.load(open(os.path.join(REPO, "data", "config.json")))
    base = {k: v for k, v in cfg.items() if k != "environments"}
    env = env_override or cfg.get("active_env")
    merged = {**base, **cfg.get("environments", {}).get(env, {})}
    return merged, env


def login(session, base_url, cid, user, pw):
    r = session.post(f"{base_url}/{cid}/api/v2/login",
                     json={"username": user, "password": pw}, verify=False, timeout=20)
    j = r.json()
    if j.get("errorCode") != 0:
        sys.exit(f"login failed: {j.get('errorCode')} {j.get('msg')}")
    return {"Csrf-Token": j["result"]["token"]}


def resolve_site(session, base_url, cid, headers, want_name, fallback):
    """Discover the site id from the controller; fall back to a known id."""
    try:
        r = session.get(f"{base_url}/{cid}/api/v2/sites",
                        headers=headers, params={"currentPage": 1, "currentSize": 100},
                        verify=False, timeout=20)
        sites = r.json().get("result", {}).get("data", [])
        if want_name:
            for s in sites:
                if s.get("name") == want_name:
                    return s["id"], s.get("name")
        if sites:
            return sites[0]["id"], sites[0].get("name")
    except Exception as e:
        print(f"  (site discovery failed: {e})")
    if fallback:
        return fallback, "(fallback)"
    sys.exit("could not resolve a site id")


def get_json(session, url, headers, params):
    return session.get(url, headers=headers, params=params, verify=False, timeout=30).json()


def pull_groups(session, base_url, cid, site, headers):
    """List all groups, then pull full paginated code detail for each."""
    gurl = f"{base_url}/{cid}/api/v2/hotspot/sites/{site}/voucherGroups"
    summaries, page = [], 1
    while True:
        res = get_json(session, gurl, headers, {"currentPage": page, "currentSize": 100}).get("result", {})
        summaries += res.get("data", [])
        if len(summaries) >= res.get("totalRows", len(summaries)) or not res.get("data"):
            break
        page += 1

    groups = []
    for g in summaries:
        gid = g["id"]
        detail, codes, page = None, [], 1
        while True:
            res = get_json(session, f"{gurl}/{gid}", headers,
                           {"currentPage": page, "currentSize": PAGE}).get("result", {})
            if detail is None:
                detail = {k: v for k, v in res.items() if k != "data"}
            codes += res.get("data", [])
            total = res.get("totalRows", len(codes))
            if len(codes) >= total or not res.get("data"):
                break
            page += 1
        detail["data"] = codes
        if len(codes) != detail.get("totalCount", len(codes)):
            print(f"  WARN {g.get('name')}: pulled {len(codes)} codes but totalCount={detail.get('totalCount')}")
        groups.append(detail)
        print(f"  {g.get('name'):38} {len(codes):>4} codes")
    return groups


def write_snapshot(meta, groups):
    os.makedirs(SNAP_DIR, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = os.path.join(SNAP_DIR, f"voucher_snapshot_{ts}.json")
    n = 1
    while os.path.exists(path):  # never overwrite
        path = os.path.join(SNAP_DIR, f"voucher_snapshot_{ts}_{n}.json"); n += 1
    doc = {**meta, "fetchedAt": datetime.now().isoformat(),
           "groupCount": len(groups), "groups": groups}
    json.dump(doc, open(path, "w"))
    return path, doc


def iter_sources():
    """All historical backup docs, oldest first, for archive rebuild."""
    files = []
    # original full backup(s) at the top level
    files += glob.glob(os.path.join(BACKUP_DIR, "voucher_groups_FULL_*.json"))
    files += glob.glob(os.path.join(SNAP_DIR, "voucher_snapshot_*.json"))
    docs = []
    for f in files:
        try:
            d = json.load(open(f))
            docs.append((d.get("fetchedAt", ""), f, d))
        except Exception as e:
            print(f"  (skip unreadable {os.path.basename(f)}: {e})")
    docs.sort(key=lambda t: t[0])
    return docs


def rebuild_archive(newest_group_ids):
    """Union of every source; per code latest state wins; deleted groups preserved."""
    arch = {}
    sources = iter_sources()
    for fetched, fname, doc in sources:
        for g in doc.get("groups", []):
            gid = g["id"]
            a = arch.setdefault(gid, {"codes": {}, "_firstSeen": fetched, "_seenIn": []})
            a.update({k: v for k, v in g.items() if k != "data"})  # latest meta wins
            a["_lastSeen"] = fetched
            a["_seenIn"].append(os.path.basename(fname))
            a["_firstSeen"] = min(a["_firstSeen"], fetched) if a["_firstSeen"] else fetched
            for c in g.get("data", []):
                a["codes"][c["id"]] = c  # latest state wins (chronological order)
    for gid, a in arch.items():
        a["totalCodesArchived"] = len(a["codes"])
        a["deletedFromController"] = gid not in newest_group_ids
    json.dump({"builtAt": datetime.now().isoformat(),
               "sourceCount": len(sources), "groups": arch}, open(ARCHIVE, "w"))
    return arch, len(sources)


def previous_snapshot_before(path):
    snaps = sorted(glob.glob(os.path.join(SNAP_DIR, "voucher_snapshot_*.json")))
    snaps = [s for s in snaps if s != path]
    prev = snaps[-1] if snaps else None
    if not prev:  # fall back to the original full backup
        full = sorted(glob.glob(os.path.join(BACKUP_DIR, "voucher_groups_FULL_*.json")))
        prev = full[-1] if full else None
    return prev


def report_delta(new_doc, prev_path):
    if not prev_path:
        print("\n(no earlier backup to diff against — this is the baseline)")
        return
    prev = json.load(open(prev_path))
    pu = {g["id"]: g.get("unusedCount") for g in prev.get("groups", [])}
    print(f"\nActivity since {os.path.basename(prev_path)} ({prev.get('fetchedAt','?')[:10]}):")
    any_move = False
    for g in new_doc["groups"]:
        b = pu.get(g["id"])
        if b is None:
            print(f"  + NEW group {g.get('name')}  ({g.get('unusedCount')} unused)"); any_move = True; continue
        consumed = b - g.get("unusedCount", b)
        if consumed:
            any_move = True
            print(f"  {g.get('name'):38} consumed {consumed:>4}   ({b} -> {g.get('unusedCount')} unused)")
    for gid in pu:
        if gid not in {g["id"] for g in new_doc["groups"]}:
            print(f"  - group {gid} no longer on controller (kept in archive)")
    if not any_move:
        print("  no change")


COMMISSION_RATE_PERCENT = 20  # flat rate — keep in sync with globals.h


def archive_to_rollup(arch):
    """Convert the archive into the device's moneyImport schema (groups only —
    income months are recomputed on-device by the nightly rollup; omitting the
    income key means moneyImport preserves the device's stored values)."""
    groups = {}
    for gid, a in arch.items():
        name = a.get("name", "")
        if not name.startswith("AUTOGEN: "):
            continue
        rest = name[9:]
        seller = rest.split(" ", 1)[1] if " " in rest else ""
        price = 0
        try:
            price = int(json.loads(a.get("description", "") or "{}").get("price", 0))
        except (ValueError, TypeError):
            pass
        total = a.get("totalCount", a.get("totalCodesArchived", 0))
        used = total - a.get("unusedCount", 0)
        month = datetime.fromtimestamp(a.get("createdTime", 0) / 1000).strftime("%Y-%m")
        groups[gid] = {"name": name, "seller": seller, "month": month,
                       "used": used, "price": price, "rate": COMMISSION_RATE_PERCENT,
                       "seen": int(datetime.now().timestamp()),
                       "deleted": a.get("deletedFromController", False)}
    return {"groups": groups}


def push_rollup(rollup, host, admin_pw):
    """POST the rollup to the device (same challenge-response login as deploy.py)."""
    import hashlib
    base = f"http://{host}"
    s = requests.Session()
    nonce = s.get(f"{base}/admin/challenge", timeout=8).json()["nonce"]
    digest = hashlib.sha256(f"{nonce}:{admin_pw}".encode()).hexdigest()
    r = s.post(f"{base}/admin/login", json={"nonce": nonce, "digest": digest}, timeout=8)
    if not (r.ok and r.json().get("ok")):
        sys.exit("device admin login failed")
    r = s.post(f"{base}/admin/money/import", data=json.dumps(rollup), timeout=15)
    print(f"import -> HTTP {r.status_code}: {r.text}")
    r.raise_for_status()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--env"); ap.add_argument("--site")
    ap.add_argument("--rollup", action="store_true",
                    help="after the backup, emit the moneyImport rollup JSON (voucher_backups/rollup.json)")
    ap.add_argument("--push", metavar="HOST", nargs="?", const="ellafi.local", default=None,
                    help="POST the rollup to the device's /admin/money/import (implies --rollup)")
    args = ap.parse_args()

    cfg, env = load_config(args.env)
    base_url = cfg["omada_url"].rstrip("/")
    cid = cfg["omada_controller_id"]
    user, pw = cfg["omada_username"], cfg["omada_password"]
    fallback_site = None
    fulls = sorted(glob.glob(os.path.join(BACKUP_DIR, "voucher_groups_FULL_*.json")))
    if fulls:
        fallback_site = json.load(open(fulls[-1])).get("siteId")

    print(f"env={env}  controller={base_url}")
    s = requests.Session()
    H = login(s, base_url, cid, user, pw)
    site, site_name = resolve_site(s, base_url, cid, H, args.site, fallback_site)
    print(f"site={site_name} ({site})\npulling groups...")

    groups = pull_groups(s, base_url, cid, site, H)
    meta = {"controller": base_url, "omadacId": cid, "siteId": site, "env": env,
            "note": "Full detail: all group fields + every code. Never overwrites."}
    snap_path, doc = write_snapshot(meta, groups)
    total_codes = sum(len(g.get("data", [])) for g in groups)
    print(f"\nsnapshot -> {os.path.relpath(snap_path, REPO)}  ({len(groups)} groups, {total_codes} codes)")

    prev = previous_snapshot_before(snap_path)
    report_delta(doc, prev)

    arch, nsrc = rebuild_archive({g["id"] for g in groups})
    deleted = [a for a in arch.values() if a["deletedFromController"]]
    print(f"\narchive -> {os.path.relpath(ARCHIVE, REPO)}  ({len(arch)} groups from {nsrc} sources)")
    if deleted:
        print(f"  {len(deleted)} deleted group(s) preserved in archive:")
        for a in deleted:
            print(f"    • {a.get('name')}  ({a['totalCodesArchived']} codes, last seen {a.get('_lastSeen','?')[:10]})")

    if args.rollup or args.push is not None:
        rollup = archive_to_rollup(arch)
        rollup_path = os.path.join(BACKUP_DIR, "rollup.json")
        json.dump(rollup, open(rollup_path, "w"), indent=1)
        print(f"\nrollup -> {os.path.relpath(rollup_path, REPO)}  ({len(rollup['groups'])} AUTOGEN groups)")
        for gid, g in rollup["groups"].items():
            flag = " (deleted)" if g["deleted"] else ""
            print(f"    {g['month']}  {g['name']:38} used {g['used']:>4} x P{g['price']}{flag}")
        if args.push is not None:
            push_rollup(rollup, args.push, cfg["omada_password"])


if __name__ == "__main__":
    main()
