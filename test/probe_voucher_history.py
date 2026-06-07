#!/usr/bin/env python3
"""
Probe Omada cloud API to find how far back voucher history is retained.

Authenticates via the 5-step TP-Link OAuth flow (reuses omada_cloud_auth.py),
then calls the aggregate voucher history endpoint with progressively older
timeStart values and reports how many buckets and total vouchers come back.

If openapi_client_id / openapi_url are in config.json, also tests the
per-group endpoint for the first voucher group found.

Usage:
  python3 probe_voucher_history.py [--debug]
"""

import json
import os
import sys
import time
import argparse
from datetime import datetime, timezone

import requests
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import omada_cloud_auth as cloud

from urllib3.exceptions import InsecureRequestWarning
requests.packages.urllib3.disable_warnings(InsecureRequestWarning)

SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "../data/config.json")

LOOKBACK_DAYS = [7, 14, 30, 90, 180, 365]


# ── Aggregate history (v2 via cloud connector) ────────────────────────────────

def probe_aggregate(session, connector_url, device_id, controller_id, site_id, csrf_token,
                    start_ms, end_ms, debug):
    url = cloud._remote_url(connector_url, device_id, controller_id,
                            f"api/v2/hotspot/sites/{site_id}/vouchers/statistics/history")
    params = {"filters.timeStart": start_ms, "filters.timeEnd": end_ms}
    print(f"  GET {url}?{json.dumps(params)}")
    r = session.get(url, headers=cloud._remote_headers(csrf_token), params=params, timeout=30)
    print(f"  HTTP {r.status_code}")
    data = r.json()
    print(f"  Response: {json.dumps(data, indent=2) if debug else json.dumps(data)[:300]}")
    if data.get("errorCode") != 0:
        return None, f"errorCode={data.get('errorCode')} {data.get('msg','')}"
    result = data.get("result", {})
    if isinstance(result, list):
        buckets = result
    else:
        buckets = result.get("usage", result.get("data", result.get("list", [])))
    return buckets, None


def get_voucher_groups(session, connector_url, device_id, controller_id, site_id, csrf_token, debug):
    url = cloud._remote_url(connector_url, device_id, controller_id,
                            f"api/v2/hotspot/sites/{site_id}/voucherGroups")
    r = session.get(url, headers=cloud._remote_headers(csrf_token),
                    params={"currentPage": 1, "currentPageSize": 20},
                    timeout=30)
    data = r.json()
    if debug:
        print(f"    {json.dumps(data, indent=2)[:600]}")
    if data.get("errorCode") != 0:
        return []
    return data.get("result", {}).get("data", [])


# ── Per-group history (OpenAPI) ───────────────────────────────────────────────

def get_openapi_token(config):
    base_url = config["openapi_url"].rstrip("/")
    omada_id = config["omada_controller_id"]
    url  = f"{base_url}/openapi/authorize/token?grant_type=client_credentials"
    body = {"omadacId": omada_id,
            "client_id": config["openapi_client_id"],
            "client_secret": config["openapi_client_secret"]}
    r    = requests.post(url, json=body, timeout=15)
    data = r.json()
    if data.get("errorCode") != 0:
        raise RuntimeError(f"OpenAPI token failed: {data.get('msg')}")
    return data["result"]["accessToken"], base_url, omada_id


def get_openapi_site_id(token, base_url, omada_id):
    url = f"{base_url}/openapi/v1/{omada_id}/sites?pageSize=20&page=1"
    r   = requests.get(url, headers={"Authorization": f"AccessToken={token}"}, timeout=15)
    data = r.json()
    if data.get("errorCode") != 0:
        raise RuntimeError(f"OpenAPI sites failed: {data.get('msg')}")
    sites = data["result"]["data"]
    return sites[0]["siteId"] if sites else None


def get_openapi_groups(token, base_url, omada_id, site_id):
    url = f"{base_url}/openapi/v1/{omada_id}/sites/{site_id}/hotspot/vouchers/voucher-groups"
    r   = requests.get(url, headers={"Authorization": f"AccessToken={token}"},
                       params={"page": 1, "pageSize": 20}, timeout=15)
    data = r.json()
    if data.get("errorCode") != 0:
        raise RuntimeError(f"OpenAPI groups failed: {data.get('msg')}")
    return data.get("result", {}).get("data", [])


def probe_connector_group(session, connector_url, device_id, controller_id, site_id, csrf_token,
                          group_id, start_sec, end_sec, debug):
    """Per-group history via openapi/v1 path through the same cloud connector session."""
    url = (f"{connector_url}/omadac/{device_id}/openapi/v1/{controller_id}"
           f"/sites/{site_id}/hotspot/vouchers/statistics/history/voucher-groups/{group_id}")
    params = {"filters.timeStart": start_sec, "filters.timeEnd": end_sec}
    print(f"  GET {url.split('omadac/')[1][:60]}...?{params}")
    r    = session.get(url, headers=cloud._remote_headers(csrf_token), params=params, timeout=30)
    print(f"  HTTP {r.status_code}")
    data = r.json()
    if debug:
        print(f"  Response: {json.dumps(data, indent=2)}")
    else:
        print(f"  Response: {json.dumps(data)[:300]}")
    if data.get("errorCode") != 0:
        return None, f"errorCode={data.get('errorCode')} {data.get('msg','')}"
    result  = data.get("result", {})
    if isinstance(result, list):
        return result, None
    return result.get("usage", result.get("data", result.get("list", []))), None


