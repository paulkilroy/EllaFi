#!/usr/bin/env python3
"""Table of every overnight run (all counts include packet headers, so they compare directly).
  not delivered = 1 - phones got / server sent        dropped before AP = server sent - to AP
Usage: overnight_summary.py <overnight dir>"""
import glob, json, os, statistics as st, sys
D = sys.argv[1]
rows = []
for d in sorted(glob.glob(f"{D}/*/")):
    c = {}
    if not os.path.exists(f"{d}counts.txt") or not os.path.exists(f"{d}phones.jsonl"):
        continue
    for line in open(f"{d}counts.txt"):
        v = line.split()
        if v:
            c[v[0]] = int(v[1]) if len(v) > 1 and v[1].lstrip("-").isdigit() else None
    try:
        ph = [json.loads(l) for l in open(f"{d}phones.jsonl") if l.strip()]
    except ValueError:
        continue
    if len(ph) < 3 or not c.get("server_sent"):
        continue
    s = dict(x.split("=") for x in open(f"{d}settings.txt").read().split())
    rows.append({"label": os.path.basename(d.rstrip("/")), "set": s, "srv": c["server_sent"] / 1e6,
                 "inet": (c.get("from_internet") or 0) / 1e6, "toap": (c.get("to_ap") or 0) / 1e6,
                 "got": c["phones_got"] / 1e6, "sdrop": c.get("shaper_dropped") or 0,
                 "ready": st.mean(p["ready_at_swipe_pct"] for p in ph),
                 "wmed": st.median(p["wait_s"]["median"] or 0 for p in ph),
                 "wp90": max(p["wait_s"]["p90"] or 0 for p in ph), "wmax": max(p["wait_s"]["max"] or 0 for p in ph),
                 "frz": sum(p["videos_with_freeze"] for p in ph), "frs": sum(p["freeze_s_total"] for p in ph),
                 "watched": sum(p["mb_watched"] for p in ph), "swipes": sum(p["swipes"] for p in ph)})
print(f"{'run':<26} {'srv MB':>7} {'to AP':>7} {'got':>7} {'not deliv':>9} {'ready':>6} {'wait med/p90/max':>17} "
      f"{'freeze n/s':>11} {'watched':>8}")
for r in rows:
    print(f"{r['label']:<26} {r['srv']:>7.1f} {r['toap']:>7.1f} {r['got']:>7.1f} {100*(1-r['got']/r['srv']):>8.0f}% "
          f"{r['ready']:>5.0f}% {r['wmed']:>6.2f}/{r['wp90']:.2f}/{r['wmax']:>5.2f} {r['frz']:>4}/{r['frs']:>5.1f} {r['watched']:>8.1f}")
# grouped: same settings across passes / repeats
print("\nGROUPED (mean over repeats; label without -a/-b/-c and -p1/-p2)")
g = {}
for r in rows:
    key = r["label"].rsplit("-p", 1)[0]
    key = key[:-2] if key[-2:] in ("-a", "-b", "-c") else key
    g.setdefault(key, []).append(r)
for k, rs in g.items():
    nd = [100 * (1 - r["got"] / r["srv"]) for r in rs]
    print(f"{k:<22} n={len(rs)}  not delivered {st.mean(nd):5.1f}% (range {min(nd):.0f}-{max(nd):.0f})  "
          f"ready {st.mean(r['ready'] for r in rs):5.1f}%  worst wait {max(r['wmax'] for r in rs):5.2f}s  "
          f"freezes {st.mean(r['frz'] for r in rs):4.1f}  got {st.mean(r['got'] for r in rs):6.1f} MB")
