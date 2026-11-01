#!/usr/bin/env python3
"""Competitor Wi-Fi test (macOS laptop, standard library only) — Nov 2026 visit.

Buy a voucher, log in, stand where their signal is good, then:
    ~/.platformio/penv/bin/python3 test/wifi_compare.py
It names the run after the connected network (a name argument overrides it). macOS hides the network name from
scripts — run `sudo ipconfig setverbose 1` once on the laptop to unlock it, or it asks you to type the name.
Run it on EllaFi first as the baseline. ~2 min per run. Data: ~10-20 MB on a capped network for our phases (≤60 MB
each), plus Apple's full-speed test ≈ speed × 20 s (~50 MB at 20 Mbps, ~250 MB at 100). On a data-limited voucher
prefix NO_APPLE=1 to skip it.

Measures, while pinging 1.1.1.1 and the network's gateway once a second:
  idle 20 s      → lag at rest
  download 30 s  → speed every second (burst-then-cap vs flat cap vs nothing) + lag under load
  upload 15 s    → upload speed + lag under load
  idle 10 s      → does it recover
Records the PUBLIC IP (Cloudflare trace): every network on Daram is Starlink, so the same public IP from two
networks at the same moment = the same dish. Wi-Fi channel/signal from system_profiler.

Writes one summary row to ~/ellafi-wifi-tests.csv and the per-second log to ~/ellafi-wifi-tests/<label>-<time>.csv.
Polite by design: short, one test at a time, only public speed-test endpoints — never touches their equipment.
"""
import csv, json, os, re, ssl, subprocess, sys, threading, time, urllib.request
from datetime import datetime
from statistics import median

# Vultr's Singapore test file (near Starlink's PH exit); Cloudflare's __down rate-limits scripts (429) — fallback only
DOWN_URLS = ["https://sgp-ping.vultr.com/vultr.com.100MB.bin", "https://speed.cloudflare.com/__down?bytes=25000000"]
UP_URL = "https://speed.cloudflare.com/__up"
PHASES = [("idle", 20), ("download", 30), ("upload", 15), ("apple", 25), ("recover", 10)]
if os.environ.get("QUICK"):                       # QUICK=1: a 20-second smoke test of the script itself
    PHASES = [("idle", 5), ("download", 6), ("upload", 5), ("apple", 25), ("recover", 3)]
if os.environ.get("NO_APPLE"):                    # skip the full-speed test on a data-limited voucher
    PHASES = [p for p in PHASES if p[0] != "apple"]
# Our own download/upload phases show the SHAPE (speed each second: burst-then-cap vs flat) but Python tops out
# ~20/45 Mbps (Oct 31: Paul's 500 Mbps line). The "apple" phase runs macOS's built-in networkQuality — finds a nearby
# Apple server, measures true capacity and lag at full load. Uses ~speed × 20 s of data (≈250 MB at 100 Mbps).
STREAMS = 8          # download connections; 4 topped out ~10 Mbps from a far server (Dallas → Singapore, Oct 31)
UP_STREAMS = 4
CAP_BYTES = int(os.environ.get("CAP_MB", 60)) * 1_000_000   # a phase stops early past this, so an uncapped fast
                                                              # network can't eat a data-limited voucher
# the PlatformIO Python ships without a CA bundle — use macOS's
urllib.request.install_opener(urllib.request.build_opener(urllib.request.HTTPSHandler(
    context=ssl.create_default_context(cafile="/etc/ssl/cert.pem" if os.path.exists("/etc/ssl/cert.pem") else None))))

stamp = datetime.now()
moved = [0]          # bytes moved by the active phase's streams
lock = threading.Lock()
stop = threading.Event()


def sh(cmd, timeout=20):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout
    except Exception:
        return ""


def ssid():
    """The connected network's name. macOS 14+ redacts it from every command-line tool unless one of these is set
    up once: `sudo ipconfig setverbose 1` (ipconfig then shows it), or a Shortcut named "Wi-Fi Name" (one action:
    Get Network Details → Wi-Fi → Network Name)."""
    m = re.search(r"^\s*SSID : (.+)$", sh(["ipconfig", "getsummary", "en0"]), re.M)
    if m and "redacted" not in m.group(1):
        return m.group(1).strip()
    out = sh(["shortcuts", "run", "Wi-Fi Name"]).strip()
    return out if out and "redacted" not in out else None


def wifi():
    """Channel, signal/noise, PHY mode of the current network."""
    try:
        d = json.loads(sh(["system_profiler", "SPAirPortDataType", "-json"]))
        cur = d["SPAirPortDataType"][0]["spairport_airport_interfaces"][0].get("spairport_current_network_information", {})
        return {"channel": cur.get("spairport_network_channel"),
                "signal": cur.get("spairport_signal_noise"), "phy": cur.get("spairport_network_phymode")}
    except Exception:
        return {}


