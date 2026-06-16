#!/usr/bin/env python3
"""Fake Omada controller for running tests without real hardware.

Listens on HTTPS port 8043 with a self-signed cert.
Implements all API endpoints the firmware calls:

  Operator session:
    POST /{cid}/api/v2/hotspot/login
    POST /{cid}/api/v2/hotspot/extPortal/auth
    GET  /{cid}/api/v2/hotspot/sites/{siteId}/clients
    POST /{cid}/api/v2/hotspot/sites/{siteId}/cmd/clients/{id}/extend
    POST /{cid}/api/v2/hotspot/sites/{siteId}/cmd/clients/{id}/disconnect

  Admin session:
    POST /{cid}/api/v2/login
    GET  /{cid}/api/v2/user/sites
    GET  /{cid}/api/v2/sites/{siteId}/clients
    GET  /{cid}/api/v2/settings/system/status   (timeZone etc.; forwarded to cloud in --cloud)

  Voucher management (admin session, forwarded to real cloud if --cloud):
    GET  /{cid}/api/v2/hotspot/sites/{siteId}/voucherGroups
    POST /{cid}/api/v2/hotspot/sites/{siteId}/voucherGroups
    GET  /{cid}/api/v2/hotspot/sites/{siteId}/voucherGroups/{groupId}

Session state is tracked in memory:
  - all_clients: keyed by MAC → IP/AP info (represents physically connected clients)
  - hotspot_sessions: dynamic, keyed by MAC — created/updated by auth, mutated by extend/disconnect

Usage:
  sudo python3 test/mock_omada.py                # full mock
  sudo python3 test/mock_omada.py --cloud        # mock hotspot, real cloud for vouchers

--cloud mode:
  Runs the 5-step TP-Link OAuth flow on startup. Admin login responses carry the
  cloud session encoded in the TPOMADA_SESSIONID cookie value (base64url JSON with
  tpec_sid, csrf, device_id, connector_url). Voucher calls and user/sites are
  forwarded to the real cloud controller; all other endpoints remain mocked.

  Why extPortal/auth and hotspot session CRUD stay mocked: those calls involve
  apMac/radioId/ssid that only exist in a real Omada AP captive-portal redirect,
  and real session IDs that only exist when a client connects through a real AP.
"""

import argparse
import base64
import json
import os
import random
import re
import ssl
import sys
import tempfile
import threading
import time
import uuid
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

# Silence urllib3's InsecureRequestWarning — cloud forwarding uses verify=False by design
# (same as the firmware's setInsecure for the Omada cloud connector). urllib3 is only present
# when requests is installed (--cloud mode), so guard the import.
try:
    import urllib3
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
except ImportError:
    pass

BOLD   = "\033[1m"
GREEN  = "\033[92m"
YELLOW = "\033[93m"
CYAN   = "\033[96m"
BLUE   = "\033[94m"
RED    = "\033[91m"
RESET  = "\033[0m"

# Simulated response delays (seconds) — based on observed real Omada cloud latency.
# Set to 0 to disable for fast unit tests.
DELAYS = {
    "login":      1.5,   # operator + admin login
    "auth":       2.5,   # extPortal/auth (fresh auth or re-auth)
    "extend":     2.0,   # extend session
    "disconnect": 1.8,   # disconnect (pause)
    "query":      0.4,   # GET hotspot/all clients, GET sites
}

OPERATOR_CSRF_TOKEN = "mock-operator-csrf-abc123"
ADMIN_CSRF_TOKEN    = "mock-admin-csrf-def456"
MOCK_SITE_ID        = "mock-site-id-001"
MOCK_SITE_NAME      = "MockSite"

# MAC → IP mapping for known test clients. Auth calls don't include an IP
# (the AP knows it, not the portal), so we look it up here. Unknown MACs get
# an auto-assigned IP from the pool below.
CLIENT_IPS = {
    "AA-BB-CC-DD-EE-FF": "192.168.0.100",
}
_ip_pool_counter = 101  # auto-assign starts at 192.168.0.101

def _assign_ip(mac):
    global _ip_pool_counter
    if mac not in CLIENT_IPS:
        CLIENT_IPS[mac] = f"192.168.0.{_ip_pool_counter}"
        _ip_pool_counter += 1
    return CLIENT_IPS[mac]

# All connected clients, keyed by MAC. Populated by auth, removed by disconnect.
# Represents what Omada's /sites/{siteId}/clients endpoint returns.
all_clients = {}

