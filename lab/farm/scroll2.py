#!/usr/bin/env python3
"""Emulated phone doom-scrolling short videos over QUIC — deterministic (no randomness), Paul's design 2026-11-09.

  - opens --conns QUIC connections once (Reels kept 4 long-lived QUIC connections; lab capture 2026-11-09)
  - every video is the same: --video-s seconds at --kbps (default 20 s x 1.0 Mbps — Reels measured 0.85-1.06 Mbps
    for a 2 Mbps customer, lab 2026-11-09 — rounded to 64 KB steps)
  - each video downloads as two requests: its first --prefetch-kb ("start") and the rest ("rest")
  - each connection works on one request at a time; a free connection takes the highest-priority job:
        1. the next video's start   2. the current video's rest   3. starts of videos further ahead (--ahead)
    (--fifo: no priority, oldest job first)
  - a video can play once it holds --start-s seconds; then it is watched for exactly --watch-s seconds
  - on swipe the old video's unfinished request is cancelled (QUIC STOP_SENDING); data still arriving for it
    is counted as wasted
Per swipe it records: was the next video ready (has --start-s seconds) at the swipe, and if not how long the
viewer waited; plus any freeze while watching (playback ran past the bytes received).
The QUIC server (lsquic on the Pi 4) holds files /p<bytes> in 64 KB steps; content is zeros.
Prints one JSON line at the end.
Run: ~/qenv/bin/python scroll2.py --url https://192.168.1.174:4433 --minutes 4 --name p1
"""
import argparse, asyncio, json, ssl, time
from urllib.parse import urlparse
from aioquic.asyncio import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived
from aioquic.quic.configuration import QuicConfiguration

STEP = 65536


class Video:
    def __init__(self, i, size, start_bytes):
        self.i, self.size, self.start_bytes = i, size, start_bytes
        self.got = 0
        self.start_done = self.rest_queued = self.rest_done = False
        self.cancelled = False


class H3Client(QuicConnectionProtocol):
    def __init__(self, *args, **kw):
        super().__init__(*args, **kw)
        self.h3 = H3Connection(self._quic); self.req = None    # (video, future, stream_id)

    def quic_event_received(self, ev):
        for e in self.h3.handle_event(ev):
            if not self.req or e.stream_id != self.req[2]:
                continue
            v, done, _ = self.req
            if isinstance(e, DataReceived):
                n = len(e.data)
                v.got = min(v.got + n, v.size) if not v.cancelled else v.got
                STATS["bytes"] += n
                if v.cancelled:
                    STATS["wasted_after_cancel"] += n
            if getattr(e, "stream_ended", False) and not done.done():
                done.set_result(True)

    async def get(self, v, nbytes):
        sid = self._quic.get_next_available_stream_id()
        done = asyncio.get_running_loop().create_future(); self.req = (v, done, sid)
        self.h3.send_headers(sid, [(b":method", b"GET"), (b":scheme", b"https"), (b":authority", HOST.encode()),
                                   (b":path", f"/p{nbytes}".encode())], end_stream=True)
        self.transmit()
        try:
            while not done.done():
                if v.cancelled:                                   # swipe: ask the server to stop this stream
                    self._quic.stop_stream(sid, 0); self.transmit()
                    await asyncio.sleep(1.0)                      # let in-flight data land (counted as waste)
                    return False
                await asyncio.sleep(0.01)
            return True
        finally:
            self.req = None


STATS = {"bytes": 0, "wasted_after_cancel": 0}
HOST = ""


