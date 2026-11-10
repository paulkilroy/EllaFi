#!/usr/bin/env python3
"""Per-interval download to the iPhone at both shaper ports, from an iphone_record.sh run (headers counted).
  in = router side (eno1), out = to AP (enp4s0); held/dropped = in - out (data the shaper still had queued, or dropped)
Usage: iphone_timeline.py <run dir> [step seconds]"""
import subprocess, sys
D, STEP, IP = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 20, "192.168.0.108"


def series(port):
    out = subprocess.run(["tshark", "-r", f"{D}/ip_{port}.pcap", "-Y", f"ip.dst=={IP}", "-T", "fields",
                          "-e", "frame.time_epoch", "-e", "ip.len"], capture_output=True, text=True).stdout
    # ICMP errors carry a second, inner ip.len: take the outer one
    return [(float(t), int(n.split(",")[0])) for t, n in (l.split("\t") for l in out.splitlines() if l)]


a, b = series("eno1"), series("enp4s0")
t0 = min(a[0][0], b[0][0])
n = int((max(a[-1][0], b[-1][0]) - t0) // STEP) + 1
ins, outs = [0] * n, [0] * n
for t, x in a:
    ins[int((t - t0) // STEP)] += x
for t, x in b:
    outs[int((t - t0) // STEP)] += x
print(f"{'from':>5} {'in MB':>6} {'out MB':>6} {'in Mbps':>8} {'out Mbps':>8} {'in-out MB':>9}")
for i in range(n):
    print(f"{int(i * STEP):>4}s {ins[i] / 1e6:6.2f} {outs[i] / 1e6:6.2f} {ins[i] * 8 / STEP / 1e6:8.2f} "
          f"{outs[i] * 8 / STEP / 1e6:8.2f} {(ins[i] - outs[i]) / 1e6:9.2f}")
print(f"total {sum(ins) / 1e6:.1f} in, {sum(outs) / 1e6:.1f} out, {100 * (1 - sum(outs) / sum(ins)):.1f}% not passed on")
