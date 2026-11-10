#!/usr/bin/env python3
"""One emulated phone: Bakhaw-shaped traffic, and what its customer would feel.

The traffic follows what the Nov 6-8 Bakhaw captures showed per phone (scratchpad flows.py / peak_summary.py):
  - ON/OFF bursts, not a steady stream: a burst, then a pause (median ~2 s, slowest 10% ~10 s).
  - burst sizes: mostly small (feed images, API calls, 20-300 kB), some video segments (0.35-4 MB),
    occasionally a big download (10-25 MB: an app update, a long video).
  - several connections at once inside a burst (1-4 carry 90% of the data), plus ~15 mostly idle
    connections that wake briefly now and then (apps keep 20-36 open; most are tiny).
  - every burst starts with a DNS lookup, as an app opening a new screen would.

What it records (JSON on stdout at the end):
  - "page" time: how long each burst took start to finish = what the customer waits for a screen to load.
  - download speed while busy, total bytes received.
  - lag under load: ping every 0.5 s to --ping (default the gateway), as median / slowest 10% / lost.
  - DNS lookup time: a popular name (answered from the router's cache) and a made-up name under
    example.com (must go out over the uplink, like a first visit) — median / slowest 10%.
  - TCP resends seen by this phone (from this network namespace's own counters).

Downloads come from --url (default Cloudflare's speed-test endpoint, which serves any size: no setup).
Run: phone.py --minutes 10 [--seed N]   (inside a network namespace for the farm, or plainly on a Pi Zero)
Needs: python3, ping, dig (dnsutils).
"""
import argparse, json, random, re, socket, statistics, subprocess, threading, time, urllib.request

URL = "https://speed.cloudflare.com/__down?bytes={n}"
POPULAR = ["facebook.com", "www.youtube.com", "www.tiktok.com", "graph.facebook.com", "scontent.xx.fbcdn.net",
           "i.ytimg.com", "www.google.com", "roblox.com", "m.facebook.com", "edge-mqtt.facebook.com"]


def pct(v, p):
    v = sorted(v)
    return round(v[min(len(v) - 1, int(p * len(v)))], 1) if v else None


def tcp_counters():
    """RetransSegs and OutSegs for this network namespace (/proc/net/snmp is per namespace)."""
    rows = [l.split() for l in open("/proc/net/snmp") if l.startswith("Tcp:")]
    d = dict(zip(rows[0][1:], map(int, rows[1][1:])))
    return d["RetransSegs"], d["OutSegs"], d["InSegs"]


