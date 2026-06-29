#!/usr/bin/env python3
"""
Probe the Omada device list (EAPs/switches/gateways) and their status — for the captive-page AP
status map. Confirms which endpoint works and what statusCategory/status values mean, so the
firmware mapping (green=connected, amber=transitional, red=down) is built against real data.

Usage:  python3 test/omada_devices.py [cloud|local]
"""
import json, os, sys, requests
from urllib3.exceptions import InsecureRequestWarning
requests.packages.urllib3.disable_warnings(InsecureRequestWarning)

CFG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../data/config.json")
cfg = json.load(open(CFG))
mode = sys.argv[1] if len(sys.argv) > 1 else "cloud"

if mode == "local":
    base = cfg["omada_url_prod_local"].rstrip("/").replace(":443", "")
    oid  = cfg["omada_controller_id_prod_local"]
else:
    base = cfg["omada_url"].rstrip("/").replace(":443", "")
    oid  = cfg["omada_controller_id"]
user, pw = cfg["omada_username"], cfg["omada_password"]
V = False  # verify_ssl

s = requests.Session()
r = s.post(f"{base}/{oid}/api/v2/login", json={"username": user, "password": pw}, verify=V)
d = r.json()
if d.get("errorCode") != 0:
    print(f"login failed: {d.get('msg')} ({d.get('errorCode')})"); sys.exit(1)
token = d["result"]["token"]
H = {"Csrf-Token": token}
print(f"logged in to {base}/{oid}")

# site id
r = s.get(f"{base}/{oid}/api/v2/user/sites?currentPage=1&currentPageSize=100", headers=H, verify=V)
site = r.json()["result"]["data"][0]["id"]
print(f"site: {site}\n")

# try candidate device endpoints; first one that returns devices wins
candidates = [
    f"/{oid}/api/v2/sites/{site}/grid/devices",
    f"/{oid}/api/v2/sites/{site}/devices",
    f"/{oid}/api/v2/grid/devices/adopted",
]
for path in candidates:
    r = s.get(base + path, headers=H, verify=V)
    try: d = r.json()
    except Exception: print(f"[{r.status_code}] {path}  (non-JSON)"); continue
    ec = d.get("errorCode")
    res = d.get("result")
    rows = res.get("data") if isinstance(res, dict) else res if isinstance(res, list) else None
    print(f"[HTTP {r.status_code}] errorCode={ec}  {path}  -> {len(rows) if rows else 0} devices")
    if ec == 0 and rows:
        print(f"\n=== devices from {path} ===")
        for dev in rows:
            print(f"  name={dev.get('name')!r:28} type={dev.get('type')!r:8} "
                  f"status={dev.get('status')!r:4} statusCategory={dev.get('statusCategory')!r:4} "
                  f"mac={dev.get('mac')!r}")
        # show the full first record so we see every available field
        print(f"\n--- full first device record ---\n{json.dumps(rows[0], indent=2)[:1600]}")
        break
