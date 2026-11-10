#!/usr/bin/env python3
"""Emulated phone doom-scrolling Reels over QUIC, built from Paul's iPhone measurements (lab, 2026-11-09).
Deterministic: no randomness anywhere.

Measured on the iPhone (Reels, 2 Mbps lane):
  - video ~1.0 Mbps (0.85-1.06 measured), fetched in 5-second pieces
  - while watching, the app stays up to 20 s ahead of playback
  - prefetch: the next video to 10 s, each of the following videos to 5 s; 27 videos deep on a fast link
    (behind 2 Mbps the link decides how far it gets: ~6 videos in 30 s)
  - 4 long-lived QUIC connections (5+ when starting from the home feed)
Chosen (Paul): a video can start once 2.5 s is buffered; watch exactly --watch-s seconds, then swipe; every
video is the same length (--video-s).
Unknown, set per run for calibration: --slots = how many pieces download at the same time (spread over the
connections).

Fill order whenever a slot is free:
  1. current video up to --start-s     2. next video's first piece     3. current video up to 20 s ahead
  4. next video's second piece         5. videos 3..--ahead, first piece each, nearest first
Swipe: the old video's unfinished pieces are cancelled (QUIC STOP_SENDING); data still arriving for them counts
as wasted. Per swipe it records whether the new video was ready (--start-s buffered) and, if not, the wait;
plus freezes while watching. Pieces are files /p<bytes> on the lsquic server (64 KB steps).
Run: ~/qenv/bin/python scroll3.py --minutes 4 --slots 4 --name p1
"""
import argparse, asyncio, json, ssl, time
from urllib.parse import urlparse
from aioquic.asyncio import connect
from aioquic.asyncio.protocol import QuicConnectionProtocol
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived
from aioquic.quic.configuration import QuicConfiguration

STEP = 65536
STATS = {"bytes": 0, "wasted_after_cancel": 0}


class Video:
    def __init__(self, i, npieces, piece):
        self.i, self.piece = i, piece
        self.got = [0] * npieces                  # bytes received per piece
        self.state = ["todo"] * npieces           # todo / busy / done
        self.cancelled = False
        self.played = 0.0                         # seconds of playback

    def playable(self):
        """Bytes playable from the start: whole pieces in order, plus the in-order part of the next one."""
        b = 0
        for k, g in enumerate(self.got):
            b += g
            if self.state[k] != "done":
                break
        return b

    def received(self):
        return sum(self.got)


class H3Client(QuicConnectionProtocol):
    def __init__(self, *args, **kw):
        super().__init__(*args, **kw)
        self.h3 = H3Connection(self._quic); self.reqs = {}     # stream id -> (video, piece, done future)

    def quic_event_received(self, ev):
        for e in self.h3.handle_event(ev):
            r = self.reqs.get(getattr(e, "stream_id", None))
            if r is None:
                continue
            v, k, done = r
            if isinstance(e, DataReceived):
                n = len(e.data); STATS["bytes"] += n
                if v.cancelled:
                    STATS["wasted_after_cancel"] += n
                else:
                    v.got[k] = min(v.got[k] + n, v.piece)
            if getattr(e, "stream_ended", False) and not done.done():
                done.set_result(True)

    async def fetch(self, v, k, host):
        sid = self._quic.get_next_available_stream_id()
        done = asyncio.get_running_loop().create_future(); self.reqs[sid] = (v, k, done)
        self.h3.send_headers(sid, [(b":method", b"GET"), (b":scheme", b"https"), (b":authority", host.encode()),
                                   (b":path", f"/p{v.piece}".encode())], end_stream=True)
        self.transmit()
        try:
            while not done.done():
                if v.cancelled:                                   # swipe: ask the server to stop this piece
                    self._quic.stop_stream(sid, 0); self.transmit()
                    await asyncio.sleep(1.0)                      # in-flight data still lands, counted as waste
                    return False
                await asyncio.sleep(0.01)
            return True
        finally:
            self.reqs.pop(sid, None)


