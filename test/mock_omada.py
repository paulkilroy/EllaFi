#!/usr/bin/env python3
"""Fake Omada controller for running tests without real hardware.

Listens on HTTPS port 443 with a self-signed cert.
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

Session state is tracked in memory:
  - all_clients: keyed by MAC → IP/AP info (represents physically connected clients)
  - hotspot_sessions: dynamic, keyed by MAC — created/updated by auth, mutated by extend/disconnect

Usage:
  sudo python3 test/mock_omada.py
  (sudo needed for port 443)
"""

import json
import ssl
import tempfile
import os
import re
import time
import uuid
from http.server import HTTPServer, BaseHTTPRequestHandler, ThreadingHTTPServer
from datetime import datetime

BOLD   = "\033[1m"
GREEN  = "\033[92m"
YELLOW = "\033[93m"
CYAN   = "\033[96m"
BLUE   = "\033[94m"
RED    = "\033[91m"
RESET  = "\033[0m"

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

# Active hotspot sessions, keyed by MAC.
# {
#   "AA-BB-CC-DD-EE-FF": {
#     "id":        hex string,
#     "mac":       "AA-BB-CC-DD-EE-FF",
#     "start":     epoch_ms,
#     "end":       epoch_ms,
#     "permanent": False,
#     "authType":  4,
#   }
# }
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

    def _api_path(self):
        """Strip /{controllerId} prefix and query string."""
        path = self.path.split("?")[0]
        parts = path.strip("/").split("/", 1)
        return ("/" + parts[1]) if len(parts) > 1 else path

    def do_POST(self):
        ts = datetime.now().strftime("%H:%M:%S")
        data, raw = self._read_body()
        api = self._api_path()

        if api == "/api/v2/hotspot/login":
            self.handle_operator_login(ts, data)
        elif api == "/api/v2/login":
            self.handle_admin_login(ts, data)
        elif api == "/api/v2/hotspot/extPortal/auth":
            self.handle_auth(ts, data)
        elif re.match(r"^/api/v2/hotspot/sites/[^/]+/cmd/clients/[^/]+/extend$", api):
            client_id = api.split("/")[-2]
            self.handle_extend(ts, client_id, data)
        elif re.match(r"^/api/v2/hotspot/sites/[^/]+/cmd/clients/[^/]+/disconnect$", api):
            client_id = api.split("/")[-2]
            self.handle_disconnect(ts, client_id)
        else:
            print(f"  {ts}  {RED}UNKNOWN POST{RESET}  {self.path}  body={raw}")
            self.send_json(404, {"errorCode": -1, "msg": "Not found"})

    def do_GET(self):
        ts = datetime.now().strftime("%H:%M:%S")
        api = self._api_path()

        if api == "/api/v2/user/sites":
            self.handle_sites(ts)
        elif re.match(r"^/api/v2/sites/[^/]+/clients$", api):
            self.handle_all_clients(ts)
        elif re.match(r"^/api/v2/hotspot/sites/[^/]+/clients$", api):
            self.handle_hotspot_clients(ts)
        else:
            print(f"  {ts}  {RED}UNKNOWN GET{RESET}  {self.path}")
            self.send_json(404, {"errorCode": -1, "msg": "Not found"})

    # -------------------------------------------------------------------------
    # Auth endpoints
    # -------------------------------------------------------------------------

    def handle_operator_login(self, ts, data):
        print(f"  {ts}  {GREEN}{BOLD}OPERATOR LOGIN{RESET}  user={data.get('name', '?')}")
        self.send_json(200, {
            "errorCode": 0,
            "msg": "Hotspot log in successfully.",
            "result": {"token": OPERATOR_CSRF_TOKEN}
        }, cookies={"TPOMADA_SESSIONID": "mock-operator-session"})

    def handle_admin_login(self, ts, data):
        print(f"  {ts}  {GREEN}{BOLD}ADMIN LOGIN{RESET}  user={data.get('username', '?')}")
        self.send_json(200, {
            "errorCode": 0,
            "msg": "Login successfully.",
            "result": {"token": ADMIN_CSRF_TOKEN}
        }, cookies={"TPOMADA_SESSIONID": "mock-admin-session"})

    # -------------------------------------------------------------------------
    # Hotspot session management
    # -------------------------------------------------------------------------

    def handle_auth(self, ts, data):
        mac       = data.get("clientMac", "")
        site      = data.get("site", "?")
        duration  = int(data.get("time", 0))
        is_deauth = duration == 0

        if is_deauth:
            # Remove hotspot session; leave all-clients entry intact (client stays connected)
            hotspot_sessions.pop(mac, None)
            print(f"  {ts}  {YELLOW}{BOLD}DEAUTH{RESET}  mac={mac} site={site}")
        else:
            minutes = duration // 60000
            end     = now_ms() + duration
            if mac in hotspot_sessions:
                # Re-auth: keep same client ID, reset end time
                hotspot_sessions[mac]["end"] = end
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
            # Add/update all-clients entry using params from auth request.
            # Prefer clientIp from the request body (sent by firmware); fall back to pool assignment.
            client_ip_from_body = data.get("clientIp", "")
            ip = client_ip_from_body or _assign_ip(mac)
            if client_ip_from_body:
                print(f"  {ts}  {BLUE}clientIp from firmware: {client_ip_from_body}{RESET}")
            else:
                print(f"  {ts}  {YELLOW}clientIp not in auth body — assigned {ip}{RESET}")
            CLIENT_IPS[mac] = ip
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
        period = int(data.get("period", 0))
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
        session = next((s for s in hotspot_sessions.values() if s["id"] == client_id), None)
        if session is None:
            print(f"  {ts}  {RED}DISCONNECT FAILED{RESET}  clientId={client_id} — not found")
            self.send_json(200, {"errorCode": -1, "msg": "Client not found"})
            return

        mac = session["mac"]
        # Terminate the hotspot session and remove from all-clients
        session["end"] = now_ms()
        all_clients.pop(mac, None)
        print(f"  {ts}  {YELLOW}{BOLD}DISCONNECT{RESET}  mac={mac}  clientId={client_id}")
        self.send_json(200, {"errorCode": 0, "msg": "Disconnect success."})

    # -------------------------------------------------------------------------
    # Query endpoints
    # -------------------------------------------------------------------------

    def handle_sites(self, ts):
        print(f"  {ts}  {BLUE}GET sites{RESET}  → id={MOCK_SITE_ID} name={MOCK_SITE_NAME}")
        self.send_json(200, {
            "errorCode": 0,
            "result": {
                "totalRows": 1, "currentPage": 1, "currentSize": 1,
                "data": [{"id": MOCK_SITE_ID, "name": MOCK_SITE_NAME}]
            }
        })

    def handle_hotspot_clients(self, ts):
        active = [s for s in hotspot_sessions.values() if s["end"] > now_ms()]
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

    def handle_all_clients(self, ts):
        connected = [
            {"mac": mac, "ip": info["ip"], "apMac": info["apMac"],
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

    # -------------------------------------------------------------------------

    def send_json(self, status, data, cookies=None):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if cookies:
            for name, value in cookies.items():
                self.send_header("Set-Cookie", f"{name}={value}; Path=/")
        self.end_headers()
        self.wfile.write(body)


def main():
    port = 8043
    cert_file, key_file = generate_self_signed_cert()

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert_file, key_file)

    server = ThreadingHTTPServer(("0.0.0.0", port), OmadaHandler)
    server.socket = context.wrap_socket(server.socket, server_side=True)

    print(f"{BOLD}Mock Omada Controller{RESET}")
    print(f"Listening on https://0.0.0.0:{port}")
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