def trace():
    """Public IP + Cloudflare data centre (Starlink exit point) from 1.1.1.1/cdn-cgi/trace."""
    try:
        txt = urllib.request.urlopen("https://1.1.1.1/cdn-cgi/trace", timeout=10).read().decode()
        kv = dict(l.split("=", 1) for l in txt.splitlines() if "=" in l)
        return kv.get("ip"), kv.get("colo")
    except Exception as e:
        return f"error: {e}", None


def pinger(host, out):
    """One ping a second for the whole test; out = [(time, ms or None)]."""
    p = subprocess.Popen(["ping", "-i", "1", host], stdout=subprocess.PIPE, text=True)
    for line in p.stdout:
        if stop.is_set():
            break
        m = re.search(r"time=([\d.]+) ms", line)
        if m:
            out.append((time.time(), float(m.group(1))))
        elif "timeout" in line:
            out.append((time.time(), None))
    p.terminate()


phase_end = [0.0]    # shared, so a phase can end early (data cap)
errors = []          # first few stream errors, printed at the end
UA = {"User-Agent": "Mozilla/5.0 (Macintosh) ellafi-wifi-compare"}   # the speed endpoints refuse Python's default


def down_stream():
    k = 0
    while time.time() < phase_end[0]:
        try:
            with urllib.request.urlopen(urllib.request.Request(DOWN_URLS[k % len(DOWN_URLS)], headers=UA), timeout=10) as r:
                while time.time() < phase_end[0]:
                    b = r.read(65536)
                    if not b:
                        break
                    with lock:
                        moved[0] += len(b)
        except Exception as e:
            errors.append(f"download: {e}")
            k += 1                                     # try the other server next
            time.sleep(1)


def up_stream():
    blob = os.urandom(250_000)       # counted when each POST finishes — small, so a 1 Mbps uplink still ticks every ~2 s
    while time.time() < phase_end[0]:
        try:
            urllib.request.urlopen(urllib.request.Request(UP_URL, data=blob, method="POST", headers=UA), timeout=10).read()
            with lock:
                moved[0] += len(blob)
        except Exception as e:
            errors.append(f"upload: {e}")
            time.sleep(0.5)


gw = re.search(r"gateway: (\S+)", sh(["route", "-n", "get", "default"]))
gw = gw.group(1) if gw else None
w = wifi()
w["ssid"] = ssid()
label = sys.argv[1] if len(sys.argv) > 1 else w["ssid"] or input(
    "macOS is hiding the network name (one-time fix: `sudo ipconfig setverbose 1`). Network / seller name: ").strip()
ip, colo = trace()
print(f"{label} · SSID {w.get('ssid')} · {w.get('channel')} · signal {w.get('signal')} · gateway {gw} · public IP {ip} ({colo})")

pings = {"internet": [], "gateway": []}
threading.Thread(target=pinger, args=("1.1.1.1", pings["internet"]), daemon=True).start()
if gw:
    threading.Thread(target=pinger, args=(gw, pings["gateway"]), daemon=True).start()

rows, spans, apple = [], {}, {}
for name, secs in PHASES:
    t0 = time.time(); end = phase_end[0] = t0 + secs
    if name == "apple":
        print("  apple    running macOS networkQuality (~25 s)…")
        try:
            apple = json.loads(sh(["networkQuality", "-s", "-c", "-M", str(secs)], timeout=secs + 30) or "{}")
        except Exception as e:
            errors.append(f"networkQuality: {e}")
        spans[name] = (t0, time.time())
        print(f"  apple    done{' ' * 30}")
        continue
    with lock:
        moved[0] = 0
    if name in ("download", "upload"):
        for _ in range(STREAMS if name == "download" else UP_STREAMS):
            threading.Thread(target=down_stream if name == "download" else up_stream, daemon=True).start()
    last = 0
    while time.time() < end:
        time.sleep(1)
        with lock:
            tot = moved[0]
        mbps = (tot - last) * 8 / 1e6
        last = tot
        rows.append({"t": round(time.time() - t0, 1), "phase": name, "mbps": round(mbps, 2) if name in ("download", "upload") else ""})
        if name in ("download", "upload"):
            print(f"  {name:<8} {rows[-1]['t']:>5.0f} s  {mbps:6.2f} Mbps", end="\r")
        if tot >= CAP_BYTES:
            print(f"  {name}: {CAP_BYTES // 1_000_000} MB reached — stopping early (fast, uncapped network)")
            break
    spans[name] = (t0, time.time())
    phase_end[0] = 0.0                             # stops this phase's streams, even when the cap ended it early
    time.sleep(1)                                  # let them wind down before the next phase counts bytes
    print(f"  {name:<8} done{' ' * 30}")