# Named client records (node aliases set via the rename PATCH) are persisted here so they survive a
# mock restart — mirroring how the real controller remembers a client's alias. Ephemeral, unnamed
# session clients (created by auth) are NOT persisted, so a fresh run still starts at "0 connected".
CLIENTS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_clients.json")

def _save_clients():
    named = {mac: rec for mac, rec in all_clients.items() if rec.get("name")}
    try:
        with open(CLIENTS_FILE, "w") as f:
            json.dump(named, f, indent=2)
    except OSError as e:
        print(f"{YELLOW}could not persist clients: {e}{RESET}")

def _load_clients():
    if not os.path.exists(CLIENTS_FILE):
        return
    try:
        with open(CLIENTS_FILE) as f:
            all_clients.update(json.load(f))
        print(f"{GREEN}loaded {len(all_clients)} persisted client alias(es) "
              f"from {os.path.basename(CLIENTS_FILE)}{RESET}")
    except (OSError, ValueError) as e:
        print(f"{YELLOW}could not load persisted clients: {e}{RESET}")

# Active hotspot sessions, keyed by MAC.
hotspot_sessions = {}


def now_ms():
    return int(time.time() * 1000)


def new_client_id():
    return uuid.uuid4().hex


def generate_self_signed_cert():
    import subprocess
    cert_file = os.path.join(tempfile.gettempdir(), "mock_omada_cert.pem")
    key_file  = os.path.join(tempfile.gettempdir(), "mock_omada_key.pem")
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048",
        "-keyout", key_file, "-out", cert_file,
        "-days", "1", "-nodes",
        "-subj", "/CN=mock-omada"
    ], capture_output=True)
    return cert_file, key_file


# ── Cloud proxy support ────────────────────────────────────────────────────────
#
# In --cloud mode, admin login encodes the real cloud session creds (tpec_sid,
# csrf, device_id, connector_url) directly into the TPOMADA_SESSIONID cookie
# value as base64url JSON. Subsequent voucher calls decode the cookie and forward
# to the cloud connector — no server-side session map needed.

CLOUD_SESSION = None   # {"tpec_sid": str, "csrf": str, "device_id": str, "connector_url": str}
_cloud_lock   = threading.Lock()

_ID_HOST       = "https://h2api-id.tplinkcloud.com"
_CLOUD_MANAGER = "https://use1-api-omada-cloud-manager.tplinkcloud.com"
_CLOUD_ACCESS  = "https://use1-api-omada-cloud-access.tplinkcloud.com"
_BROWSER_HEADERS = {
    "User-Agent": ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"),
    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
    "Accept-Language": "en-US,en;q=0.9",
}