async def main():
    global HOST
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--url", default="https://192.168.1.174:4433")
    p.add_argument("--minutes", type=float, default=4)
    p.add_argument("--conns", type=int, default=4)
    p.add_argument("--video-s", type=float, default=20)
    p.add_argument("--kbps", type=float, default=1000)
    p.add_argument("--prefetch-kb", type=int, default=1024)
    p.add_argument("--start-s", type=float, default=2.5)
    p.add_argument("--watch-s", type=float, default=4)
    p.add_argument("--ahead", type=int, default=3, help="videos after the next one whose start is prefetched")
    p.add_argument("--fifo", action="store_true", help="no priority: oldest job first")
    p.add_argument("--name", default="phone")
    p.add_argument("--sni", default="pi4", help="name the QUIC server's certificate is set up for (lsquic -c pi4,...)")
    a = p.parse_args()
    u = urlparse(a.url); HOST = u.hostname
    bps = a.kbps * 1000 / 8
    size = round(bps * a.video_s / STEP) * STEP
    first = min(size, a.prefetch_kb * 1024)
    need = int(bps * a.start_s)
    videos = [Video(i, size, first) for i in range(5000)]
    cfg = QuicConfiguration(is_client=True, alpn_protocols=H3_ALPN, verify_mode=ssl.CERT_NONE, server_name=a.sni,
                            max_data=64 << 20, max_stream_data=16 << 20)
    stack, conns = [], []
    for _ in range(a.conns):
        cm = connect(u.hostname, u.port or 443, configuration=cfg, create_protocol=H3Client)
        conns.append(await cm.__aenter__()); stack.append(cm)
    state = {"cur": 0}
    busy = set()                                  # (video index, kind) being downloaded

    def next_job():
        cur = state["cur"]
        order = []
        nxt = videos[cur + 1]
        order.append((videos[cur], "start"))      # current video's start if somehow still missing
        order.append((nxt, "start"))
        order.append((videos[cur], "rest"))
        order += [(videos[cur + k], "start") for k in range(2, 2 + a.ahead)]
        if a.fifo:                                # oldest first: lower video index first, start before rest
            order.sort(key=lambda j: (j[0].i, j[1] != "start"))
        for v, kind in order:
            if v.cancelled or (v.i, kind) in busy:
                continue
            if kind == "start" and not v.start_done:
                return v, kind
            if kind == "rest" and v.start_done and not v.rest_done and v.size > v.start_bytes:
                return v, kind
        return None

    async def slot(c):
        while True:
            j = next_job()
            if j is None:
                await asyncio.sleep(0.01); continue
            v, kind = j
            busy.add((v.i, kind))
            ok = await c.get(v, v.start_bytes if kind == "start" else v.size - v.start_bytes)
            busy.discard((v.i, kind))
            if ok and kind == "start":
                v.start_done = True
            elif ok:
                v.rest_done = True

    tasks = [asyncio.create_task(slot(c)) for c in conns]
    waits, freezes = [], []
    t_open = time.time()
    while videos[0].got < need:                   # app open: time until the first video can play
        await asyncio.sleep(0.01)
    first_wait = time.time() - t_open
    t_end = time.time() + a.minutes * 60
    while time.time() < t_end:
        v = videos[state["cur"]]
        # watch exactly watch_s seconds of video; a freeze = playback reaches bytes not yet received
        played, frozen, t = 0.0, 0.0, time.time()
        while played < a.watch_s:
            await asyncio.sleep(0.02)
            dt = time.time() - t; t = time.time()
            if v.got >= min(v.size, int(bps * (played + dt))):
                played += dt
            else:
                frozen += dt
        freezes.append(frozen)
        v.cancelled = True                        # swipe: stop this video's downloads
        state["cur"] += 1
        nv = videos[state["cur"]]
        t0 = time.time()
        while nv.got < need:                      # is the next video ready? if not, how long do we wait
            await asyncio.sleep(0.01)
        waits.append(time.time() - t0)
    for t in tasks:
        t.cancel()
    q = lambda xs, f: round(sorted(xs)[min(len(xs) - 1, int(f * len(xs)))], 2) if xs else None
    print(json.dumps({"name": a.name, "swipes": len(waits), "first_video_wait_s": round(first_wait, 2),
                      "ready_at_swipe_pct": round(100 * sum(w < 0.05 for w in waits) / max(len(waits), 1)),
                      "wait_s": {"median": q(waits, .5), "p90": q(waits, .9), "max": q(waits, 1)},
                      "freeze_s_total": round(sum(freezes), 1), "videos_with_freeze": sum(f > 0.05 for f in freezes),
                      "mb_received": round(STATS["bytes"] / 1e6, 1),
                      "mb_wasted_after_cancel": round(STATS["wasted_after_cancel"] / 1e6, 1),
                      "settings": {k: getattr(a, k) for k in ("conns", "video_s", "kbps", "prefetch_kb", "start_s",
                                                              "watch_s", "ahead", "fifo")}}))
    for cm in stack:
        try:
            await cm.__aexit__(None, None, None)
        except Exception:
            pass


if __name__ == "__main__":
    asyncio.run(main())
