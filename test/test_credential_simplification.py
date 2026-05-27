#!/usr/bin/env python3
"""
Tests whether admin credentials alone can replace both the operator AND admin sessions.

The firmware currently uses two credential pairs:
  - Operator (omada_operator_username/password): hotspot login → list/extend/disconnect clients
  - Admin   (omada_username/password):           admin login   → load sites, list all WiFi clients

This script logs in with EACH credential set and tries ALL four operations, printing
pass/fail for each. If admin creds pass all four, we can drop the operator pair.

Usage:
  python3 test/test_credential_simplification.py [local|cloud]
"""

import json
import sys
import os
import requests
from urllib3.exceptions import InsecureRequestWarning
requests.packages.urllib3.disable_warnings(InsecureRequestWarning)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "../data/config.json")

def load_config():
    with open(CONFIG_PATH) as f:
        return json.load(f)

# ── Login helpers ─────────────────────────────────────────────────────────────

def admin_login(base_url, omada_id, username, password, verify_ssl):
    """Admin login → /api/v2/login"""
    url = f"{base_url}/{omada_id}/api/v2/login"
    r = requests.post(url, json={"username": username, "password": password}, verify=verify_ssl)
    data = r.json()
    if data.get("errorCode") != 0:
        return None, None, f"errorCode={data.get('errorCode')} {data.get('msg')}"
    return data["result"]["token"], r.cookies, None

def operator_login(base_url, omada_id, username, password, verify_ssl):
    """Operator (hotspot) login → /api/v2/hotspot/login"""
    url = f"{base_url}/{omada_id}/api/v2/hotspot/login"
    r = requests.post(url, json={"name": username, "password": password}, verify=verify_ssl)
    data = r.json()
    if data.get("errorCode") != 0:
        return None, None, f"errorCode={data.get('errorCode')} {data.get('msg')}"
    return data["result"]["token"], r.cookies, None

# ── Operations ────────────────────────────────────────────────────────────────

def try_load_sites(base_url, omada_id, token, cookies, verify_ssl):
    """Admin op: GET /api/v2/user/sites — load site list"""
    url = f"{base_url}/{omada_id}/api/v2/user/sites?currentPage=1&currentPageSize=100"
    r = requests.get(url, headers={"Csrf-Token": token}, cookies=cookies, verify=verify_ssl)
    data = r.json()
    if data.get("errorCode") != 0:
        return None, f"errorCode={data.get('errorCode')} {data.get('msg')}"
    sites = data.get("result", {}).get("data", [])
    return [{"name": s.get("name"), "id": s.get("id")} for s in sites], None

def try_list_wifi_clients(base_url, omada_id, site_id, token, cookies, verify_ssl):
    """Admin op: GET /api/v2/sites/{siteId}/clients — all connected WiFi clients"""
    url = f"{base_url}/{omada_id}/api/v2/sites/{site_id}/clients?page=1&pageSize=100&filters.active=true"
    r = requests.get(url, headers={"Csrf-Token": token}, cookies=cookies, verify=verify_ssl)
    data = r.json()
    if data.get("errorCode") != 0:
        return None, f"errorCode={data.get('errorCode')} {data.get('msg')}"
    return data.get("result", {}).get("data", []), None

def try_list_hotspot_clients(base_url, omada_id, site_id, token, cookies, verify_ssl):
    """Operator op: GET /api/v2/hotspot/sites/{siteId}/clients — authenticated hotspot sessions"""
    url = f"{base_url}/{omada_id}/api/v2/hotspot/sites/{site_id}/clients?sorts.end=desc&currentPage=1&currentPageSize=10"
    r = requests.get(url, headers={"Csrf-Token": token}, cookies=cookies, verify=verify_ssl)
    data = r.json()
    if data.get("errorCode") != 0:
        return None, f"errorCode={data.get('errorCode')} {data.get('msg')}"
    return data.get("result", {}).get("data", []), None

def try_extend_client(base_url, omada_id, site_id, client_id, token, cookies, verify_ssl):
    """Operator op: POST extend a hotspot client by 60s (minimum allowed)"""
    url = f"{base_url}/{omada_id}/api/v2/hotspot/sites/{site_id}/cmd/clients/{client_id}/extend"
    r = requests.post(url, json={"period": 60000}, headers={"Csrf-Token": token}, cookies=cookies, verify=verify_ssl)
    data = r.json()
    if data.get("errorCode") != 0:
        return False, f"errorCode={data.get('errorCode')} {data.get('msg')}"
    return True, None

def try_portal_auth(base_url, omada_id, site_name, token, cookies, verify_ssl):
    """Operator op: POST extPortal/auth with a fake MAC — tests permission only, not real auth.
    Permission errors return -1007; param/client errors return something else (meaning permission granted)."""
    url = f"{base_url}/{omada_id}/api/v2/hotspot/extPortal/auth"
    payload = {
        "clientMac": "00-00-00-00-00-01",  # fake MAC — will fail at validation, not permission
        "site": site_name,
        "time": "300000",
        "authType": 4,
        "apMac": "00-00-00-00-00-02",
        "ssidName": "TestSSID",
        "radioId": "0",
    }
    r = requests.post(url, json=payload, headers={"Csrf-Token": token}, cookies=cookies, verify=verify_ssl)
    data = r.json()
    code = data.get("errorCode")
    if code == 0:
        return True, None
    if code == -1007:
        return False, f"errorCode={code} {data.get('msg')} — NO PERMISSION"
    # Any other error means the request reached the handler (permission ok, but params rejected)
    return True, f"errorCode={code} {data.get('msg')} — permission ok, params rejected (expected)"

