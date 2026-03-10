#!/usr/bin/env python3
"""Chaos load test: spawns N concurrent clients firing random actions (startCoinInsert, testInsertCoin, getStatus, pause, resume) to verify the server survives concurrent, out-of-order requests without crashing or deadlocking.

Coin insertion has the highest probability so clients will occasionally
reach an authenticated state and exercise pause/resume meaningfully.

Usage:
    python3 stress_test.py [--host HOST] [--port PORT] [--clients N]
                             [--duration SECONDS]

Dependencies:
    pip install aiohttp
"""

import argparse
import asyncio
import json
import random
import sys
import time
import traceback

import aiohttp
import ipaddress



async def run_client(idx, host, port, duration, fake_ip=None, fake_mac=None):
    # construct urls with optional query params
    qs = []
    if fake_mac:
        qs.append(f"clientMac={fake_mac}")
    if fake_ip:
        qs.append(f"clientIp={fake_ip}")
    qstr = "?" + "&".join(qs) if qs else ""
    url = f"http://{host}:{port}/{qstr}"
    ws_url = f"ws://{host}:{port}/ws{qstr}"

    headers = {}
    if fake_ip:
        headers["X-Forwarded-For"] = fake_ip
    if fake_mac:
        headers["X-Client-Mac"] = fake_mac

    try:
        # initial HTTP GET
        async with aiohttp.ClientSession() as session:
            async with session.get(url, timeout=5, headers=headers) as resp:
                assert resp.status == 200, f"[{idx}] Expected HTTP status=200, got {resp.status}"
                text = await resp.text()
                print(f"[{idx}] HTTP GET -> Expected=200, Got={resp.status}, len={len(text)} ✓")

        # open websocket using aiohttp
        async with aiohttp.ClientSession() as session:
            async with session.ws_connect(ws_url, headers=headers, timeout=5) as ws:
                def act(name):
                    msg = {"action": name}
                    if fake_ip:
                        msg["clientIp"] = fake_ip
                    return msg

                await ws.send_json(act("getStatus"))
                start = time.time()
                while time.time() - start < duration:
                    # drain any pending messages (non-blocking 0.5s tick)
                    try:
                        msg = await asyncio.wait_for(ws.receive(), timeout=0.5)
                        print(f"[{idx}] WS recv: {msg.data}")
                    except asyncio.TimeoutError:
                        pass
                    # randomly fire an action each tick
                    # coin actions weighted higher so sessions occasionally authenticate
                    r = random.random()
                    if r < 0.30:
                        await ws.send_json(act("testInsertCoin"))
                    elif r < 0.50:
                        await ws.send_json(act("startCoinInsert"))
                    elif r < 0.65:
                        await ws.send_json(act("getStatus"))
                    elif r < 0.75:
                        await ws.send_json(act("pause"))
                    elif r < 0.85:
                        await ws.send_json(act("resume"))
                    # else: 15% chance — do nothing this tick
                print(f"[{idx}] WS session completed ✓")
    except Exception as e:
        import traceback
        print(f"[{idx}] ✗ Error: {type(e).__name__}: {e}")
        traceback.print_exc()


async def main():
    parser = argparse.ArgumentParser(description="EllaFi stress tester")
    parser.add_argument("--host", default="ellafi.local", help="server host (default: ellafi.local)")
    parser.add_argument("--port", type=int, default=80, help="server port (default 80)")
    parser.add_argument("--clients", type=int, default=10, help="number of concurrent clients (default 10)")
    parser.add_argument("--duration", type=int, default=10, help="duration each client stays connected (sec) (default 10)")
    parser.add_argument("--start-ip", help="first source IP to bind (e.g. 127.0.0.2)", default=None)
    parser.add_argument("--start-mac", help="first MAC to use in query params (00:11:22:33:44:55)", default=None)
    args = parser.parse_args()

    # build lists of fake IPs/MACs — random base if not specified
    import random as _random
    if not args.start_ip:
        args.start_ip = f"10.{_random.randint(1,254)}.{_random.randint(1,254)}.1"
        print(f"[chaos] using random start-ip: {args.start_ip}")
    if not args.start_mac:
        args.start_mac = "02:00:%02x:%02x:%02x:01" % (_random.randint(0,255), _random.randint(0,255), _random.randint(0,255))
        print(f"[chaos] using random start-mac: {args.start_mac}")

    fake_ips = []
    fake_macs = []

    try:
        base = ipaddress.IPv4Address(args.start_ip)
        for i in range(args.clients):
            fake_ips.append(str(base + i))
    except Exception:
        print("invalid start-ip")
        sys.exit(1)

    parts = args.start_mac.split(":")
    if len(parts) == 6:
        base_bytes = [int(p, 16) for p in parts]
        for i in range(args.clients):
            b = base_bytes.copy()
            b[5] = (b[5] + i) & 0xff
            fake_macs.append(":".join(f"{x:02x}" for x in b))
    else:
        print("invalid start-mac")
        sys.exit(1)

    tasks = []
    for i in range(args.clients):
        ip = fake_ips[i] if i < len(fake_ips) else None
        mac = fake_macs[i] if i < len(fake_macs) else None
        tasks.append(run_client(i, args.host, args.port, args.duration, ip, mac))

    await asyncio.gather(*tasks)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[stress_test] Interrupted by user")
        sys.exit(0)
    except Exception as e:
        import traceback
        print(f"\n[stress_test] Error: {type(e).__name__}: {e}")
        traceback.print_exc()
        sys.exit(1)