def _do_cloud_auth(config):
    """5-step TP-Link OAuth flow. Returns session dict. Raises RuntimeError on failure."""
    import requests as _req
    s             = _req.Session()
    controller_id = config["omada_controller_id"]

    # Step 1: session_code from TP-Link ID login page
    r = s.get("https://id.tplinkcloud.com/", headers=_BROWSER_HEADERS)
    session_code = None
    for pat in [r'["\']sessionCode["\']\s*:\s*["\']([A-F0-9]{32})["\']',
                r'sessionCode[=:\s]+["\']?([A-F0-9]{32})']:
        m = re.search(pat, r.text, re.IGNORECASE)
        if m:
            session_code = m.group(1)
            break
    if not session_code:
        qs = parse_qs(urlparse(r.url).query)
        session_code = (qs.get("sessionCode") or qs.get("session_code") or
                        ["EA7C2AE2D0A045ACAC76A33F929ED1EA"])[0]

    # Step 2: TP-Link ID login → redirectParams (state, clientId, redirect_uri, …)
    r = s.post(f"{_ID_HOST}/api/v1/login",
               json={"email": config["omada_username"], "password": config["omada_password"],
                     "terminalUUID": "d3084c5a-60b8-4fa3-b166-f9b7c3e13f58",
                     "privatePolicyChecked": False},
               headers={"session_code": session_code, "Content-Type": "application/json",
                        "Origin": "https://id.tplinkcloud.com",
                        "Referer": "https://id.tplinkcloud.com/"})
    data = r.json()
    if not data.get("success") and data.get("errorCode", 0) != 0:
        raise RuntimeError(f"TP-Link login failed: {data.get('msg', data)}")
    result      = data.get("result", {})
    rp_raw      = result.get("redirectParams", "")
    rp          = parse_qs(rp_raw)
    state       = rp.get("state",    [""])[0]
    service_url = result.get("serviceUrl", "https://use1-api-id.tplinkcloud.com")

    # Step 3: OAuth2 authorize → extract code from 302 Location
    r = s.get(f"{service_url}/oauth/authorize?{rp_raw}",
              cookies={c.name: c.value for c in s.cookies},
              headers={**_BROWSER_HEADERS,
                       "Origin": "https://id.tplinkcloud.com",
                       "Referer": "https://id.tplinkcloud.com/",
                       "Sec-Fetch-Mode": "navigate"},
              allow_redirects=False)
    code = None
    if r.status_code in (301, 302, 303, 307, 308):
        loc   = r.headers.get("Location", "")
        parts = loc.split("#", 1)
        qsd   = parse_qs(urlparse(parts[0]).query)
        if "code" in qsd:
            code = qsd["code"][0]
        elif len(parts) > 1 and "?" in parts[1]:
            fqs = parse_qs(parts[1].split("?", 1)[1])
            if "code" in fqs:
                code = fqs["code"][0]
    if not code:
        raise RuntimeError("OAuth2: no code= found in authorize redirect")

    # Step 4: exchange code for TPEC_SID + csrfToken
    r = s.post(f"{_CLOUD_MANAGER}/api/v1/central/account/login-with-uid-code",
               json={"code": code, "state": state,
                     "uidServiceUrl": "https://use1-api-id.tplinkcloud.com", "canary": True})
    data = r.json()
    if data.get("errorCode", -1) != 0:
        raise RuntimeError(f"login-with-uid-code failed: {data.get('msg', data)}")
    csrf_token = data.get("result", {}).get("csrfToken", "")
    if csrf_token:
        s.cookies.set("csrfToken", csrf_token, domain=".tplinkcloud.com", path="/")

    tpec_sid = s.cookies.get("TPEC_SID")
    if not tpec_sid:
        for c in s.cookies:
            if len(c.value) > 30 and "tplinkcloud" in (c.domain or ""):
                tpec_sid = c.value
                break
    if not tpec_sid:
        raise RuntimeError("TPEC_SID cookie not found after login-with-uid-code")

    # Step 5: get device_id and connector_url from cloud-access organizations
    r = s.get(f"{_CLOUD_ACCESS}/api/v1/cloudaccess/organizations"
              "?currentPage=1&currentPageSize=10&searchKey=&filters.deviceType=0",
              headers={"csrf-token": csrf_token, "X-Requested-With": "XMLHttpRequest",
                       "Origin": "https://use1-omada-cloud.tplinkcloud.com",
                       "Referer": "https://use1-omada-cloud.tplinkcloud.com/",
                       "request-hash": "1"})
    data = r.json()
    if data.get("errorCode") != 0:
        raise RuntimeError(f"organizations API failed: {data.get('msg', data)}")
    device_id = connector_url = None
    for entry in data.get("result", {}).get("data", []):
        if entry.get("omadacId") == controller_id:
            device_id     = entry["deviceId"]
            connector_url = entry.get("connectorUrl",
                                      "https://use1-api-omada-controller-connector.tplinkcloud.com")
            break
    if not device_id:
        raise RuntimeError(f"No organization entry for controller_id={controller_id}")

    return {"tpec_sid": tpec_sid, "csrf": csrf_token,
            "device_id": device_id, "connector_url": connector_url}


def _make_tpomada_cookie(session):
    """Encode cloud session creds into a TPOMADA_SESSIONID cookie value (base64url JSON)."""
    payload = {"s": session["tpec_sid"], "c": session["csrf"],
               "d": session["device_id"], "u": session["connector_url"]}
    return base64.urlsafe_b64encode(json.dumps(payload).encode()).decode().rstrip("=")


def _decode_tpomada_cookie(val):
    """Decode a cloud-encoded TPOMADA_SESSIONID value. Returns dict or None."""
    try:
        padded = val + "=" * (-len(val) % 4)
        return json.loads(base64.urlsafe_b64decode(padded))
    except Exception:
        return None


_CLOUD_FWD_HEADERS = {
    "omada-remote-timeout": "60",
    "omada-request-source": "web-remote",
    "Origin":               "https://use1-omada-cloud.tplinkcloud.com",
    "Referer":              "https://use1-omada-cloud.tplinkcloud.com/",
    "X-Requested-With":     "XMLHttpRequest",
}


