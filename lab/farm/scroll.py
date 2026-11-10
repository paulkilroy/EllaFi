#!/usr/bin/env python3
"""One emulated phone doom-scrolling Reels against the lab download server (server.py on the Pi 4).

Copies what Paul's iPhone did on real Reels (lab capture 2026-11-09) and the published prefetch studies:
  - 4 long-lived connections to the server, shared by all downloads (Reels kept 4 QUIC connections open)
  - videos: --video-s seconds at --kbps (Reels measured ~1.4 Mbps 720p) -> ~3.5 MB each
  - keeps the first --prefetch-kb of each of the next --ahead videos downloaded (TikTok-style 1 MB x 5)
  - when a video starts playing, fetches the rest of it in --chunk-kb pieces
  - swipes every --swipe-s seconds; on a swipe the old video's queued pieces are cancelled, but a piece already
    downloading still arrives (what the app does; that data is never watched)
Records per swipe the start delay: time until the new video holds --start-s seconds of playback.
Prints one JSON line at the end. Same --seed = same videos each run.
Run: scroll.py --minutes 4 --url http://192.168.1.174:8080 [--name p1]
     scroll.py --quic --url https://192.168.1.174:4433   (QUIC, needs ~/qenv with aioquic)
"""
import argparse, http.client, json, queue, random, statistics, threading, time
from urllib.parse import urlparse

a = None


class Video:
    def __init__(self, i, size):
        self.i, self.size = i, size
        self.got = 0                      # bytes received so far (in order: pieces are fetched front to back)
        self.next_off = 0                 # next byte offset not yet requested
        self.cancelled = False


def worker(jobs, stats, lock):
    """One of the 4 connections: takes (video, offset, length) jobs and downloads them in turn."""
    u = urlparse(a.url)
    conn = None
    while True:
        v, off, n = jobs.get()
        if v is None:
            return
        if v.cancelled:
            continue
        for attempt in (1, 2):
            try:
                conn = conn or http.client.HTTPConnection(u.hostname, u.port or 80, timeout=60)
                conn.request("GET", f"/__down?bytes={n}", headers={"User-Agent": "scroll"})
                r = conn.getresponse()
                while True:
                    b = r.read(16384)
                    if not b:
                        break
                    with lock:
                        v.got += len(b); stats["bytes"] += len(b)
                        if v.cancelled:
                            stats["unwatched"] += len(b)
                break
            except Exception:
                with lock: stats["errors"] += 1
                conn = None


QUIC_STEP = 65536                         # the QUIC server holds one file per 64 KB step (quic_files.sh)


def quic_worker(jobs, stats, lock):
    """Same job as worker(), over one long-lived HTTP/3 (QUIC) connection, like Reels' 4 QUIC connections.
    The server decides how hard to push (lsquic, congestion control set on its side); this side only receives."""
    import asyncio, ssl
    from aioquic.asyncio import connect
    from aioquic.asyncio.protocol import QuicConnectionProtocol
    from aioquic.h3.connection import H3_ALPN, H3Connection
    from aioquic.h3.events import DataReceived, HeadersReceived
    from aioquic.quic.configuration import QuicConfiguration

    class H3Client(QuicConnectionProtocol):
        def __init__(self, *args, **kw):
            super().__init__(*args, **kw)
            self.h3 = H3Connection(self._quic); self.waiters = {}

        def quic_event_received(self, ev):
            for e in self.h3.handle_event(ev):
                w = self.waiters.get(e.stream_id)
                if w is None:
                    continue
                v, done = w
                if isinstance(e, DataReceived):
                    with lock:
                        v.got += len(e.data); stats["bytes"] += len(e.data)
                        if v.cancelled:
                            stats["unwatched"] += len(e.data)
                if getattr(e, "stream_ended", False):
                    done.set_result(True); self.waiters.pop(e.stream_id, None)

        async def get(self, path, v):
            sid = self._quic.get_next_available_stream_id()
            done = asyncio.get_running_loop().create_future(); self.waiters[sid] = (v, done)
            self.h3.send_headers(sid, [(b":method", b"GET"), (b":scheme", b"https"), (b":authority", u.hostname.encode()),
                                       (b":path", path.encode()), (b"user-agent", b"scroll")], end_stream=True)
            self.transmit()
            await asyncio.wait_for(done, 60)

    u = urlparse(a.url)

    async def run():
        cfg = QuicConfiguration(is_client=True, alpn_protocols=H3_ALPN, verify_mode=ssl.CERT_NONE,
                                max_data=64 << 20, max_stream_data=16 << 20)
        loop = asyncio.get_running_loop()
        while True:
            try:
                async with connect(u.hostname, u.port or 443, configuration=cfg, create_protocol=H3Client) as c:
                    while True:
                        v, off, n = await loop.run_in_executor(None, jobs.get)
                        if v is None:
                            return
                        if v.cancelled:
                            continue
                        await c.get(f"/p{-(-n // QUIC_STEP) * QUIC_STEP}", v)
            except Exception:
                with lock: stats["errors"] += 1
                await asyncio.sleep(0.5)

    asyncio.run(run())