# ── Reporting ─────────────────────────────────────────────────────────────────

def summarize(buckets):
    if not buckets:
        return 0, 0, None, False
    total      = sum(b.get("count", 0) for b in buckets)
    oldest     = min(b.get("time", float("inf")) for b in buckets)
    oldest_str = datetime.fromtimestamp(oldest / 1000, tz=timezone.utc).strftime("%Y-%m-%d")
    has_amount = any(float(b.get("amount", 0) or 0) > 0 for b in buckets)
    return len(buckets), total, oldest_str, has_amount


def sample_bucket(buckets):
    """Return the most recent non-zero-count bucket for inspection."""
    for b in sorted(buckets, key=lambda x: x.get("time", 0), reverse=True):
        if b.get("count", 0) > 0:
            return b
    return None


def print_header(label):
    print(f"\n{'─'*76}")
    print(f"  {label}")
    print(f"{'─'*76}")
    print(f"  {'Window':<12} {'Buckets':>8} {'Total':>8}  {'Oldest':<14}  {'amount?':<8}  Error")
    print(f"  {'-'*72}")


def print_row(days, buckets, err, show_sample=False):
    label = f"{days} days"
    if err:
        print(f"  {label:<12} {'—':>8} {'—':>8}  {'—':<14}  {'—':<8}  {err}")
    else:
        n, total, oldest, has_amount = summarize(buckets)
        amt_flag = "YES" if has_amount else ("no" if n else "—")
        print(f"  {label:<12} {n:>8} {total:>8}  {oldest or 'no data':<14}  {amt_flag:<8}")
        if show_sample and buckets:
            b = sample_bucket(buckets)
            if b:
                ts = datetime.fromtimestamp(b["time"] / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")
                print(f"    sample bucket: time={ts}  count={b.get('count')}  "
                      f"amount={repr(b.get('amount', 'missing'))}")


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Probe Omada voucher history retention")
    parser.add_argument("--debug", action="store_true", help="verbose API responses")
    args = parser.parse_args()

    with open(CONFIG_PATH) as f:
        config = json.load(f)
    controller_id = config["omada_controller_id"]
    now_sec       = int(time.time())

    # ── Step 1-5: cloud connector auth ────────────────────────────────────────
    s = requests.Session()

    print("Authenticating via TP-Link cloud (5-step OAuth)...")
    session_code = cloud.get_session_code(s, args.debug) or "EA7C2AE2D0A045ACAC76A33F929ED1EA"
    state, client_id, redirect_uri, scope, service_url, rp_raw = \
        cloud.tplink_login(s, config, session_code, args.debug)
    code = cloud.get_oauth_code(s, state, client_id, redirect_uri, scope, service_url, rp_raw, args.debug)
    if not code:
        print("ERROR: OAuth2 code not found"); sys.exit(1)
    csrf_token, _ = cloud.login_with_uid_code(s, code, state, args.debug)
    device_id, connector_url = cloud.get_organization_device_id(s, controller_id, csrf_token, args.debug)
    if not device_id:
        print("ERROR: deviceId not found"); sys.exit(1)
    site_id = cloud.get_user_sites_remote(s, connector_url, device_id, controller_id, csrf_token, args.debug)
    if not site_id:
        print("ERROR: site_id not found"); sys.exit(1)

    print(f"\nAuth OK  site_id={site_id}")

    # ── Probe aggregate history (v2 API) ──────────────────────────────────────
    print_header("Aggregate voucher history  (api/v2  via cloud connector)")
    for i, days in enumerate(LOOKBACK_DAYS):
        buckets, err = probe_aggregate(s, connector_url, device_id, controller_id, site_id,
                                       csrf_token, now_sec - days * 86400, now_sec, args.debug)
        print_row(days, buckets, err, show_sample=(i == 0))

    # ── List voucher groups ───────────────────────────────────────────────────
    groups = get_voucher_groups(s, connector_url, device_id, controller_id, site_id, csrf_token, args.debug)
    if groups:
        print(f"\nVoucher groups ({len(groups)} total):")
        if groups:
            print(f"  Fields in first group: {list(groups[0].keys())}")
        for g in groups[:5]:
            print(f"  id={g.get('id','?')}  name={g.get('name','?')}  "
                  f"used={g.get('usedCount','?')}  unused={g.get('unusedCount','?')}  "
                  f"all_values={json.dumps({k:v for k,v in g.items() if k not in ('id','name')})}")
        if len(groups) > 5:
            print(f"  ... and {len(groups)-5} more")
    else:
        print("\nNo voucher groups found.")

    # ── Probe per-group history (openapi/v1 via connector) ────────────────────
    if not groups:
        return
    # Use the first group with used vouchers as a representative sample
    test_group = next((g for g in groups if g.get("usedCount", 0) > 0), groups[0])
    gid  = test_group.get("id")
    name = test_group.get("name", "?")
    used = test_group.get("usedCount", 0)
    print_header(f"Per-group history  (openapi/v1 via connector)  group: {name} ({used} used)")
    for i, days in enumerate(LOOKBACK_DAYS):
        buckets, err = probe_connector_group(s, connector_url, device_id, controller_id, site_id,
                                             csrf_token, gid,
                                             now_sec - days * 86400, now_sec, args.debug)
        print_row(days, buckets, err, show_sample=(i == 0))


if __name__ == "__main__":
    main()
