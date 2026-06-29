#!/usr/bin/env python3
"""
Preview the captive page (web/index.html) exactly as the firmware serves it — without flashing.

Serves the real web/ files plus the LittleFS-backed routes the page expects:
  /logo       → data/logo.webp if present, else web/EllaFi.webp (the baked default)
  /map.svg    → data/map.svg   (the coverage map; 404 if absent → card stays hidden)
  /theme.css  → data/theme.css if present, else empty (baked dark stays)
  /status     → a fake "connected, no session" status with the map's APs colored, so you can see
                the AP coverage card light up (one representative mix of green/amber/red)

Run it, then open the printed URL. Ctrl+C to stop.
  python3 tools/preview.py            # default port 8000
  python3 tools/preview.py 8080       # custom port
"""
import http.server, json, os, re, socketserver, sys, mimetypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB, DATA = os.path.join(ROOT, "web"), os.path.join(ROOT, "data")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000

def read(path): return open(path, "rb").read()

map_path = os.path.join(DATA, "map.svg")
MACS = re.findall(r'data-mac="([^"]+)"', open(map_path).read()) if os.path.exists(map_path) else []
# representative mix so every state shows: mostly online, one issue, one offline
MIX = [2, 2, 1, 0, 2, 2, 1, 0]

class H(http.server.BaseHTTPRequestHandler):
    def send(self, code, ctype, body):
        if isinstance(body, str): body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path.split("?")[0]
        try:
            if p in ("/", "/index.html"): return self.send(200, "text/html", read(os.path.join(WEB, "index.html")))
            if p == "/admin":             return self.send(200, "text/html", read(os.path.join(WEB, "admin.html")))
            if p == "/logo":
                lp = os.path.join(DATA, "logo.webp")
                lp = lp if os.path.exists(lp) else os.path.join(WEB, "EllaFi.webp")
                return self.send(200, "image/webp", read(lp))
            if p == "/map.svg":
                return self.send(200, "image/svg+xml", read(map_path)) if os.path.exists(map_path) \
                       else self.send(404, "text/plain", "")
            if p == "/theme.css":
                tp = os.path.join(DATA, "theme.css")
                return self.send(200, "text/css", read(tp) if os.path.exists(tp) else b"")
            if p == "/status":
                devs = {mac: MIX[i % len(MIX)] for i, mac in enumerate(MACS)}
                return self.send(200, "application/json", json.dumps({
                    "type": "status", "clientMac": "AA:BB:CC:DD:EE:FF", "clientIp": "192.168.0.142",
                    "sessionStartMillis": 0, "sessionEndMillis": 0, "pausedRemainingMillis": 0,
                    "coinCount": 0, "coinInsertTimeLeft": 0, "minutesPerCoin": 24, "devices": devs}))
            asset = os.path.join(WEB, p.lstrip("/"))            # any other web/ asset (css/js/images)
            if os.path.isfile(asset):
                return self.send(200, mimetypes.guess_type(asset)[0] or "application/octet-stream", read(asset))
            return self.send(404, "text/plain", "")
        except Exception as e:
            self.send(500, "text/plain", str(e))

    def log_message(self, *a): pass

print(f"Captive-page preview → http://localhost:{PORT}/   (admin: /admin)")
print(f"map APs: {len(MACS)}   Ctrl+C to stop")
socketserver.TCPServer.allow_reuse_address = True
socketserver.TCPServer(("", PORT), H).serve_forever()
