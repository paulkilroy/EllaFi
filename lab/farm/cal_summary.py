#!/usr/bin/env python3
"""Summarize calibration runs: data arriving at the AP for the phone (mini PC capture) vs what the phone got.
Usage: cal_summary.py <calibrate dir>..."""
import json, subprocess, sys
from collections import defaultdict
for d in sys.argv[1:]:
    out = subprocess.run(["tshark", "-r", f"{d}/internet.pcap", "-Y", "ip.dst==192.168.0.106", "-T", "fields",
                          "-e", "frame.time_epoch", "-e", "ip.len"], capture_output=True, text=True).stdout.split()
    pk = [(float(out[i]), int(out[i + 1].split(",")[0])) for i in range(0, len(out), 2)]
    t0 = pk[0][0]; sec = defaultdict(int); b10 = defaultdict(int)
    for t, n in pk:
        sec[int(t - t0)] += n; b10[int((t - t0) / 0.01)] += n
    mb = [sec[i] * 8 / 1e6 for i in range(int(pk[-1][0] - t0) + 1)]
    ph = json.load(open(f"{d}/phone.json"))
    arrived = sum(n for _, n in pk) / 1e6
    srt = sorted(mb); v10 = sorted(x * 8 / 0.01 / 1e6 for x in b10.values())
    print(f"{d.split('/')[-1]}: slots {ph['settings']['slots']}")
    print(f"  arrived {arrived:.1f} MB = {arrived*8/len(mb):.2f} Mbps avg; per-second median {srt[len(srt)//2]:.1f}, "
          f"p90 {srt[int(.9*len(srt))]:.1f}, max {srt[-1]:.1f}; 10 ms median {v10[len(v10)//2]:.1f} max {v10[-1]:.1f}")
    print(f"  phone got {ph['mb_received']} MB (watched {ph['mb_watched']}); not delivered "
          f"{100*(1-ph['mb_received']/arrived):.0f}%; ready at swipe {ph['ready_at_swipe_pct']}%, wait median "
          f"{ph['wait_s']['median']} p90 {ph['wait_s']['p90']} max {ph['wait_s']['max']}, first video {ph['first_video_wait_s']} s, "
          f"freezes {ph['videos_with_freeze']}")
    print("  first 40 s:", " ".join(f"{x:.0f}" for x in mb[:40]))
