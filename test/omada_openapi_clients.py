#!/usr/bin/env python3
"""
Standalone utility for managing hotspot clients via the Omada OpenAPI (northbound) directly.

Requires OpenAPI credentials created in Omada portal:
  Cloud:  GlobalView -> OpenAPI -> Client Management -> Create Client
  Local:  OmadaSDN controller Settings -> OpenAPI -> Client Management

Endpoints used:
  GET  .../hotspot/authed-records               - list (supports searchKey fuzzy match)
  POST .../hotspot/authed-records/{id}/period   - extend time
  POST .../hotspot/authed-records/{id}/disconnect - disconnect client
  DEL  .../hotspot/authed-records/{id}          - delete record

searchKey fuzzy-matches: client_mac, client_name, ssid_name, voucher.code,
                         local_user.user_name, form_name, auth_admin, network_name
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

def get_access_token(base_url, omada_id, client_id, client_secret, verify_ssl):
    url = f"{base_url}/openapi/authorize/token?grant_type=client_credentials"
    body = {
        "omadacId":      omada_id,
        "client_id":     client_id,
        "client_secret": client_secret,
    }
    print(f"POST {url}")
    print(f"Body: {json.dumps(body, indent=2)}\n")

    r = requests.post(url, json=body, verify=verify_ssl)
    data = r.json()
    print(f"Response: {json.dumps(data, indent=2)}\n")

    if data.get("errorCode") != 0:
        print(f"ERROR: {data.get('msg')}")
        sys.exit(1)

    return data["result"]["accessToken"]

def get_sites(base_url, omada_id, token, verify_ssl):
    url = f"{base_url}/openapi/v1/{omada_id}/sites?pageSize=20&page=1"
    headers = {"Authorization": f"AccessToken={token}"}
    print(f"GET {url}\n")

    r = requests.get(url, headers=headers, verify=verify_ssl)
    data = r.json()
    print(f"Sites response: {json.dumps(data, indent=2)}\n")

    if data.get("errorCode") != 0:
        print(f"ERROR fetching sites: {data.get('msg')}")
        sys.exit(1)

    return data["result"]["data"]

def get_authed_records(base_url, omada_id, site_id, token, verify_ssl, search_key=None):
    url = f"{base_url}/openapi/v1/{omada_id}/sites/{site_id}/hotspot/authed-records?pageSize=100&page=1"
    if search_key:
        url += f"&searchKey={search_key}"
    headers = {"Authorization": f"AccessToken={token}"}
    print(f"GET {url}\n")

    r = requests.get(url, headers=headers, verify=verify_ssl)
    data = r.json()
    print(f"Authed records response: {json.dumps(data, indent=2)}\n")

    if data.get("errorCode") != 0:
        print(f"ERROR fetching records: {data.get('msg')}")
        sys.exit(1)

    return data["result"]["data"]

def main():
    parser = argparse.ArgumentParser(description="List Omada authorized hotspot clients via OpenAPI")
    parser.add_argument("target", nargs="?", default="cloud", choices=["local", "cloud"],
                        help="local = openapi_url_local from config (self-signed), cloud = openapi_url from config (default)")
    parser.add_argument("--search", metavar="KEY",
                        help="fuzzy searchKey: matches client_mac, client_name, ssid_name, etc.")
    args = parser.parse_args()

    config = load_config()

    if args.target == "cloud":
        base_url   = config["openapi_url"]
        omada_id   = config["omada_controller_id"]
        verify_ssl = True
    else:
        base_url   = config["openapi_url_local"]
        omada_id   = config["omada_controller_id_prod_local"]
        verify_ssl = False

    client_id     = config["openapi_client_id"]
    client_secret = config["openapi_client_secret"]

    print(f"=== Target: {args.target} ({base_url}) ===\n")

    # Step 1: Get access token
    print("--- Step 1: Get access token ---")
    token = get_access_token(base_url, omada_id, client_id, client_secret, verify_ssl)
    print(f"Access token: {token}\n")

    # Step 2: Get sites
    print("--- Step 2: Get sites ---")
    sites = get_sites(base_url, omada_id, token, verify_ssl)
    if not sites:
        print("No sites found.")
        sys.exit(1)

    for site in sites:
        print(f"  Site: {site.get('name')} / id: {site.get('siteId')}")
    print()

    # Step 3: Get authed records for each site
    for site in sites:
        site_id   = site.get("siteId")
        site_name = site.get("name")
        search_label = f" (searchKey='{args.search}')" if args.search else ""
        print(f"--- Step 3: Authed records for site '{site_name}' ({site_id}){search_label} ---")
        records = get_authed_records(base_url, omada_id, site_id, token, verify_ssl, args.search)

        if not records:
            print("  No authorized clients.\n")
            continue

        print(f"  {len(records)} record(s):")
        for r in records:
            print(f"  - id={r.get('id','?')}  mac={r.get('mac','?')}  name={r.get('name','?')}  "
                  f"remainingTime={r.get('remainingTime','?')}ms  status={r.get('status','?')}")
        print()

if __name__ == "__main__":
    main()
