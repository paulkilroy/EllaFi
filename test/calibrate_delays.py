#!/usr/bin/env python3
"""Derive mock_omada.py DELAYS (p50/p90) from a REAL device serial log.

The firmware brackets every Omada call with two ESP_LOG lines that carry the device millis() timestamp:
a start ("POST/GET <url>") and a finish ("… successful" / "… HTTP N, M bytes"). The gap between them is
the round-trip latency to the controller. This pairs them, buckets by operation, and prints p50/p90 so
the mock's sleeps match what the live cloud controller actually does.

USAGE:
  1. Point a device at the LIVE controller (not the mock) and capture serial through a full coin flow —
     auth, extend, pause (disconnect), resume (re-auth) — and let it run a couple of 60s cache refreshes
     for `query` samples:
        ~/.platformio/penv/bin/python3 -m platformio device monitor -e esp32-s3-devkitc-1-serial \
            | tee live_omada.log
  2. ~/.platformio/penv/bin/python3 test/calibrate_delays.py live_omada.log
  3. Paste the printed DELAYS block into mock_omada.py.
"""
import sys, re
from statistics import quantiles

# device line: "<maybe prefix> I (<millis>) EllaFi: <message>"
LINE = re.compile(r"\((\d+)\)\s+EllaFi:\s*(.*)")

# start line (url) -> op key ; matched by substring in the POST/GET url
START = [
    ("/api/v2/login",   "login"),
    ("/extPortal/auth", "auth"),
    ("/extend",         "extend"),
    ("/disconnect",     "disconnect"),
    ("/user/sites",     "query"),     # loadOmadaSites
    ("/clients",        "query"),     # hotspot + all-clients GETs
]
# finish line (message prefix) -> op key
FINISH = [
    ("Login successful",          "login"),
    ("Authentication successful", "auth"),
    ("Extend successful",         "extend"),
    ("Disconnect successful",     "disconnect"),
    ("Site loaded:",              "query"),
    ("getHotspotClientsJson: HTTP", "query"),
    ("getAllClientsJson: HTTP",     "query"),
]

def op_of(msg, table):
    for needle, key in table:
        if needle in msg:
            return key
    return None

def main():
    if len(sys.argv) < 2:
        sys.exit("usage: calibrate_delays.py <serial-log-file>")
    samples = {k: [] for k in ("login", "auth", "extend", "disconnect", "query")}
    pending = {}  # op -> start_ms (most recent open request of that op)
    for raw in open(sys.argv[1], errors="replace"):
        m = LINE.search(raw)
        if not m:
            continue
        ms, msg = int(m.group(1)), m.group(2)
        if msg.startswith("POST ") or msg.startswith("GET "):
            op = op_of(msg, START)
            if op:
                pending[op] = ms           # newest start wins (drops an unfinished/failed prior one)
            continue
        op = op_of(msg, FINISH)
        if op and op in pending:
            dur = (ms - pending.pop(op)) / 1000.0
            if dur >= 0:
                samples[op].append(dur)

    print(f"# calibrated from {sys.argv[1]}\n# op          n    p50      p90")
    out = {}
    for op, xs in samples.items():
        if not xs:
            print(f"# {op:<11} 0    (no samples — capture more)")
            continue
        xs.sort()
        p50 = xs[len(xs)//2]
        p90 = (quantiles(xs, n=10)[8] if len(xs) >= 10 else xs[-1])  # 90th pct, or max if <10 samples
        out[op] = round(p90, 1)          # mock uses p90 so it models the slow tail it must tolerate
        print(f"# {op:<11} {len(xs):<4} {p50:6.2f}s  {p90:6.2f}s")
    print("\nDELAYS = {")
    for op in ("login", "auth", "extend", "disconnect", "query"):
        if op in out:
            print(f'    "{op}":{" "*(11-len(op))} {out[op]},')
    print("}")

if __name__ == "__main__":
    main()