stop.set()


def lag(host, phase):
    if phase not in spans:
        return None, None, None
    a, b = spans[phase]
    v = [ms for t, ms in pings[host] if a <= t <= b]
    got = sorted(x for x in v if x is not None)
    if not v:
        return None, None, None
    p95 = got[min(len(got) - 1, int(len(got) * .95))] if got else None
    return (round(median(got), 1) if got else None), (round(p95, 1) if p95 is not None else None), round(100 * (len(v) - len(got)) / len(v))


first_byte = lambda v: v[next((i for i, x in enumerate(v) if x > 0), len(v)):]   # skip connection setup
dl = first_byte([r["mbps"] for r in rows if r["phase"] == "download"])
ul = first_byte([r["mbps"] for r in rows if r["phase"] == "upload"])
summary = {
    "when": stamp.strftime("%Y-%m-%d %H:%M"), "label": label, "ssid": w.get("ssid"), "channel": w.get("channel"),
    "signal": w.get("signal"), "public_ip": ip, "colo": colo,
    "down_first5s_mbps": round(median(dl[:5]), 2) if dl else None,       # burst?
    "down_median_mbps": round(median(dl[5:] or dl), 2) if dl else None,  # what it settles to
    "up_median_mbps": round(median(ul[3:] or ul), 2) if ul else None,
    # Apple networkQuality: true capacity + lag at FULL load (responsiveness is round trips per minute → ms)
    "apple_down_mbps": round(apple["dl_throughput"] / 1e6, 1) if apple.get("dl_throughput") else None,
    "apple_up_mbps": round(apple["ul_throughput"] / 1e6, 1) if apple.get("ul_throughput") else None,
    "apple_idle_ms": round(apple["base_rtt"], 1) if apple.get("base_rtt") else None,
    "apple_loaded_down_ms": round(60000 / apple["dl_responsiveness"]) if apple.get("dl_responsiveness") else None,
    "apple_loaded_up_ms": round(60000 / apple["ul_responsiveness"]) if apple.get("ul_responsiveness") else None,
    "apple_mb_used": round((apple.get("dl_bytes_transferred", 0) + apple.get("ul_bytes_transferred", 0)) / 1e6) if apple else None,
    "apple_server": apple.get("test_endpoint"),
}
for host in ("internet", "gateway"):
    for ph in ("idle", "download", "upload", "apple"):
        m, p, l = lag(host, ph)
        summary[f"{host}_{ph}_median_ms"], summary[f"{host}_{ph}_p95_ms"], summary[f"{host}_{ph}_loss_pct"] = m, p, l

path = os.path.expanduser("~/ellafi-wifi-tests.csv")
old = list(csv.DictReader(open(path))) if os.path.exists(path) else []
with open(path, "w", newline="") as f:          # rewrite, so rows from older versions keep lining up with new columns
    wr = csv.DictWriter(f, fieldnames=list(summary), extrasaction="ignore")
    wr.writeheader()
    wr.writerows(old + [summary])
os.makedirs(os.path.expanduser("~/ellafi-wifi-tests"), exist_ok=True)
detail = os.path.expanduser(f"~/ellafi-wifi-tests/{re.sub(r'[^A-Za-z0-9]+', '_', label)}-{stamp:%m%d-%H%M}.csv")
with open(detail, "w", newline="") as f:
    wr = csv.DictWriter(f, fieldnames=["t", "phase", "mbps"])
    wr.writeheader()
    wr.writerows(rows)

print(f"\n{label}: down {summary['down_first5s_mbps']} → {summary['down_median_mbps']} Mbps (first 5 s → settled), "
      f"up {summary['up_median_mbps']} Mbps")
print(f"  lag to internet, median / p95 / loss: idle {lag('internet', 'idle')} · "
      f"while downloading {lag('internet', 'download')} · while uploading {lag('internet', 'upload')}")
if apple:
    print(f"  Apple full-speed test: {summary['apple_down_mbps']} down / {summary['apple_up_mbps']} up Mbps · lag idle "
          f"{summary['apple_idle_ms']} ms → loaded {summary['apple_loaded_down_ms']} / {summary['apple_loaded_up_ms']} ms "
          f"· {summary['apple_mb_used']} MB used · {summary['apple_server']}")
print(f"  public IP {ip} ({colo}) — same IP on two networks at the same time = same Starlink dish")
print(f"  saved → {path} and {detail}")
if errors:
    print(f"  {len(errors)} stream errors, first: {errors[0]}")
