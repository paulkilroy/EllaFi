#!/usr/bin/env python3
"""
Local HTTP proxy for Omada cloud voucher history.

Authenticates via the 5-step TP-Link OAuth flow (same as omada_cloud_auth.py),
then exposes a simple endpoint the EllaFi admin dashboard can poll for daily
voucher sales data broken down by seller.

Usage:
  python3 cloud_proxy.py [--port 5055] [--debug]

Endpoints:
  GET /health                         → {"ok": true}
  GET /voucher-sales?days=30          → {"days":[{date,vouchers,voucherRev},...]}

Config fields required (data/config.json):
  omada_controller_id, omada_username, omada_password
"""

import argparse
import json
import os
import sys
import time
import threading
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

import requests
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import omada_cloud_auth as cloud_auth
from urllib3.exceptions import InsecureRequestWarning
requests.packages.urllib3.disable_warnings(InsecureRequestWarning)

SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "../data/config.json")

DEBUG = False


# ── Cloud session ─────────────────────────────────────────────────────────────

class CloudSession:
    """Holds a live connector session and exposes simple API helpers."""

    def __init__(self, config):
        self.config        = config
        self.controller_id = config["omada_controller_id"]
        self.session       = None
        self.device_id     = None
        self.connector_url = None
        self.csrf_token    = None
        self.site_id       = None
        self.lock          = threading.Lock()

    def authenticate(self):
        s = requests.Session()
        print("[auth] Starting 5-step TP-Link OAuth...")

        sc = cloud_auth.get_session_code(s, DEBUG) or "EA7C2AE2D0A045ACAC76A33F929ED1EA"
        state, cid, ruri, scope, svc_url, rp_raw = cloud_auth.tplink_login(s, self.config, sc, DEBUG)
        code = cloud_auth.get_oauth_code(s, state, cid, ruri, scope, svc_url, rp_raw, DEBUG)
        if not code:
            raise RuntimeError("OAuth2: could not retrieve code")
        csrf, _ = cloud_auth.login_with_uid_code(s, code, state, DEBUG)
        device_id, connector_url = cloud_auth.get_organization_device_id(
            s, self.controller_id, csrf, DEBUG)
        if not device_id:
            raise RuntimeError("Could not find deviceId for controller")
        site_id = cloud_auth.get_user_sites_remote(
            s, connector_url, device_id, self.controller_id, csrf, DEBUG)
        if not site_id:
            raise RuntimeError("Could not find site_id")

        with self.lock:
            self.session       = s
            self.device_id     = device_id
            self.connector_url = connector_url
            self.csrf_token    = csrf
            self.site_id       = site_id
        print(f"[auth] OK  device_id={device_id[:8]}...  site_id={site_id}")

    def _headers(self):
        return cloud_auth._remote_headers(self.csrf_token)

    def _url_v2(self, path):
        return cloud_auth._remote_url(self.connector_url, self.device_id,
                                       self.controller_id, path)

    def _url_openapi(self, path):
        return (f"{self.connector_url}/omadac/{self.device_id}"
                f"/openapi/v1/{self.controller_id}/{path}")

    def get_voucher_groups(self):
        url = self._url_v2(f"api/v2/hotspot/sites/{self.site_id}/voucherGroups")
        r   = self.session.get(url, headers=self._headers(),
                               params={"currentPage": 1, "currentPageSize": 100}, timeout=30)
        data = r.json()
        if data.get("errorCode") != 0:
            raise RuntimeError(f"voucherGroups error: {data.get('msg')}")
        return data.get("result", {}).get("data", [])

    def get_group_history(self, group_id, start_sec, end_sec):
        """Returns daily usage buckets for one group. time field is in ms."""
        url  = self._url_openapi(
            f"sites/{self.site_id}/hotspot/vouchers/statistics/history"
            f"/voucher-groups/{group_id}")
        r    = self.session.get(url, headers=self._headers(),
                                params={"filters.timeStart": start_sec,
                                        "filters.timeEnd":   end_sec},
                                timeout=30)
        data = r.json()
        if DEBUG:
            print(f"[api] group {group_id[:8]} → {json.dumps(data)[:200]}")
        if data.get("errorCode") != 0:
            raise RuntimeError(f"group history error: {data.get('msg')}")
        return data.get("result", {}).get("usage", [])


# ── Revenue helpers ───────────────────────────────────────────────────────────

