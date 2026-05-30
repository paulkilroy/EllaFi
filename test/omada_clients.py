#!/usr/bin/env python3
"""
Standalone utility for managing hotspot clients via the Omada v2 web API directly (no OpenAPI/OAuth needed).
Credit: https://github.com/zachcheatham/ha-omada

Endpoints (operator session):
  List hotspot: GET  /{omadaId}/api/v2/hotspot/sites/{siteId}/clients
  Extend:       POST /{omadaId}/api/v2/hotspot/sites/{siteId}/cmd/clients/{clientId}/extend
                     body: {"period": <milliseconds>}  — adds to existing end, NOT to now
  Disconnect:   POST /{omadaId}/api/v2/hotspot/sites/{siteId}/cmd/clients/{clientId}/disconnect
                     no body; works on expired sessions (Omada does NOT auto-kick on expiry)

Endpoints (admin session, unused in main flow):
  List all:     GET  /{omadaId}/api/v2/sites/{siteId}/clients

Usage:
  python3 omada_clients.py [local|cloud] [--search KEY]
  python3 omada_clients.py --extend 36-18-EA-86-B6-43
  python3 omada_clients.py --extend 36-18-EA-86-B6-43 --extend-time 600000
  python3 omada_clients.py --disconnect 36-18-EA-86-B6-43
"""

import json
import sys
import argparse
import os
import requests
from urllib3.exceptions import InsecureRequestWarning
requests.packages.urllib3.disable_warnings(InsecureRequestWarning)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "../data/config.json")

def load_config():
    with open(CONFIG_PATH) as f:
        return json.load(f)

def admin_login(base_url, omada_id, username, password, verify_ssl):
    url = f"{base_url}/{omada_id}/api/v2/login"
    body = {"username": username, "password": password}
    print(f"POST {url}")
    print(f"Body: {json.dumps(body, indent=2)}\n")

    r = requests.post(url, json=body, verify=verify_ssl)
    data = r.json()
    print(f"Response: {json.dumps(data, indent=2)}\n")

    if data.get("errorCode") != 0:
        print(f"ERROR: {data.get('msg')} (errorCode={data.get('errorCode')})")
        sys.exit(1)

    return data["result"]["token"], r.cookies

def operator_login(base_url, omada_id, username, password, verify_ssl):
    url = f"{base_url}/{omada_id}/api/v2/hotspot/login"
    body = {"name": username, "password": password}
    print(f"POST {url}")
    print(f"Body: {json.dumps(body, indent=2)}\n")

    r = requests.post(url, json=body, verify=verify_ssl)
    data = r.json()
    print(f"Response: {json.dumps(data, indent=2)}\n")

    if data.get("errorCode") != 0:
        print(f"ERROR: {data.get('msg')} (errorCode={data.get('errorCode')})")
        sys.exit(1)

    return data["result"]["token"], r.cookies

def fetch_site_id(base_url, omada_id, token, verify_ssl, cookies):
    """Fetch the first site ID from /api/v2/user/sites using an admin session."""
    url = f"{base_url}/{omada_id}/api/v2/user/sites?currentPage=1&currentPageSize=100"
    r = requests.get(url, headers={"Csrf-Token": token}, cookies=cookies, verify=verify_ssl)
    data = r.json()
    if data.get("errorCode") != 0:
        print(f"ERROR fetching sites: {data.get('msg')}")
        sys.exit(1)
    sites = data.get("result", {}).get("data", [])
    if not sites:
        print("ERROR: no sites returned")
        sys.exit(1)
    site_id = sites[0]["id"]
    print(f"Site ID: {site_id}  ({sites[0].get('name', '?')})\n")
    return site_id

def get_site_id(base_url, omada_id, token, site_name, verify_ssl, cookies):
    url = f"{base_url}/{omada_id}/api/v2/users/current?currentPage=1&currentPageSize=10000"
    headers = {"Csrf-Token": token}
    print(f"GET {url}\n")

    r = requests.get(url, headers=headers, cookies=cookies, verify=verify_ssl)
    data = r.json()
    print(f"Response: {json.dumps(data, indent=2)}\n")

    if data.get("errorCode") != 0:
        print(f"ERROR fetching user info: {data.get('msg')}")
        sys.exit(1)

    sites = data.get("result", {}).get("privilege", {}).get("sites", [])
    for s in sites:
        print(f"  Available site: name='{s.get('name')}' key='{s.get('key')}'")
        if s.get("name") == site_name or s.get("key") == site_name:
            return s["key"]

    if len(sites) == 1:
        print(f"  Only one site found, using it: {sites[0]['key']}")
        return sites[0]["key"]

    print(f"ERROR: site '{site_name}' not found in {[s.get('name') for s in sites]}")
    sys.exit(1)