def main():
    global a
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--url", default="http://192.168.1.174:8080")
    p.add_argument("--minutes", type=float, default=4)
    p.add_argument("--swipe-s", type=float, default=3)
    p.add_argument("--video-s", type=float, default=20)
    p.add_argument("--kbps", type=float, default=1400)
    p.add_argument("--ahead", type=int, default=5)
    p.add_argument("--prefetch-kb", type=int, default=1024)
    p.add_argument("--chunk-kb", type=int, default=512)
    p.add_argument("--start-s", type=float, default=1.0, help="seconds of video needed before it can start")
    p.add_argument("--conns", type=int, default=4)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--name", default="phone")
    p.add_argument("--quic", action="store_true", help="fetch over HTTP/3 from the lsquic server (--url https://host:port)")
    a = p.parse_args()
    rng = random.Random(a.seed)
    bps = a.kbps * 1000 / 8
    videos = [Video(i, int(bps * a.video_s * rng.uniform(0.6, 1.4))) for i in range(10_000)]
    jobs, lock = queue.Queue(), threading.Lock()
    stats = {"bytes": 0, "unwatched": 0, "errors": 0}
    for _ in range(a.conns):
        threading.Thread(target=quic_worker if a.quic else worker, args=(jobs, stats, lock), daemon=True).start()

    def want(v, upto):
        """Queue pieces of v up to byte 'upto' that are not yet requested."""
        upto = min(upto, v.size)
        while v.next_off < upto:
            n = min(a.chunk_kb * 1024, upto - v.next_off)
            jobs.put((v, v.next_off, n)); v.next_off += n

    delays = []
    t_end = time.time() + a.minutes * 60
    cur = 0
    for v in videos[:a.ahead + 1]:                      # app open: first piece of the current + next videos
        want(v, a.prefetch_kb * 1024)
    while time.time() < t_end:
        v = videos[cur]
        need = min(v.size, int(bps * a.start_s))
        t0 = time.time()
        while v.got < need and time.time() < t_end:     # start delay: the spinner the customer sees
            time.sleep(0.02)
        delays.append(time.time() - t0)
        want(v, v.size)                                 # playing: fetch the rest of this video
        for nxt in videos[cur + 1:cur + 1 + a.ahead]:   # keep the next videos' first piece ready
            want(nxt, a.prefetch_kb * 1024)
        time.sleep(a.swipe_s)                           # watch, then swipe
        with lock:
            v.cancelled = True                          # queued pieces dropped; in-flight piece still lands
            stats["unwatched"] += max(0, v.got - int(bps * (a.swipe_s + delays[-1])))
        cur += 1
    for _ in range(a.conns):
        jobs.put((None, 0, 0))
    q = lambda xs, f: round(sorted(xs)[min(len(xs) - 1, int(f * len(xs)))], 2) if xs else None
    print(json.dumps({"name": a.name, "swipes": len(delays), "mb_received": round(stats["bytes"] / 1e6, 1),
                      "mb_unwatched_est": round(stats["unwatched"] / 1e6, 1), "errors": stats["errors"],
                      "start_delay_s": {"median": q(delays, .5), "p90": q(delays, .9), "max": q(delays, 1),
                                        "over_1s_pct": round(100 * sum(d > 1 for d in delays) / max(len(delays), 1))}}))


if __name__ == "__main__":
    main()