async def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--url", default="https://192.168.1.174:4433")
    p.add_argument("--sni", default="pi4", help="name the QUIC server's certificate is set up for")
    p.add_argument("--minutes", type=float, default=4)
    p.add_argument("--conns", type=int, default=4)
    p.add_argument("--slots", type=int, default=4, help="pieces downloading at the same time (unknown: calibrate)")
    p.add_argument("--kbps", type=float, default=1000)
    p.add_argument("--piece-s", type=float, default=5)
    p.add_argument("--video-s", type=float, default=20)
    p.add_argument("--start-s", type=float, default=2.5)
    p.add_argument("--ahead-s", type=float, default=20, help="current video kept this far ahead of playback")
    p.add_argument("--ahead", type=int, default=27, help="videos ahead that get their first piece")
    p.add_argument("--watch-s", type=float, default=4)
    p.add_argument("--name", default="phone")
    a = p.parse_args()
    u = urlparse(a.url)
    bps = a.kbps * 1000 / 8
    piece = max(STEP, round(bps * a.piece_s / STEP) * STEP)
    sec_per_piece = piece / bps
    npieces = max(1, round(a.video_s / sec_per_piece))
    need = bps * a.start_s
    videos = [Video(i, npieces, piece) for i in range(3000)]
    cfg = QuicConfiguration(is_client=True, alpn_protocols=H3_ALPN, verify_mode=ssl.CERT_NONE, server_name=a.sni,
                            max_data=256 << 20, max_stream_data=16 << 20)
    stack, conns = [], []
    for _ in range(a.conns):
        cm = connect(u.hostname, u.port or 443, configuration=cfg, create_protocol=H3Client)
        conns.append(await cm.__aenter__()); stack.append(cm)
    st = {"cur": 0}

    def want(v, k):
        return 0 <= k < npieces and not v.cancelled and v.state[k] == "todo"

    def next_job():
        cur = st["cur"]; v, nx = videos[cur], videos[cur + 1]
        upto = int((v.played + a.ahead_s) / sec_per_piece)            # pieces covering playback + 20 s
        jobs = [(v, k) for k in range(int(a.start_s // sec_per_piece) + 1)]          # 1. current to start
        jobs += [(nx, 0)]                                                              # 2. next, first piece
        jobs += [(v, k) for k in range(upto + 1)]                                      # 3. current, 20 s ahead
        jobs += [(nx, 1)]                                                              # 4. next, second piece
        jobs += [(videos[cur + d], 0) for d in range(2, a.ahead + 1)]                 # 5. further ahead
        for jv, k in jobs:
            if want(jv, k):
                return jv, k
        return None

    async def slot(c):
        while True:
            j = next_job()
            if j is None:
                await asyncio.sleep(0.01); continue
            v, k = j
            v.state[k] = "busy"
            ok = await c.fetch(v, k, a.sni)
            v.state[k] = "done" if ok else "todo"

    tasks = [asyncio.create_task(slot(conns[i % a.conns])) for i in range(a.slots)]
    waits, freezes = [], []
    t_open = time.time()
    while videos[0].playable() < need:
        await asyncio.sleep(0.01)
    first_wait = time.time() - t_open
    t_end = time.time() + a.minutes * 60
    while time.time() < t_end:
        v = videos[st["cur"]]
        frozen, t = 0.0, time.time()
        while v.played < a.watch_s:                   # watch exactly watch_s seconds of video
            await asyncio.sleep(0.02)
            dt = time.time() - t; t = time.time()
            if v.playable() >= min(v.received() if all(s == "done" for s in v.state) else 1e18, bps * (v.played + dt)):
                v.played += dt
            else:
                frozen += dt
        freezes.append(frozen)
        v.cancelled = True                            # swipe
        st["cur"] += 1
        nv = videos[st["cur"]]
        t0 = time.time()
        while nv.playable() < need:                   # is the next video ready? if not, how long do we wait
            await asyncio.sleep(0.01)
        waits.append(time.time() - t0)
    for t in tasks:
        t.cancel()
    played_bytes = sum(min(x.received(), int(bps * x.played)) for x in videos[:st["cur"] + 1])
    q = lambda xs, f: round(sorted(xs)[min(len(xs) - 1, int(f * len(xs)))], 2) if xs else None
    print(json.dumps({"name": a.name, "swipes": len(waits), "first_video_wait_s": round(first_wait, 2),
                      "ready_at_swipe_pct": round(100 * sum(w < 0.05 for w in waits) / max(len(waits), 1)),
                      "wait_s": {"median": q(waits, .5), "p90": q(waits, .9), "max": q(waits, 1)},
                      "freeze_s_total": round(sum(freezes), 1), "videos_with_freeze": sum(f > 0.05 for f in freezes),
                      "mb_received": round(STATS["bytes"] / 1e6, 1),
                      "mb_watched": round(played_bytes / 1e6, 1),
                      "mb_wasted_after_cancel": round(STATS["wasted_after_cancel"] / 1e6, 1),
                      "settings": {k: getattr(a, k) for k in ("conns", "slots", "kbps", "piece_s", "video_s", "start_s",
                                                              "ahead_s", "ahead", "watch_s")}}))
    for cm in stack:
        try:
            await cm.__aexit__(None, None, None)
        except Exception:
            pass


if __name__ == "__main__":
    asyncio.run(main())