def get_all_clients(base_url, omada_id, site_id, token, verify_ssl, cookies, active_only=True):
    params = "page=1&pageSize=1000000"
    if active_only:
        params += "&filters.active=true"
    url = f"{base_url}/{omada_id}/api/v2/sites/{site_id}/clients?{params}"
    headers = {"Csrf-Token": token}
    print(f"GET {url}\n")

    r = requests.get(url, headers=headers, cookies=cookies, verify=verify_ssl)
    data = r.json()

    if data.get("errorCode") != 0:
        print(f"ERROR fetching clients: {data.get('msg')}")
        print(f"Full response: {json.dumps(data, indent=2)}")
        sys.exit(1)

    print(f"Full response: {json.dumps(data, indent=2)}\n")
    return data.get("result", {}).get("data", [])

def extend_client(base_url, omada_id, site_id, client_id, time_ms, token, verify_ssl, cookies):
    # client_id is the internal id from the hotspot client list (NOT the MAC)
    # returns {"errorCode":0,"msg":"Success.","result":"<clientMac>"}
    url = f"{base_url}/{omada_id}/api/v2/hotspot/sites/{site_id}/cmd/clients/{client_id}/extend"
    body = {"period": time_ms}  # confirmed: key is "period", value in milliseconds
    headers = {"Csrf-Token": token}
    print(f"POST {url}")
    print(f"Body: {json.dumps(body)}\n")

    r = requests.post(url, json=body, headers=headers, cookies=cookies, verify=verify_ssl)
    data = r.json()
    print(f"Response: {json.dumps(data, indent=2)}\n")
    return data

def disconnect_client(base_url, omada_id, site_id, client_id, token, verify_ssl, cookies):
    # client_id is the internal id from the hotspot client list (NOT the MAC)
    # no request body — confirmed from DevTools (Content-Length: 0)
    url = f"{base_url}/{omada_id}/api/v2/hotspot/sites/{site_id}/cmd/clients/{client_id}/disconnect"
    headers = {"Csrf-Token": token}
    print(f"POST {url} (no body)\n")

    r = requests.post(url, json=None, headers=headers, cookies=cookies, verify=verify_ssl)
    data = r.json()
    print(f"Response: {json.dumps(data, indent=2)}\n")
    return data

def get_hotspot_clients(base_url, omada_id, site_id, token, verify_ssl, cookies, search=None):
    # Each record in the returned list has these confirmed fields:
    #   id           - internal session ID (hex string, e.g. "69a34b85162e843881806923")
    #                  use this (NOT mac) for extend_client / disconnect_client calls
    #   mac          - client MAC address (e.g. "36-18-EA-86-B6-43")
    #   valid        - bool: True = session active, False = expired
    #   start        - session start time, Unix epoch milliseconds
    #   end          - session expiry time, Unix epoch milliseconds
    #                  remaining_milliseconds = end - now_milliseconds
    #                  (time.time() returns seconds, so now_milliseconds = time.time() * 1000)
    #   duration     - seconds the client was actually connected to WiFi during this session
    #                  (independent of valid; an expired session can have duration > 0
    #                   if the client used it before it expired)
    #   authType     - 4 = External Portal Server
    #   permanent    - bool: True = no expiry
    #   ssid         - SSID name
    #   download/upload - bytes transferred (can be 0 even for active sessions)
    # Results are sorted by end desc (most recent first).
    params = "sorts.end=desc&page=1&pageSize=100"
    if search:
        params += f"&searchKey={search}"
    url = f"{base_url}/{omada_id}/api/v2/hotspot/sites/{site_id}/clients?{params}"
    headers = {"Csrf-Token": token}
    print(f"GET {url}\n")

    r = requests.get(url, headers=headers, cookies=cookies, verify=verify_ssl)
    data = r.json()
    print(f"Full response: {json.dumps(data, indent=2)}\n")

    if data.get("errorCode") != 0:
        print(f"ERROR fetching hotspot clients: {data.get('msg')}")
        return None

    return data.get("result", {}).get("data", [])

