#!/usr/bin/env python3
"""Summarize an ab_scroll.sh results folder: per mode, data that crossed the internet link vs what the phones got.
Usage: ab_summary.py <results dir>"""
import json, os, statistics as st, subprocess, sys
D = sys.argv[1]
PHONES = "ip.dst==192.168.0.105||ip.dst==192.168.0.106||ip.dst==192.168.0.107"
srv = {}
for line in open(f"{D}/server.txt"):
    v = line.split()
    srv.setdefault(v[0], {})[v[1]] = int(v[2]);
    if len(v) > 3: srv[v[0]][v[3]] = int(v[4])


def down_bytes(pcap):
    """Bytes toward the phones (IP length), from a header capture."""
    out = subprocess.run(["tshark", "-r", pcap, "-Y", PHONES, "-T", "fields", "-e", "ip.len"], capture_output=True, text=True).stdout
    return sum(int(x.split(",")[0]) for x in out.split() if x)


print(f"{'mode':<7} {'internet MB':>11} {'to AP MB':>9} {'phones got':>10} {'thrown away':>11} {'srv resent':>10} "
      f"{'start median':>12} {'p90':>5} {'>1s':>5} {'max':>5}")
for m in ("open", "omada", "shape"):
    if not os.path.exists(f"{D}/{m}.phones.jsonl"):
        continue
    ph = [json.loads(l) for l in open(f"{D}/{m}.phones.jsonl") if l.strip()]
    got = sum(p["mb_received"] for p in ph)
    inet = down_bytes(f"{D}/{m}.internet.pcap") / 1e6
    toap = down_bytes(f"{D}/{m}.to_ap.pcap") / 1e6
    s = srv.get(m, {})
    rs = 100 * s.get("server_resent_pkts", 0) / max(s.get("server_sent_pkts", 1), 1)
    med = st.median(p["start_delay_s"]["median"] for p in ph)
    p90 = max(p["start_delay_s"]["p90"] for p in ph)
    o1 = st.mean(p["start_delay_s"]["over_1s_pct"] for p in ph)
    mx = max(p["start_delay_s"]["max"] for p in ph)
    print(f"{m:<7} {inet:>11.0f} {toap:>9.0f} {got:>10.0f} {100*(1-got/inet) if inet else 0:>10.0f}% {rs:>9.0f}% "
          f"{med:>11.2f}s {p90:>4.1f}s {o1:>4.0f}% {mx:>4.1f}s")
