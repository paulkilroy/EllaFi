#!/usr/bin/env python3
"""Summarize the latest ab3 results folder (or the one given): one row per setup, every column, totals for the
3 Pi phones. Omada counter = after - before per phone; a phone whose counter went down (reset mid-run) is left out
and the cell says how many of 3 were usable.
Usage: ab3_summary.py [results dir]"""
import glob, json, os, subprocess, sys
D = sys.argv[1] if len(sys.argv) > 1 else sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), "results/ab3-*")))[-1]
IPS = ("192.168.0.105", "192.168.0.106", "192.168.0.107")


def cap_bytes(pcap):
    if not os.path.exists(pcap):
        return None
    out = subprocess.run(["tshark", "-r", pcap, "-Y", " || ".join(f"ip.dst=={ip}" for ip in IPS), "-T", "fields",
                          "-e", "ip.len"], capture_output=True, text=True).stdout.split()
    return sum(int(x.split(",")[0]) for x in out) / 1e6


def omada(path):
    d = {}
    if os.path.exists(path):
        for line in open(path):
            v = line.split()
            if len(v) > 7:
                d[v[0]] = float(v[7])
    return d


print(f"{D}\n")
print(f"{'setup':<8} {'server sent':>11} {'from internet':>13} {'to AP':>8} {'Omada ctr':>11} {'phones got':>10} "
      f"{'not delivered':>13} {'ready':>6} {'wait med/p90/max s':>19} {'freezes':>8}")
for m in ("open", "omada", "gateway", "shaper"):
    if not os.path.exists(f"{D}/{m}.phones.jsonl"):
        continue
    ph = [json.loads(l) for l in open(f"{D}/{m}.phones.jsonl") if l.strip()]
    got = sum(p["mb_received"] for p in ph)
    srv = int(open(f"{D}/{m}.server.txt").read().split()[1]) / 1e6
    inet, toap = cap_bytes(f"{D}/{m}.internet.pcap"), cap_bytes(f"{D}/{m}.toap.pcap")
    b, a = omada(f"{D}/{m}.omada_before.txt"), omada(f"{D}/{m}.omada_after.txt")
    ok = [a[ip] - b.get(ip, 0) for ip in a if a[ip] - b.get(ip, 0) >= 0]
    octr = f"{sum(ok):.1f}M" + ("" if len(ok) == 3 else f" ({len(ok)}/3)")
    ready = sum(p["ready_at_swipe_pct"] for p in ph) / len(ph)
    med = sorted(p["wait_s"]["median"] or 0 for p in ph)[len(ph) // 2]
    p90 = max(p["wait_s"]["p90"] or 0 for p in ph); mx = max(p["wait_s"]["max"] or 0 for p in ph)
    fr = sum(p["videos_with_freeze"] for p in ph)
    # measured from what the server sent: a gateway limit drops BEFORE the mini PC's recorders
    print(f"{m:<8} {srv:>10.1f}M {inet:>12.1f}M {toap:>7.1f}M {octr:>11} {got:>9.1f}M {100*(1-got/srv):>12.0f}% "
          f"{ready:>5.0f}% {med:>8.2f}/{p90:.2f}/{mx:.2f} {fr:>8}")
print("\nnot delivered = 1 - phones got / server sent. The open row is the baseline: there nothing is dropped, so its"
      "\nfigure is pure overhead (packet headers the server counts and the phones don't); compare the others against it.")
for f in sorted(glob.glob(f"{D}/*.capture_drops.txt")):
    print(os.path.basename(f), " | ".join(l.split(": ")[-1].strip() for l in open(f) if l.strip()))
