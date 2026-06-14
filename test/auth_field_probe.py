#!/usr/bin/env python3
"""
Probe whether Omada's extPortal/auth REQUIRES apMac / radioId, or works with clientMac alone.

Mirrors the firmware's authenticateOmadaClient (src/omada.cpp): same URL/body, authType=4
(external portal). The only variable is whether we send apMac/ssidName/radioId.

Pending detection uses the explicit field on the all-clients list: authStatus == 1
  (0 = no-auth/wired, 1 = PENDING, 2 = AUTHORIZED).  See memory/api_captures/sites-clients.md.

Modes:
  (default) direct      — omada_url + /api/v2/login (mock on localhost, or a reachable controller)
  --cloud               — TP-Link cloud OAuth via the connector (reaches the live village from anywhere;
                          reuses mock_omada._do_cloud_auth so there's one OAuth implementation)

SAFETY: runs against the LIVE controller and grants a real device ~1 min of access.
  - With no --mac it only LISTS pending candidates and exits (auths nobody).
  - Refuses to fire without the exact --mac, so you can't blind-auth a stranger.
  - Prefer YOUR OWN test phone: join the portal SSID, don't authenticate, then pass its MAC.

Usage:
    ~/.platformio/penv/bin/python3 test/auth_field_probe.py --cloud
    ~/.platformio/penv/bin/python3 test/auth_field_probe.py --cloud --mac AA-BB-CC-DD-EE-FF
    ~/.platformio/penv/bin/python3 test/auth_field_probe.py --cloud --mac ... --with-fields  # control
"""
import json, os, sys, time, argparse, requests
from urllib3.exceptions import InsecureRequestWarning
requests.packages.urllib3.disable_warnings(InsecureRequestWarning)

SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "../data/config.json")
EXTERNAL_PORTAL = 4


def load_config():
    with open(CONFIG_PATH) as f:
        return json.load(f)


class Api:
    """One call() surface over either the direct /api/v2 path or the cloud connector."""
    def __init__(self, config, cloud):
        self.s = requests.Session()
        self.s.verify = False
        cid = config["omada_controller_id"]
        if cloud:
            from mock_omada import _do_cloud_auth, _CLOUD_FWD_HEADERS   # reuse the one OAuth impl
            sess = _do_cloud_auth(config)
            print(f"cloud session OK — device_id={sess['device_id'][:8]}…")
            self.prefix  = f"{sess['connector_url']}/omadac/{sess['device_id']}/{cid}"
            self.s.cookies.set("TPEC_SID",  sess["tpec_sid"])
            self.s.cookies.set("csrfToken", sess["csrf"])
            self.headers = {**_CLOUD_FWD_HEADERS, "csrf-token": sess["csrf"]}
        else:
            base = config["omada_url"].replace(":443", "").rstrip("/")
            lr = self.s.post(f"{base}/{cid}/api/v2/login",
                             json={"username": config["omada_username"], "password": config["omada_password"]})
            login = lr.json()
            if login.get("errorCode") != 0:
                sys.exit(f"login failed: {login.get('msg')}")
            self.prefix  = f"{base}/{cid}"
            self.headers = {"Csrf-Token": login["result"]["token"]}

    def get(self, path):
        return self.s.get(self.prefix + path, headers=self.headers, timeout=30).json()

    def post(self, path, body):
        return self.s.post(self.prefix + path, json=body, headers=self.headers, timeout=30).json()


def list_clients(api, sid):
    r = api.get(f"/api/v2/sites/{sid}/clients?page=1&pageSize=200&filters.active=true")
    return r.get("result", {}).get("data", [])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cloud", action="store_true", help="reach the live controller via TP-Link cloud OAuth")
    ap.add_argument("--mac", help="MAC of a PENDING client (authStatus==1) to auth for 1 min")
    ap.add_argument("--with-fields", action="store_true", help="control run: include apMac/ssidName/radioId")
    ap.add_argument("--auth-type", type=int, default=EXTERNAL_PORTAL, help="authType to send (4=external portal, 3=voucher); default 4")
    ap.add_argument("--code", help="voucher code (sent as voucherCode, for --auth-type 3 experiments)")
    args = ap.parse_args()

    config = load_config()
    api = Api(config, args.cloud)

    sites = api.get("/api/v2/user/sites?currentPage=1&currentPageSize=100")["result"]["data"]
    site = next((s for s in sites if s["id"] == config.get("omada_site_id")), sites[0])
    sid, site_name = site["id"], site["name"]
    print(f"site: {site_name} ({sid})\n")

    clients = list_clients(api, sid)
    pending = [x for x in clients if x.get("authStatus") == 1]

    if not args.mac:
        print(f"PENDING clients (authStatus==1) — {len(pending)} found:")
        for x in pending:
            print(f"  {x['mac']}  ssid={x.get('ssid','?'):<16} ap={x.get('apMac','?')} radio={x.get('radioId','?')}  ip={x.get('ip','?')}")
        print("\nRe-run with --mac <MAC> to auth one for 1 min (minimal body unless --with-fields).")
        print("⚠ that MAC is a REAL device on the portal — use your own test phone if you can.")
        return

    target = next((x for x in clients if x["mac"].upper() == args.mac.upper()), None)
    if not target:
        sys.exit(f"{args.mac} not in the active client list")
    if target.get("authStatus") != 1:
        print(f"⚠ {args.mac} authStatus={target.get('authStatus')} (not 1/pending) — result may be inconclusive")

    body = {"clientMac": args.mac, "site": site_name, "time": "60000", "authType": args.auth_type}
    if args.code:
        body["voucherCode"] = args.code
    if args.with_fields:
        body.update({"apMac": target.get("apMac", ""), "ssidName": target.get("ssid", ""),
                     "radioId": str(target.get("radioId", ""))})
    print(f"POST extPortal/auth  body={json.dumps(body)}")
    resp = api.post("/api/v2/hotspot/extPortal/auth", body)
    print(f"→ errorCode={resp.get('errorCode')}  msg={resp.get('msg')}\n")

    time.sleep(2)
    after = next((x for x in list_clients(api, sid) if x["mac"].upper() == args.mac.upper()), {})
    status = after.get("authStatus")
    print(f"authStatus after: {status}  (1=still pending, 2=AUTHORIZED)")

    ok = resp.get("errorCode") == 0 and status == 2
    fields = "WITH apMac/ssid/radioId" if args.with_fields else "MINIMAL (no apMac/ssid/radioId)"
    print("\n=== VERDICT ===")
    if ok and not args.with_fields:
        print("✅ MINIMAL body AUTHORIZED — apMac/radioId are NOT required. We can cut them.")
    elif ok:
        print("✅ authorized (control run, full body).")
    elif resp.get("errorCode") == 0:
        print(f"⚠ accepted ({fields}) but authStatus still {status} — inconclusive; check the device.")
    else:
        print(f"❌ rejected ({fields}): {resp.get('msg')} — fields may be required (or authType 4 invalid on this portal).")


if __name__ == "__main__":
    main()