# ── Test runner ───────────────────────────────────────────────────────────────

def run_tests(label, login_fn, cfg, base_url, omada_id, site_id, verify_ssl):
    print(f"\n{'='*60}")
    print(f"  Testing with: {label}")
    print(f"{'='*60}")

    username = cfg.get("omada_username") if "Admin" in label else cfg.get("omada_operator_username")
    password = cfg.get("omada_password") if "Admin" in label else cfg.get("omada_operator_password")

    print(f"  Login as: {username}")
    token, cookies, err = login_fn(base_url, omada_id, username, password, verify_ssl)
    if err:
        print(f"  [FAIL] Login: {err}")
        return
    print(f"  [PASS] Login")

    results = {}

    # 1. Load sites — use the returned site ID and name for all subsequent calls
    sites, err = try_load_sites(base_url, omada_id, token, cookies, verify_ssl)
    if err:
        results["load_sites"] = f"FAIL — {err}"
        return False
    else:
        resolved_site_id   = sites[0]["id"]   if sites else site_id
        resolved_site_name = sites[0]["name"] if sites else ""
        results["load_sites"] = f"PASS — {len(sites)} site(s): {[(s['name'], s['id']) for s in sites]}"

    # 2. List WiFi clients (admin endpoint)
    clients, err = try_list_wifi_clients(base_url, omada_id, resolved_site_id, token, cookies, verify_ssl)
    if err:
        results["list_wifi_clients"] = f"FAIL — {err}"
    else:
        results["list_wifi_clients"] = f"PASS — {len(clients)} active WiFi client(s)"

    # 3. List hotspot clients (operator endpoint)
    hotspot_clients, err = try_list_hotspot_clients(base_url, omada_id, resolved_site_id, token, cookies, verify_ssl)
    if err:
        results["list_hotspot_clients"] = f"FAIL — {err}"
    else:
        results["list_hotspot_clients"] = f"PASS — {len(hotspot_clients)} hotspot session(s)"

    # 4. Extend a client (operator endpoint) — only if we have a session to test with
    if hotspot_clients:
        client_id = hotspot_clients[0].get("id")
        client_mac = hotspot_clients[0].get("mac", "?")
        ok, err = try_extend_client(base_url, omada_id, resolved_site_id, client_id, token, cookies, verify_ssl)
        if err:
            results["extend_client"] = f"FAIL — {err} (tested on {client_mac})"
        else:
            results["extend_client"] = f"PASS (tested on {client_mac})"
    else:
        results["extend_client"] = "SKIP — no active hotspot sessions to test with"

    # 5. Portal auth (extPortal/auth) — the critical firmware operation; uses fake MAC to probe permission only
    ok, err = try_portal_auth(base_url, omada_id, resolved_site_name, token, cookies, verify_ssl)
    if err and "NO PERMISSION" in err:
        results["portal_auth"] = f"FAIL — {err}"
    elif err:
        results["portal_auth"] = f"PASS — {err}"
    else:
        results["portal_auth"] = "PASS — unexpectedly succeeded (fake MAC accepted?)"

    for op, result in results.items():
        status = "PASS" if result.startswith("PASS") else ("SKIP" if result.startswith("SKIP") else "FAIL")
        prefix = "  [" + status + "]"
        print(f"{prefix} {op}: {result}")

    all_passed = all(r.startswith("PASS") or r.startswith("SKIP") for r in results.values())
    print(f"\n  {'✓ ALL PASSED' if all_passed else '✗ SOME FAILED'} with {label}")
    return all_passed

def main():
    cfg = load_config()

    use_local = len(sys.argv) > 1 and sys.argv[1] == "local"
    if use_local:
        raw_url = cfg.get("omada_url_prod_local", "")
        verify_ssl = False
        print("Using: local controller")
    else:
        raw_url = cfg.get("omada_url", "")
        verify_ssl = True
        print("Using: cloud/essentials controller")

    base_url = raw_url.rstrip("/").removesuffix(":443")
    omada_id = cfg.get("omada_controller_id") if not use_local else cfg.get("omada_controller_id_prod_local")
    site_id  = cfg.get("omada_site_id", "")

    print(f"Base URL:  {base_url}")
    print(f"Omada ID:  {omada_id}")
    print(f"Site ID:   {site_id}")

    admin_passed   = run_tests("Admin creds   (omada_username)",    admin_login,    cfg, base_url, omada_id, site_id, verify_ssl)
    operator_passed = run_tests("Operator creds (omada_operator_username)", operator_login, cfg, base_url, omada_id, site_id, verify_ssl)

    print(f"\n{'='*60}")
    print("  SUMMARY")
    print(f"{'='*60}")
    print(f"  Admin creds cover all ops:    {'YES — safe to remove operator pair' if admin_passed else 'NO'}")
    print(f"  Operator creds cover all ops: {'YES' if operator_passed else 'NO'}")
    if admin_passed:
        print("\n  Recommendation: use only admin creds, drop omada_operator_username/password from config.")
    elif operator_passed:
        print("\n  Recommendation: use only operator creds, drop omada_username/password from config.")
    else:
        print("\n  Both credential sets have gaps — keep both.")

if __name__ == "__main__":
    main()
