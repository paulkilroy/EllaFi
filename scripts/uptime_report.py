#!/usr/bin/env python3
"""Reliability report: Starlink dish telemetry × ESP-visible outages × Omada device alerts,
overlaid on one timeline with uptime stats. Writes tools/uptime_report.html (self-contained).

Sources:
  dish   grpcurl -> 192.168.100.1:9200 get_history: eventLog (the app's Events feed, unix ns)
                    + outages[] (micro-outages, GPS-epoch ns)
  esp    http://<esp>/errors: healthcheck failures + path-probe verdicts + reboots
  omada  {cid}/api/v2/sites/{site}/logs/alerts: AP_ISOLATE / LINK_DOWN etc.

Usage: ./uptime_report.py [--hours 24]
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import time

import requests
import urllib3

urllib3.disable_warnings()

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "tools", "uptime_report.html")
TZ = 8 * 3600                       # Asia/Manila
GPS_TO_UNIX = 315964782             # GPS epoch offset (incl. leap seconds)


def local(ts, fmt="%m/%d %H:%M:%S"):
    return time.strftime(fmt, time.gmtime(ts + TZ))


def load_config():
    cfg = json.load(open(os.path.join(REPO, "data", "config.json")))
    env = cfg.get("environments", {}).get(cfg.get("active_env", ""), {})
    merged = dict(cfg)
    merged.update(env)
    return merged


def pull_dish(t0, t1):
    events = []
    try:
        raw = subprocess.run(["grpcurl", "-plaintext", "-max-time", "15",
                              "-d", '{"get_history":{}}',
                              "192.168.100.1:9200", "SpaceX.API.Device.Device/Handle"],
                             capture_output=True, text=True, timeout=20)
        h = json.loads(raw.stdout)["dishGetHistory"]
    except Exception as e:
        print(f"  dish pull failed: {e}")
        return events
    for e in h.get("eventLog", {}).get("events", []):
        ts = int(e.get("startTimestampNs", 0)) / 1e9
        if not (t0 <= ts <= t1):
            continue
        events.append({"src": "starlink", "ts": ts, "dur": int(e.get("durationNs", 0)) / 1e9,
                       "kind": e.get("reason", "?").replace("EVENT_REASON_", ""),
                       "sev": e.get("severity", "").replace("EVENT_SEVERITY_", "").lower()})
    for o in h.get("outages", []):
        ts = int(o.get("startTimestampNs", 0)) / 1e9 + GPS_TO_UNIX
        if not (t0 <= ts <= t1):
            continue
        events.append({"src": "starlink", "ts": ts, "dur": int(o.get("durationNs", 0)) / 1e9,
                       "kind": o.get("cause", "?"), "sev": "advisory"})
    # eventLog + outages overlap for larger events — dedupe on rounded start second
    seen, out = set(), []
    for e in sorted(events, key=lambda x: -x["dur"]):
        k = round(e["ts"])
        if k in seen:
            continue
        seen.add(k)
        out.append(e)
    return out


def pull_esp(host, password, t0, t1):
    s = requests.Session()
    base = f"http://{host}"
    nonce = s.get(f"{base}/admin/challenge", timeout=8).json()["nonce"]
    digest = hashlib.sha256(f"{nonce}:{password}".encode()).hexdigest()
    s.post(f"{base}/admin/login", json={"nonce": nonce, "digest": digest}, timeout=8)
    text = s.get(f"{base}/errors", timeout=15).text
    events, last_fail = [], None
    for line in text.splitlines():
        m = re.match(r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+\[\w+\]\s+(.*)", line)
        if not m:
            continue
        ts = time.mktime(time.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")) - time.timezone  # log is UTC
        msg = m.group(2)
        if not (t0 <= ts <= t1):
            continue
        if msg.startswith("healthcheck ping failed"):
            last_fail = {"src": "esp", "ts": ts, "dur": 60, "kind": "WAN_BLIP", "sev": "warning"}
            events.append(last_fail)
        elif msg.startswith("path probe") and last_fail:
            last_fail["kind"] = "WAN_BLIP " + msg.replace("path probe: ", "")
        elif msg.startswith("BOOT:"):
            kind = "POWER_CUT_REBOOT" if "POWERON" in msg or "BROWNOUT" in msg else "REBOOT"
            events.append({"src": "esp", "ts": ts, "dur": 17, "kind": kind,
                           "sev": "critical" if kind == "POWER_CUT_REBOOT" else "advisory"})
        elif "RES TIGHT" in msg and "mem:" in msg:
            events.append({"src": "esp", "ts": ts, "dur": 5, "kind": "MEMORY_TIGHT", "sev": "warning"})
    return events


def pull_omada(cfg, t0, t1):
    base = cfg["omada_url"].rstrip("/")
    cid = cfg["omada_controller_id"]
    s = requests.Session()
    r = s.post(f"{base}/{cid}/api/v2/login",
               json={"username": cfg["omada_username"], "password": cfg["omada_password"]},
               verify=False, timeout=20)
    hdr = {"Csrf-Token": r.json()["result"]["token"]}
    r = s.get(f"{base}/{cid}/api/v2/user/sites", headers=hdr,
              params={"currentPage": 1, "currentPageSize": 100}, verify=False, timeout=20)
    site = r.json()["result"]["data"][0]
    site_id = site.get("siteId") or site.get("id")
    # MAC → device name, so alerts read "BES AP was isolated" instead of a MAC
    r = s.get(f"{base}/{cid}/api/v2/sites/{site_id}/grid/devices", headers=hdr,
              params={"currentPage": 1, "currentPageSize": 100}, verify=False, timeout=20)
    names = {d["mac"]: d.get("name", d["mac"]) for d in r.json().get("result", {}).get("data", [])}
    events, page = [], 1
    while True:
        r = s.get(f"{base}/{cid}/api/v2/sites/{site_id}/logs/alerts", headers=hdr,
                  params={"currentPage": page, "currentPageSize": 100,
                          "filters.timeStart": int(t0 * 1000), "filters.timeEnd": int(t1 * 1000)},
                  verify=False, timeout=20)
        res = r.json().get("result", {})
        rows = res.get("data", [])
        for a in rows:
            content = a.get("content", "")
            for mac, name in names.items():
                content = content.replace(mac, name)
            events.append({"src": "omada", "ts": a["time"] / 1000, "dur": 30,
                           "kind": a.get("key", "?"),
                           "sev": (a.get("level") or "warning").lower(),
                           "detail": re.sub(r"\[(ap|switch|gateway):", "", content).replace("]", "")[:120]})
        if page * 100 >= (res.get("totalRows") or 0):
            break
        page += 1
    return events


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=int, default=24)
    args = ap.parse_args()
    cfg = load_config()
    t1 = time.time()
    t0 = t1 - args.hours * 3600

    print("[1/4] dish telemetry…")
    dish = pull_dish(t0, t1)
    print(f"      {len(dish)} events")
    print("[2/4] ESP event log…")
    esp = pull_esp(cfg.get("static_ip", "192.168.1.3"), cfg["omada_password"], t0, t1)
    print(f"      {len(esp)} events")
    print("[3/4] Omada alerts…")
    omada = pull_omada(cfg, t0, t1)
    print(f"      {len(omada)} events")

    dish_down = sum(e["dur"] for e in dish)
    uptime_pct = max(0.0, 100 * (1 - dish_down / (args.hours * 3600)))
    data = {
        "generated": local(t1), "hours": args.hours, "t0": t0, "t1": t1,
        "uptimePct": round(uptime_pct, 3), "dishDownS": round(dish_down, 1),
        "events": sorted(dish + esp + omada, key=lambda e: e["ts"]),
    }
    for e in data["events"]:
        e["label"] = local(e["ts"], "%H:%M:%S")

    tpl = open(os.path.join(REPO, "tools", "uptime_template.html")).read()
    open(OUT, "w").write(tpl.replace("/*__DATA__*/null", json.dumps(data)))
    print(f"[4/4] wrote {os.path.relpath(OUT, REPO)}  "
          f"(Starlink uptime {uptime_pct:.2f}%, {len(dish)} dish / {len(esp)} esp / {len(omada)} omada events)")


if __name__ == "__main__":
    main()