def find_active_client(clients, mac, require_unexpired=True):
    """Find the most recent session for a given MAC.
    Uses end timestamp (not valid flag) since valid lags behind reality.
    require_unexpired=True  → only match sessions where end > now (use for extend)
    require_unexpired=False → match most recent session regardless of expiry (use for disconnect,
                              since Omada does not kick the client from WiFi on session expiry)
    """
    import time as _time
    now_ms = int(_time.time() * 1000)
    mac = mac.upper().replace(":", "-")
    # sessions are sorted by end desc, so first match is the most recent
    if require_unexpired:
        matches = [c for c in clients if c.get("mac", "").upper() == mac and c.get("end", 0) > now_ms]
        if not matches:
            print(f"ERROR: no unexpired session found for MAC {mac} (session may have already expired)")
            sys.exit(1)
    else:
        matches = [c for c in clients if c.get("mac", "").upper() == mac]
        if not matches:
            print(f"ERROR: no session found for MAC {mac}")
            sys.exit(1)
    return matches[0]

def main():
    parser = argparse.ArgumentParser(description="List Omada hotspot clients via v2 web API")
    parser.add_argument("target", nargs="?", default="cloud", choices=["local", "cloud"])
    parser.add_argument("--search", metavar="KEY", help="filter listed clients by MAC or name")
    parser.add_argument("--extend", metavar="MAC", help="extend the active session for this MAC")
    parser.add_argument("--extend-time", metavar="MS", type=int, default=600000,
                        help="milliseconds to extend by (default: 600000 = 10 minutes)")
    parser.add_argument("--disconnect", metavar="MAC", help="disconnect the active session for this MAC")
    args = parser.parse_args()

    config = load_config()

    if args.target == "cloud":
        base_url   = config["omada_url"].rstrip("/443").rstrip(":443")
        omada_id   = config["omada_controller_id"]
        verify_ssl = True
    else:
        base_url   = config["omada_url_prod_local"].rstrip("/443").rstrip(":443")
        omada_id   = config["omada_controller_id_prod_local"]
        verify_ssl = False

    print(f"=== Target: {args.target} ({base_url}) ===\n")

    # Admin login to fetch site_id
    admin_token, admin_cookies = admin_login(
        base_url, omada_id, config["omada_username"], config["omada_password"], verify_ssl)
    site_id = fetch_site_id(base_url, omada_id, admin_token, verify_ssl, admin_cookies)

    # --- Operator login: hotspot-authed clients ---
    print("=" * 60)
    print("Operator login — hotspot-authed clients (includes remainingTime)")
    print("=" * 60)
    op_token, op_cookies = operator_login(
        base_url, omada_id,
        config["omada_operator_username"], config["omada_operator_password"],
        verify_ssl
    )
    print(f"Operator token: {op_token}\n")

    print("--- Hotspot clients ---")
    hotspot_clients = get_hotspot_clients(base_url, omada_id, site_id, op_token, verify_ssl, op_cookies, args.search)

    if hotspot_clients is None:
        print("  (endpoint returned an error — path may differ on this controller version)")
    elif not hotspot_clients:
        print("  No hotspot-authenticated clients.")
    else:
        import time as _time
        now_ms = int(_time.time() * 1000)
        print(f"  {len(hotspot_clients)} hotspot client(s):")
        for c in hotspot_clients:
            end_ms = c.get('end')
            remaining_s = max(0, round((end_ms - now_ms) / 1000)) if end_ms else '?'
            print(f"  - {c.get('mac')}  id={c.get('id','?')}  valid={c.get('valid','?')}  "
                  f"remaining={remaining_s}s  end={end_ms}")

    if args.extend:
        print()
        print("=" * 60)
        print(f"Extending session for {args.extend} by {args.extend_time}ms")
        print("=" * 60)
        all_clients = get_hotspot_clients(base_url, omada_id, site_id, op_token, verify_ssl, op_cookies)
        client = find_active_client(all_clients, args.extend)
        print(f"Found active session: id={client['id']}\n")
        extend_client(base_url, omada_id, site_id, client["id"], args.extend_time, op_token, verify_ssl, op_cookies)

    if args.disconnect:
        print()
        print("=" * 60)
        print(f"Disconnecting session for {args.disconnect}")
        print("=" * 60)
        all_clients = get_hotspot_clients(base_url, omada_id, site_id, op_token, verify_ssl, op_cookies)
        client = find_active_client(all_clients, args.disconnect, require_unexpired=False)
        print(f"Found active session: id={client['id']}\n")
        disconnect_client(base_url, omada_id, site_id, client["id"], op_token, verify_ssl, op_cookies)

if __name__ == "__main__":
    main()