def group_price(group):
    """Extract price per voucher from a voucherGroup record."""
    # unitPrice set for groups created with a price in Omada UI
    if group.get("unitPrice"):
        try:
            return float(group["unitPrice"])
        except (ValueError, TypeError):
            pass
    # description JSON set by AUTOGEN groups from our admin page
    desc = group.get("description", "")
    if desc.startswith("{"):
        try:
            return float(json.loads(desc).get("price", 0))
        except Exception:
            pass
    return 0.0


def bucket_revenue(bucket, price_per_voucher):
    """Revenue for one daily bucket: use amount if non-zero, else count × price."""
    amount = float(bucket.get("amount", 0) or 0)
    if amount > 0:
        return amount
    return bucket.get("count", 0) * price_per_voucher


# ── Aggregation ───────────────────────────────────────────────────────────────

def format_date(day_number):
    ts = day_number * 86400 + 43200
    dt = datetime.fromtimestamp(ts, tz=timezone.utc)
    return f"{dt.month}/{dt.day}"


def build_daily(groups, n_days, now_sec, cs):
    today_day = now_sec // 86400
    start_day = today_day - (n_days - 1)
    start_sec = start_day * 86400

    day_vouchers = [0] * n_days
    day_rev      = [0.0] * n_days

    for group in groups:
        gid   = group.get("id")
        price = group_price(group)
        if not gid:
            continue
        try:
            buckets = cs.get_group_history(gid, start_sec, now_sec)
            for b in buckets:
                t_ms  = b.get("time", 0)
                t_sec = t_ms / 1000
                d_num = int(t_sec // 86400)
                idx   = d_num - start_day
                if 0 <= idx < n_days:
                    day_vouchers[idx] += b.get("count", 0)
                    day_rev[idx]      += bucket_revenue(b, price)
        except Exception as e:
            print(f"[warn] group {gid[:8]} failed: {e}")

    return [
        {"date": format_date(start_day + i),
         "vouchers": day_vouchers[i],
         "voucherRev": round(day_rev[i])}
        for i in range(n_days)
    ]


# ── HTTP handler ──────────────────────────────────────────────────────────────

class ProxyHandler(BaseHTTPRequestHandler):
    cs     = None   # CloudSession, set by main()
    groups = []
    groups_at = 0

    def log_message(self, fmt, *args):
        if DEBUG:
            super().log_message(fmt, *args)

    def _refresh_groups(self):
        if time.time() - ProxyHandler.groups_at > 300:
            ProxyHandler.groups    = ProxyHandler.cs.get_voucher_groups()
            ProxyHandler.groups_at = time.time()
            print(f"[groups] refreshed — {len(ProxyHandler.groups)} groups")

    def _json(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        parsed = urlparse(self.path)
        qs     = parse_qs(parsed.query)

        if parsed.path == "/health":
            self._json(200, {"ok": True})
            return

        if parsed.path == "/voucher-sales":
            try:
                n_days = int(qs.get("days", ["30"])[0])
                n_days = max(1, min(n_days, 365))
                self._refresh_groups()
                days = build_daily(ProxyHandler.groups, n_days, int(time.time()), ProxyHandler.cs)
                self._json(200, {"days": days})
                total_v = sum(d["vouchers"] for d in days)
                total_r = sum(d["voucherRev"]  for d in days)
                print(f"[req] /voucher-sales?days={n_days} → {total_v} vouchers  ₱{total_r}")
            except Exception as e:
                print(f"[error] {e}")
                self._json(500, {"error": str(e)})
            return

        self._json(404, {"error": "not found"})


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    global DEBUG
    parser = argparse.ArgumentParser(description="Omada cloud voucher proxy for EllaFi admin")
    parser.add_argument("--port",  type=int, default=5055)
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()
    DEBUG = args.debug

    with open(CONFIG_PATH) as f:
        config = json.load(f)

    cs = CloudSession(config)
    cs.authenticate()

    ProxyHandler.cs        = cs
    ProxyHandler.groups    = cs.get_voucher_groups()
    ProxyHandler.groups_at = time.time()
    print(f"[init] {len(ProxyHandler.groups)} voucher groups loaded")

    server = HTTPServer(("127.0.0.1", args.port), ProxyHandler)
    print(f"\n[ready] http://127.0.0.1:{args.port}/voucher-sales?days=30\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[stop]")


if __name__ == "__main__":
    main()