def _forward_to_cloud(method, path, qs, body_bytes, controller_id, creds):
    """Proxy one request to the real cloud connector. Returns (http_status, body_bytes)."""
    import requests as _req
    url = f"{creds['u']}/omadac/{creds['d']}/{controller_id}{path}"
    if qs:
        url += "?" + qs
    s = _req.Session()
    s.cookies.set("TPEC_SID",  creds["s"])
    s.cookies.set("csrfToken", creds["c"])
    headers = {**_CLOUD_FWD_HEADERS, "csrf-token": creds["c"]}
    if body_bytes:
        headers["Content-Type"] = "application/json"
    r = s.request(method, url, headers=headers, data=body_bytes, verify=False, timeout=30)
    return r.status_code, r.content


def _fake_voucher_history(start_sec, end_sec, price=20):
    """Fake daily voucher buckets (timeInterval=24) matching real API format."""
    buckets = []
    day = (start_sec // 86400 + 1) * 86400
    while day <= end_sec:
        count = random.randint(5, 40) if random.random() < 0.8 else 0
        if count:
            buckets.append({
                "count": count, "timeInterval": 24,
                "time": day * 1000,              # ms, midnight UTC
                "amount": str(count * price), "currency": "PHP",
            })
        day += 86400
    return buckets


def _cloud_refresh_loop(config):
    """Background thread: refresh cloud session every 4 hours before it expires."""
    while True:
        time.sleep(4 * 3600)
        try:
            new = _do_cloud_auth(config)
            with _cloud_lock:
                CLOUD_SESSION.update(new)
            print(f"{GREEN}Cloud session refreshed{RESET}")
        except Exception as e:
            print(f"{RED}Cloud session refresh failed: {e}{RESET}")


# ── HTTP handler ───────────────────────────────────────────────────────────────

class OmadaHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        pass

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode("utf-8") if length > 0 else ""
        try:
            return json.loads(raw) if raw else {}, raw
        except json.JSONDecodeError:
            return {}, raw

    def _api_path_qs(self):
        """Return (api_path_without_controller_id, query_string)."""
        path = self.path
        qs   = ""
        if "?" in path:
            path, qs = path.split("?", 1)
        parts = path.strip("/").split("/", 1)
        return ("/" + parts[1]) if len(parts) > 1 else path, qs

    def _api_path(self):
        api, _ = self._api_path_qs()
        return api

    def _controller_id(self):
        return self.path.strip("/").split("/")[0]

    def do_POST(self):
        ts = datetime.now().strftime("%H:%M:%S")
        data, raw = self._read_body()
        api, qs   = self._api_path_qs()

        if api == "/api/v2/hotspot/login":
            self.handle_operator_login(ts, data)
        elif api == "/api/v2/login":
            self.handle_admin_login(ts, data)
        elif api == "/api/v2/hotspot/extPortal/auth":
            self.handle_auth(ts, data)
        elif re.match(r"^/api/v2/hotspot/sites/[^/]+/cmd/clients/[^/]+/extend$", api):
            self.handle_extend(ts, api.split("/")[-2], data)
        elif re.match(r"^/api/v2/hotspot/sites/[^/]+/cmd/clients/[^/]+/disconnect$", api):
            self.handle_disconnect(ts, api.split("/")[-2])
        elif CLOUD_SESSION and re.match(r"^/api/v2/hotspot/sites/[^/]+/voucherGroups", api):
            self.handle_cloud_forward(ts, "POST", api, qs, raw.encode() if raw else None)
        else:
            print(f"  {ts}  {RED}UNKNOWN POST{RESET}  {self.path}  body={raw}")
            self.send_json(404, {"errorCode": -1, "msg": "Not found"})

    def do_GET(self):
        ts = datetime.now().strftime("%H:%M:%S")
        api, qs = self._api_path_qs()

        if CLOUD_SESSION and api == "/api/v2/user/sites":
            self.handle_cloud_forward(ts, "GET", api, qs, None)
        elif re.match(r"^/api/v2/hotspot/sites/[^/]+/voucherGroups", api):
            if CLOUD_SESSION:
                self.handle_cloud_forward(ts, "GET", api, qs, None)
            else:
                self.send_json(200, {"errorCode": 0, "result": {"totalRows": 0, "data": []}})
        elif re.match(r"^/api/v2/hotspot/sites/[^/]+/vouchers/statistics/history$", api):
            if CLOUD_SESSION:
                self.handle_cloud_forward(ts, "GET", api, qs, None)
            else:
                self.handle_voucher_history(ts, qs)
        elif re.match(r"^/api/v2/hotspot/sites/[^/]+/vouchers/statistics/history/groups/[^/]+$", api):
            if CLOUD_SESSION:
                self.handle_cloud_forward(ts, "GET", api, qs, None)
            else:
                self.handle_voucher_history(ts, qs)
        elif api == "/api/v2/user/sites":
            self.handle_sites(ts)
        elif re.match(r"^/api/v2/sites/[^/]+/clients$", api):
            self.handle_all_clients(ts)
        elif re.match(r"^/api/v2/hotspot/sites/[^/]+/clients$", api):
            self.handle_hotspot_clients(ts)
        elif api == "/api/v2/settings/system/status":
            if CLOUD_SESSION:
                self.handle_cloud_forward(ts, "GET", api, qs, None)   # pass through the real controller's status
            else:
                # offline mock: no real controller — firmware only reads timeZone
                self.send_json(200, {"errorCode": 0, "result": {"timeZone": "Asia/Manila"}})
        else:
            print(f"  {ts}  {RED}UNKNOWN GET{RESET}  {self.path}")
            self.send_json(404, {"errorCode": -1, "msg": "Not found"})

    def do_PATCH(self):
        ts = datetime.now().strftime("%H:%M:%S")
        data, raw = self._read_body()
        api, qs   = self._api_path_qs()
        if re.match(r"^/api/v2/sites/[^/]+/clients/[^/]+$", api):       # rename / set client alias
            # Always handled locally — client identity is faked locally (so is the all-clients list).
            # Forwarding to real cloud only ever returned -41011 for bench nodes that aren't real
            # controller clients, and the real rename shape is already captured in api_captures/.
            self.handle_set_client_name(ts, api.split("/")[-1], data)
        else:
            print(f"  {ts}  {RED}UNKNOWN PATCH{RESET}  {self.path}  body={raw}")
            self.send_json(404, {"errorCode": -1, "msg": "Not found"})

    def handle_set_client_name(self, ts, mac, data):
        name = data.get("name", "")
        rec = all_clients.setdefault(mac, {"ip": "", "apMac": "", "ssid": "", "radioId": 0})
        rec["name"] = name
        _save_clients()   # persist the alias so it survives a mock restart
        print(f"  {ts}  {GREEN}{BOLD}SET client name{RESET}  mac={mac} name={name!r}")
        self.send_json(200, {"errorCode": 0, "msg": "Success.",
                             "result": {"mac": mac, "name": name, **rec}})

    # ── Cloud forwarding ───────────────────────────────────────────────────────

    def handle_cloud_forward(self, ts, method, api, qs, body_bytes):
        """Extract TPOMADA_SESSIONID from Cookie header and proxy the call to real cloud."""
        session_id = None
        for part in self.headers.get("Cookie", "").split(";"):
            k, _, v = part.strip().partition("=")
            if k == "TPOMADA_SESSIONID":
                session_id = v
                break
        if not session_id:
            self.send_json(401, {"errorCode": -1, "msg": "no TPOMADA_SESSIONID cookie"})
            return
        creds = _decode_tpomada_cookie(session_id)
        if not creds:
            self.send_json(401, {"errorCode": -1, "msg": "invalid TPOMADA_SESSIONID"})
            return
        print(f"  {ts}  {CYAN}→ cloud{RESET}  {method} {api}{('?' + qs) if qs else ''}")
        try:
            status, body = _forward_to_cloud(method, api, qs, body_bytes,
                                             self._controller_id(), creds)
        except Exception as e:
            print(f"  {ts}  {RED}cloud forward error: {e}{RESET}")
            self.send_json(502, {"errorCode": -1, "msg": str(e)})
            return
        # Surface what the real controller returned (the cloud-forward path skips send_json).
        try:
            cd = json.loads(body); cec, cmsg = cd.get("errorCode"), cd.get("msg", "")
        except Exception:
            cec, cmsg = None, ""
        cbad = status >= 400 or (cec not in (0, None))
        cdetail = f"  errorCode={cec} msg={cmsg!r}" if (cec not in (0, None) or cmsg) else ""
        print(f"  {ts}  {(RED if cbad else GREEN)}← cloud HTTP {status}{RESET}  {api}{cdetail}")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ── Auth endpoints ─────────────────────────────────────────────────────────

    def handle_operator_login(self, ts, data):
        time.sleep(DELAYS["login"])
        print(f"  {ts}  {GREEN}{BOLD}OPERATOR LOGIN{RESET}  user={data.get('name', '?')}")
        self.send_json(200, {
            "errorCode": 0,
            "msg": "Hotspot log in successfully.",
            "result": {"token": OPERATOR_CSRF_TOKEN}
        }, cookies={"TPOMADA_SESSIONID": "mock-operator-session"})

    def handle_admin_login(self, ts, data):
        time.sleep(DELAYS["login"])
        if CLOUD_SESSION:
            with _cloud_lock:
                cookie_val = _make_tpomada_cookie(CLOUD_SESSION)
            print(f"  {ts}  {GREEN}{BOLD}ADMIN LOGIN → cloud{RESET}  user={data.get('username', '?')}")
            self.send_json(200, {
                "errorCode": 0,
                "msg": "Login successfully.",
                "result": {"token": "proxy"}
            }, cookies={"TPOMADA_SESSIONID": cookie_val})
        else:
            print(f"  {ts}  {GREEN}{BOLD}ADMIN LOGIN{RESET}  user={data.get('username', '?')}")
            self.send_json(200, {
                "errorCode": 0,
                "msg": "Login successfully.",
                "result": {"token": ADMIN_CSRF_TOKEN}
            }, cookies={"TPOMADA_SESSIONID": "mock-admin-session"})

    # ── Hotspot session management ─────────────────────────────────────────────

    def handle_auth(self, ts, data):
        time.sleep(DELAYS["auth"])
        mac       = data.get("clientMac", "")
        site      = data.get("site", "?")
        duration  = int(data.get("time", 0))
        is_deauth = duration == 0

        if is_deauth:
            hotspot_sessions.pop(mac, None)
            print(f"  {ts}  {YELLOW}{BOLD}DEAUTH{RESET}  mac={mac} site={site}")
        else:
            minutes = duration // 60000
            end     = now_ms() + duration
            if mac in hotspot_sessions:
                hotspot_sessions[mac]["end"]   = end
                hotspot_sessions[mac]["start"] = now_ms()
            else:
                hotspot_sessions[mac] = {
                    "id":        new_client_id(),
                    "mac":       mac,
                    "start":     now_ms(),
                    "end":       end,
                    "permanent": False,
                    "authType":  4,
                }
            client_ip_from_body = data.get("clientIp", "")
            ip = client_ip_from_body or _assign_ip(mac)
            if client_ip_from_body:
                print(f"  {ts}  {BLUE}clientIp from firmware: {client_ip_from_body}{RESET}")
            else:
                print(f"  {ts}  {YELLOW}clientIp not in auth body — assigned {ip}{RESET}")
            CLIENT_IPS[mac] = ip
            hotspot_sessions[mac]["ip"] = ip   # real Omada returns ip in the hotspot client record too
            all_clients[mac] = {
                "ip":      ip,
                "apMac":   data.get("apMac", ""),
                "ssid":    data.get("ssidName", ""),
                "radioId": data.get("radioId", 0),
            }
            remaining = (end - now_ms()) // 1000
            print(f"  {ts}  {CYAN}{BOLD}AUTH{RESET}  mac={mac} site={site}"
                  f"  duration={minutes}m  expires_in={remaining}s"
                  f"  ap={data.get('apMac','?')} ssid={data.get('ssidName','?')} radio={data.get('radioId','?')}")

        self.send_json(200, {"errorCode": 0, "msg": "Auth success."})

    def handle_extend(self, ts, client_id, data):
        time.sleep(DELAYS["extend"])
        period  = int(data.get("period", 0))
        session = next((s for s in hotspot_sessions.values() if s["id"] == client_id), None)
        if session is None:
            print(f"  {ts}  {RED}EXTEND FAILED{RESET}  clientId={client_id} — not found")
            self.send_json(200, {"errorCode": -1, "msg": "Client not found"})
            return
        session["end"] += period
        remaining = (session["end"] - now_ms()) // 1000
        print(f"  {ts}  {CYAN}{BOLD}EXTEND{RESET}  mac={session['mac']}  +{period // 60000}m"
              f"  new_remaining={remaining}s  end={session['end']}")
        self.send_json(200, {"errorCode": 0, "msg": "Extend success."})

    def handle_disconnect(self, ts, client_id):
        time.sleep(DELAYS["disconnect"])
        session = next((s for s in hotspot_sessions.values() if s["id"] == client_id), None)
        if session is None:
            print(f"  {ts}  {RED}DISCONNECT FAILED{RESET}  clientId={client_id} — not found")
            self.send_json(200, {"errorCode": -1, "msg": "Client not found"})
            return
        mac = session["mac"]
        session["end"] = now_ms()
        if all_clients.pop(mac, None) is not None:
            _save_clients()   # drop it from the persisted store too if it was named
        print(f"  {ts}  {YELLOW}{BOLD}DISCONNECT{RESET}  mac={mac}  clientId={client_id}")
        self.send_json(200, {"errorCode": 0, "msg": "Disconnect success."})

    # ── Query endpoints ────────────────────────────────────────────────────────

    def handle_sites(self, ts):
        time.sleep(DELAYS["query"])
        print(f"  {ts}  {BLUE}GET sites{RESET}  → id={MOCK_SITE_ID} name={MOCK_SITE_NAME}")
        self.send_json(200, {
            "errorCode": 0,
            "result": {
                "totalRows": 1, "currentPage": 1, "currentSize": 1,
                "data": [{"id": MOCK_SITE_ID, "name": MOCK_SITE_NAME}]
            }
        })

    def handle_hotspot_clients(self, ts):
        time.sleep(DELAYS["query"])
        active  = [s for s in hotspot_sessions.values() if s["end"] > now_ms()]
        summary = "  ".join(
            f"mac={s['mac']} remaining={(s['end'] - now_ms()) // 1000}s" for s in active
        ) or "none"
        print(f"  {ts}  {BLUE}GET hotspot clients{RESET}  → {len(active)} active  {summary}")
        self.send_json(200, {
            "errorCode": 0,
            "result": {
                "totalRows": len(active), "currentPage": 1, "currentSize": len(active),
                "data": active
            }
        })

    def handle_cloud_group_history(self, ts, api, qs):
        """Forward per-group history to the real openapi/v1 path through the connector."""
        m = re.match(r"^/api/v2/hotspot/sites/([^/]+)/vouchers/statistics/history/groups/([^/]+)$", api)
        site_id  = m.group(1)
        group_id = m.group(2)
        with _cloud_lock:
            creds = dict(CLOUD_SESSION)
        controller_id = self._controller_id()
        url = (f"{creds['u']}/omadac/{creds['d']}/openapi/v1/{controller_id}"
               f"/sites/{site_id}/hotspot/vouchers/statistics/history/voucher-groups/{group_id}")
        if qs:
            url += "?" + qs
        print(f"  {ts}  {CYAN}→ cloud group history{RESET}  {group_id[:8]}...")
        try:
            import requests as _req
            s = _req.Session()
            s.cookies.set("TPEC_SID",  creds["s"])
            s.cookies.set("csrfToken", creds["c"])
            headers = {**_CLOUD_FWD_HEADERS, "csrf-token": creds["c"]}
            r = s.get(url, headers=headers, verify=False, timeout=30)
            status, body = r.status_code, r.content
        except Exception as e:
            print(f"  {ts}  {RED}cloud group history error: {e}{RESET}")
            self.send_json(502, {"errorCode": -1, "msg": str(e)})
            return
        # Surface what the real controller returned (the cloud-forward path skips send_json).
        try:
            cd = json.loads(body); cec, cmsg = cd.get("errorCode"), cd.get("msg", "")
        except Exception:
            cec, cmsg = None, ""
        cbad = status >= 400 or (cec not in (0, None))
        cdetail = f"  errorCode={cec} msg={cmsg!r}" if (cec not in (0, None) or cmsg) else ""
        print(f"  {ts}  {(RED if cbad else GREEN)}← cloud HTTP {status}{RESET}  {api}{cdetail}")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def handle_voucher_history(self, ts, qs):
        time.sleep(DELAYS["query"])
        params    = parse_qs(qs)
        start_sec = int(params.get("filters.timeStart", ["0"])[0])
        end_sec   = int(params.get("filters.timeEnd",   [str(int(time.time()))])[0])
        buckets   = _fake_voucher_history(start_sec, end_sec)
        n_days    = max(1, (end_sec - start_sec) // 86400)
        total     = sum(b["count"] for b in buckets)
        print(f"  {ts}  {BLUE}GET voucher history{RESET}  {n_days}d → {len(buckets)} buckets  total={total}")
        self.send_json(200, {"errorCode": 0, "result": {"usage": buckets}})

    def handle_all_clients(self, ts):
        time.sleep(DELAYS["query"])
        connected = [
            {"mac": mac, "name": info.get("name", mac), "ip": info["ip"], "apMac": info["apMac"],
             "ssid": info["ssid"], "radioId": info["radioId"]}
            for mac, info in all_clients.items()
        ]
        summary = "  ".join(f"mac={c['mac']} ip={c['ip']}" for c in connected) or "none"
        print(f"  {ts}  {BLUE}GET all clients{RESET}  → {len(connected)} connected  {summary}")
        self.send_json(200, {
            "errorCode": 0,
            "result": {
                "totalRows": len(connected), "currentPage": 1, "currentSize": len(connected),
                "data": connected
            }
        })

    # ── Response helper ────────────────────────────────────────────────────────

    def send_json(self, status, data, cookies=None):
        body = json.dumps(data).encode("utf-8")
        # Surface what we return so mock-side failures (e.g. a 502 / errorCode) are visible, not inferred.
        ec  = data.get("errorCode") if isinstance(data, dict) else None
        msg = data.get("msg", "")   if isinstance(data, dict) else ""
        bad = status >= 400 or (ec not in (0, None))
        detail = f"  errorCode={ec} msg={msg!r}" if (ec not in (0, None) or msg) else ""
        print(f"  {datetime.now().strftime('%H:%M:%S')}  {(RED if bad else GREEN)}← HTTP {status}{RESET}  {self._api_path()}{detail}")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if cookies:
            for name, value in cookies.items():
                self.send_header("Set-Cookie", f"{name}={value}; Path=/")
        self.end_headers()
        self.wfile.write(body)


_START_MTIME = os.path.getmtime(__file__)

def _watch_staleness():
    """Nag if this file has been edited since the process started — i.e. you're running an old copy."""
    while True:
        time.sleep(60)
        try:
            if os.path.getmtime(__file__) > _START_MTIME + 1:
                print(f"\n  {YELLOW}{BOLD}⚠  mock_omada.py changed on disk — you're running an OLD copy. "
                      f"Restart it (Ctrl-C, then re-run).{RESET}\n")
        except Exception:
            pass


def main():
    parser = argparse.ArgumentParser(description="Mock Omada controller")
    parser.add_argument(
        "--cloud", action="store_true",
        help="Proxy admin login, user/sites, and voucher calls to the real TP-Link cloud. "
             "Requires omada_username, omada_password, omada_controller_id in data/config.json. "
             "extPortal/auth and hotspot session CRUD remain mocked.")
    args = parser.parse_args()

    threading.Thread(target=_watch_staleness, daemon=True).start()  # nag if this file gets edited
    _load_clients()   # restore persisted node aliases from a previous run

    if args.cloud:
        config_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                   "../data/config.json")
        with open(config_path) as f:
            cloud_config = json.load(f)
        print(f"{BOLD}Authenticating with TP-Link cloud (5-step OAuth)...{RESET}")
        global CLOUD_SESSION
        try:
            CLOUD_SESSION = _do_cloud_auth(cloud_config)
        except Exception as e:
            print(f"{RED}Cloud auth failed: {e}{RESET}")
            sys.exit(1)
        print(f"{GREEN}Cloud session OK — device_id={CLOUD_SESSION['device_id'][:8]}...{RESET}")
        t = threading.Thread(target=_cloud_refresh_loop, args=(cloud_config,), daemon=True)
        t.start()

    port = 8043
    cert_file, key_file = generate_self_signed_cert()

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert_file, key_file)

    server = ThreadingHTTPServer(("0.0.0.0", port), OmadaHandler)
    server.socket = context.wrap_socket(server.socket, server_side=True)

    cloud_note = f" {CYAN}(+cloud proxy for vouchers){RESET}" if args.cloud else ""
    print(f"{BOLD}Mock Omada Controller{RESET}{cloud_note}")
    print(f"Listening on https://0.0.0.0:{port}")
    if not args.cloud:
        print(f"Mock site: {MOCK_SITE_NAME} ({MOCK_SITE_ID})")
    print(f"Known client IPs: {CLIENT_IPS}")
    print(f"Sessions start empty — auth calls will create them.")
    print(f"Waiting for requests...\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()
        os.unlink(cert_file)
        os.unlink(key_file)


if __name__ == "__main__":
    main()