def fetch(n, out, timeout=120):
    """Download n bytes; record bytes actually received. Errors count as a failed fetch, not a crash."""
    t0 = time.time(); got = 0
    try:
        # Cloudflare refuses Python's default User-Agent (403)
        rq = urllib.request.Request(URL.format(n=n), headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(rq, timeout=timeout) as r:
            while True:
                b = r.read(65536)
                if not b:
                    break
                got += len(b)
        out.append((got, time.time() - t0, True))
    except Exception:
        out.append((got, time.time() - t0, False))


def dns_time(name):
    """Lookup time in ms from dig (it reports the resolver's answer time), None if no answer."""
    r = subprocess.run(["dig", "+tries=1", "+time=3", name], capture_output=True, text=True)
    m = re.search(r"Query time: (\d+) msec", r.stdout)
    return int(m.group(1)) if m else None


class Pinger(threading.Thread):
    def __init__(self, host):
        super().__init__(daemon=True); self.host = host; self.rtt = []; self.sent = 0; self.p = None

    def run(self):
        self.p = subprocess.Popen(["ping", "-i", "0.5", "-W", "2", "-O", self.host], stdout=subprocess.PIPE,
                                  text=True, bufsize=1)
        for line in self.p.stdout:
            if "icmp_seq" in line:
                self.sent += 1
                m = re.search(r"time=([\d.]+)", line)
                if m:
                    self.rtt.append(float(m.group(1)))

    def stop(self):
        if self.p:
            self.p.terminate()


def idle_conn(stop, rng):
    """A mostly idle app connection: a tiny request every 20-60 s (keeps the '20-36 connections' shape)."""
    while not stop.is_set():
        fetch(rng.randint(200, 3000), [], timeout=20)
        stop.wait(rng.uniform(20, 60))


def burst_size(rng):
    x = rng.random()
    if x < 0.80:
        return int(rng.lognormvariate(11.2, 0.9))        # median ~75 kB, mostly 20-300 kB
    if x < 0.97:
        return rng.randint(350_000, 4_000_000)            # video segment
    return rng.randint(10_000_000, 25_000_000)            # app update / long video


def main():
    global URL
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--minutes", type=float, default=10)
    p.add_argument("--seed", type=int, default=None, help="same seed = same traffic, for A/B runs")
    p.add_argument("--ping", default=None, help="ping target (default: this phone's gateway)")
    p.add_argument("--url", default=URL, help="download URL with {n} for the byte count")
    p.add_argument("--idle", type=int, default=15, help="mostly idle connections kept open")
    p.add_argument("--name", default=socket.gethostname())
    a = p.parse_args()
    URL = a.url
    rng = random.Random(a.seed)
    gw = a.ping or (re.search(r"default via (\S+)", subprocess.run(["ip", "route"], capture_output=True,
                                                                     text=True).stdout) or [None, "1.1.1.1"])[1]
    r0, o0, i0 = tcp_counters()
    pinger = Pinger(gw); pinger.start()
    stop = threading.Event()
    for _ in range(a.idle):
        threading.Thread(target=idle_conn, args=(stop, random.Random(rng.random())), daemon=True).start()
    pages, dns_pop, dns_new, got, busy_s, fails = [], [], [], 0, 0.0, 0
    end = time.time() + a.minutes * 60
    while time.time() < end:
        # every screen starts with a lookup: usually a cached popular name, 1 in 4 a never-seen one
        if rng.random() < 0.25:
            dns_new.append(dns_time(f"x{rng.getrandbits(40):x}.example.com"))
        else:
            dns_pop.append(dns_time(rng.choice(POPULAR)))
        total = burst_size(rng)
        k = 1 if total < 150_000 else rng.choice([1, 2, 2, 3, 4])     # parallel connections in this burst
        parts = [total // k] * k
        res = []
        th = [threading.Thread(target=fetch, args=(n, res)) for n in parts]
        t0 = time.time()
        for t in th: t.start()
        for t in th: t.join()
        dt = time.time() - t0
        b = sum(x[0] for x in res); got += b; busy_s += dt; fails += sum(not x[2] for x in res)
        pages.append((total, dt))
        stop.wait(min(rng.lognormvariate(0.7, 1.0), 30))              # median ~2 s, slowest 10% ~7-10 s
    stop.set(); pinger.stop(); time.sleep(0.5)
    r1, o1, i1 = tcp_counters()
    small = [dt for n, dt in pages if n < 300_000]
    video = [dt for n, dt in pages if 350_000 <= n <= 4_000_000]
    lost = pinger.sent - len(pinger.rtt)
    print(json.dumps({
        "name": a.name, "seed": a.seed, "minutes": a.minutes, "bursts": len(pages), "failed_fetches": fails,
        "mb_received": round(got / 1e6, 2),
        "mbps_while_busy": round(got * 8 / busy_s / 1e6, 2) if busy_s else None,
        "page_s": {"small_median": pct(small, .5), "small_p90": pct(small, .9),
                   "video_median": pct(video, .5), "video_p90": pct(video, .9)},
        "ping_ms": {"target": gw, "median": pct(pinger.rtt, .5), "p90": pct(pinger.rtt, .9),
                    "max": max(pinger.rtt) if pinger.rtt else None,
                    "lost_pct": round(100 * lost / pinger.sent, 1) if pinger.sent else None},
        "dns_ms": {"cached_median": pct([x for x in dns_pop if x is not None], .5),
                   "cached_p90": pct([x for x in dns_pop if x is not None], .9),
                   "new_median": pct([x for x in dns_new if x is not None], .5),
                   "new_p90": pct([x for x in dns_new if x is not None], .9),
                   "unanswered": sum(x is None for x in dns_pop + dns_new)},
        "tcp": {"resent_pct": round(100 * (r1 - r0) / max(o1 - o0, 1), 2), "in_segs": i1 - i0},
    }))


if __name__ == "__main__":
    main()
