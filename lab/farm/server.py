#!/usr/bin/env python3
"""Download server for the shaper lab: fake YouTube/FB/TikTok on the AT&T side of the network.

GET /__down?bytes=N  -> N bytes (same path as Cloudflare's speed test, so phone.py --url just points here).
Plain http, no TLS: the phones measure the network, not encryption. Keep-alive, so a phone can hold a few
long-lived connections like Reels does.
The Starlink-like delay is added on this Pi's cable with tc netem (see delay.sh), not here.
Run: server.py [--port 8080]
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

CHUNK = b"\0" * 65536
MAX_BYTES = 100_000_000


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        u = urlparse(self.path)
        if u.path != "/__down":
            self.send_error(404); return
        n = min(int(parse_qs(u.query).get("bytes", ["0"])[0]), MAX_BYTES)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(n))
        self.end_headers()
        try:
            while n > 0:
                k = min(n, len(CHUNK))
                self.wfile.write(CHUNK[:k]); n -= k
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=8080)
    a = p.parse_args()
    ThreadingHTTPServer.daemon_threads = True
    ThreadingHTTPServer(("", a.port), Handler).serve_forever()
