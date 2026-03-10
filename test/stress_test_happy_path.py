#!/usr/bin/env python3
"""Full end-to-end flow: coin insert → auth → reconnect → pause → resume → verify time ticks down (11 steps)."""

import asyncio
import json
import time
import traceback
import aiohttp
# we use aiohttp's ws_connect instead of the websockets package to avoid compatibility issues



def _determine_local_ip(remote_host, remote_port=80):
    """Return local IP used to reach *remote_host*:*remote_port*.

    Uses a UDP "connect" trick that doesn't send traffic but lets the OS
    pick the outbound interface.  Returns None on failure.
    """
    import socket
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect((remote_host, remote_port))
            return s.getsockname()[0]
    except Exception:
        return None


async def run_happy_path_test(host, port, fake_ip=None, fake_mac=None, finalization_wait=11):
    """Happy path: coins → auth → reconnect → pause → resume → verify expiry."""
    # if caller didn't specify an IP, auto-detect one from the routing table
    if not fake_ip:
        guessed = _determine_local_ip(host, port)
        if guessed:
            fake_ip = guessed
            print(f"[HAPPY] detected local IP {fake_ip} for server {host}:{port}")
        else:
            print("[HAPPY] warning: could not auto-detect local IP; tests may fail if server requires a clientIp param")

    # if no MAC provided, create a pseudo‑random one so the server log isn't empty
    if not fake_mac:
        # deterministic but varying per process
        import random
        # combine values into a single hashable seed
        seed_val = hash((host, port, fake_ip))
        random.seed(seed_val)
        fake_mac = "02:00:00:%02x:%02x:%02x" % tuple(random.randint(0, 255) for _ in range(3))
        print(f"[HAPPY] using generated MAC address {fake_mac}")

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

    def act(name):
        msg = {"action": name}
        if fake_ip:
            msg["clientIp"] = fake_ip
        return msg

    try:
        # Step 1: GET portal page
        try:
            async with aiohttp.ClientSession() as session:
                async with session.get(url, timeout=5, headers=headers) as resp:
                    assert resp.status == 200, f"[Step 1] Expected status=200, got {resp.status}"
                    print(f"[HAPPY] Step 1: Portal GET -> Expected=200, Got={resp.status} ✓")
        except asyncio.TimeoutError:
            print(f"[HAPPY] ✗ Step 1 timed out contacting {host}:{port} (url={url})")
            return False
        except aiohttp.ClientError as ce:
            print(f"[HAPPY] ✗ Step 1 network error: {ce}")
            return False

        # Step 2: Connect WS and start coin session
        # use aiohttp's ws_connect which avoids the extra_headers issue
        async with aiohttp.ClientSession() as session:
            async with session.ws_connect(ws_url, headers=headers, timeout=5) as ws:
                # Get initial status
                await ws.send_json(act("getStatus"))
                msg1 = await asyncio.wait_for(ws.receive(), timeout=3)
                data1 = json.loads(msg1.data)
                auth = data1.get('sessionEndMillis', 0) > time.time() * 1000
                assert auth == False, f"[Step 2] Expected not authenticated initially, got sessionEndMillis={data1.get('sessionEndMillis')}"
                # If another client's coin insert is still active, wait for it to expire (max 12s)
                coin_wait_deadline = time.time() + 12
                while data1.get('coinInsertTimeLeft', 0) > 0:
                    if time.time() > coin_wait_deadline:
                        raise AssertionError(f"[Step 2] Timed out waiting for existing coin insert to expire")
                    print(f"[HAPPY] Step 2: Another coin insert active ({data1['coinInsertTimeLeft']}s left), waiting...")
                    await asyncio.sleep(1)
                    await ws.send_json(act("getStatus"))
                    msg1 = await asyncio.wait_for(ws.receive(), timeout=3)
                    data1 = json.loads(msg1.data)
                print(f"[HAPPY] Step 2: Initial status -> Expected=not authenticated, Got={auth} ✓")

                # Step 3: Start coin insertion
                await ws.send_json(act("startCoinInsert"))
                msg2 = await asyncio.wait_for(ws.receive(), timeout=3)
                data2 = json.loads(msg2.data)
                coin_active = data2.get('coinInsertTimeLeft', 0) > 0
                assert coin_active == True, f"[Step 3] Expected coinInsertTimeLeft>0, got {data2.get('coinInsertTimeLeft')} — full response: {data2}"
                print(f"[HAPPY] Step 3: Coin session started -> Expected=True, Got={coin_active} ✓")

                # Step 3b: Refresh HTTP portal during coin session (race condition test)
                async with aiohttp.ClientSession() as session:
                    async with session.get(url, timeout=5, headers=headers) as resp:
                        assert resp.status == 200, f"[Step 3b] Expected status=200, got {resp.status}"
                        print(f"[HAPPY] Step 3b: Portal refresh during coin session -> Expected=200, Got={resp.status} ✓")

                # Step 4: Insert 1 coin
                await ws.send_json(act("testInsertCoin"))
                await asyncio.sleep(0.1)
                print(f"[HAPPY] Step 4: 1 coin inserted ✓")

        # Step 4b: Reconnect WebSocket mid-flow (race condition test #6)
        print(f"[HAPPY] Step 4b: Reconnecting WebSocket...")
        async with aiohttp.ClientSession() as session:
            async with session.ws_connect(ws_url, headers=headers, timeout=5) as ws:
                # Verify session state persists across reconnection
                await ws.send_json(act("getStatus"))
                msg4b = await asyncio.wait_for(ws.receive(), timeout=3)
                data4b = json.loads(msg4b.data)

                coin_count_after_reconnect = data4b.get('coinCount')
                is_active = data4b.get('coinInsertTimeLeft', 0) > 0
                assert is_active == True, f"[Step 4b] Expected coinInsertTimeLeft>0, got {data4b.get('coinInsertTimeLeft')}"
                assert coin_count_after_reconnect == 1, f"[Step 4b] Expected coinCount=1, got {coin_count_after_reconnect}"
                print(f"[HAPPY] Step 4b: After reconnect -> Expected=(active=True, count=1), Got=(active={is_active}, count={coin_count_after_reconnect}) ✓")

                # Step 4c: Refresh HTTP portal again
                async with aiohttp.ClientSession() as session2:
                    async with session2.get(url, timeout=5, headers=headers) as resp:
                        assert resp.status == 200, f"[Step 4c] Expected status=200, got {resp.status}"
                        print(f"[HAPPY] Step 4c: Portal refresh after reconnect -> Expected=200, Got={resp.status} ✓")

                # Step 4d: Insert another coin before finalization
                await ws.send_json(act("testInsertCoin"))
                await asyncio.sleep(0.1)
                print(f"[HAPPY] Step 4d: 1 more coin inserted (total 2) ✓")

                # Step 5: Wait for finalization (10s timeout + buffer)
                await asyncio.sleep(finalization_wait)
                # server sends {"type":"authenticated"} on success or {"type":"error"} on failure
                authenticated = False
                end_time = asyncio.get_running_loop().time() + 5
                ignored = 0
                try:
                    while asyncio.get_running_loop().time() < end_time:
                        try:
                            msg3 = await asyncio.wait_for(ws.receive(), timeout=2)
                            payload = json.loads(msg3.data)
                            if payload.get('type') == 'authenticated':
                                authenticated = True
                                break
                            elif payload.get('type') == 'error':
                                raise AssertionError(f"[Step 5] Server returned error: {payload.get('message')} — {payload.get('detail', '')}")
                            else:
                                ignored += 1
                        except asyncio.TimeoutError:
                            continue
                        except Exception as inner_e:
                            print(f"[HAPPY] Step 5: error reading msg: {type(inner_e).__name__}: {inner_e}")
                            raise
                    if ignored > 0:
                        print(f"[HAPPY] Step 5: ignored {ignored} intermediate messages")
                    if not authenticated:
                        raise AssertionError("[Step 5] Expected authenticated message within 5s, got timeout")
                except Exception as finalize_e:
                    print(f"[HAPPY] Step 5 finalization error: {type(finalize_e).__name__}: {finalize_e}")
                    raise
                print(f"[HAPPY] Step 5: Finalization -> Expected=authenticated, Got=authenticated ✓")

                # Step 6: Verify authenticated
                await ws.send_json(act("getStatus"))
                msg4 = await asyncio.wait_for(ws.receive(), timeout=3)
                data4 = json.loads(msg4.data)
                initial_expires = max(0, data4.get("sessionEndMillis", 0) - int(time.time() * 1000))
                min_expected_ms = 5 * 2 * 60 * 1000 - 10000  # 2 coins * 5 min each, minus 10s tolerance for round-trip
                assert initial_expires >= min_expected_ms, f"[Step 6] Expected expiresInMillis>={min_expected_ms}, got {initial_expires}"
                print(f"[HAPPY] Step 6: Authenticated -> Expected=expiresInMillis>={min_expected_ms}, Got={initial_expires} ✓")

                # Step 7: Pause
                paused_remaining = None
                if ws.closed:
                    raise AssertionError("[Step 7] Expected WebSocket open, got closed")
                try:
                    await ws.send_json(act("pause"))
                    msg5 = await asyncio.wait_for(ws.receive(), timeout=3)
                    data5 = json.loads(msg5.data)
                    paused_remaining = data5.get("pausedRemainingMillis", 0)
                    session_end = data5.get("sessionEndMillis", 0)
                    print(f"[HAPPY] Step 7: Paused, pausedRemainingMillis={paused_remaining}")
                    assert paused_remaining > 0, f"[Step 7] Expected pausedRemainingMillis>0, got {paused_remaining}"
                    assert session_end == 0 or session_end <= time.time() * 1000, f"[Step 7] Expected sessionEndMillis expired after pause, got {session_end}"
                    print(f"[HAPPY] Step 7: Paused -> Expected=(remaining>0, sessionEndMillis expired), Got=(remaining={paused_remaining}, sessionEndMillis={session_end}) ✓")
                except Exception as e:
                    print(f"[HAPPY] Step 7: pause/recv error: {e}")
                    raise

                # Step 7b: Verify pause file exists
                fake_mac_clean = fake_mac.replace(":", "") if fake_mac else "000000000000"
                pause_file_url = f"http://{host}:{port}/pause_{fake_mac_clean}.dat"
                async with aiohttp.ClientSession() as session_http:
                    async with session_http.get(pause_file_url, timeout=5, headers=headers) as resp:
                        if resp.status == 200:
                            pause_data = await resp.read()
                            assert len(pause_data) == 4, f"[Step 7b] Expected pause file size=4 bytes, got {len(pause_data)}"
                            print(f"[HAPPY] Step 7b: Pause file -> Expected=4 bytes, Got={len(pause_data)} ✓")
                        else:
                            print(f"[HAPPY] Step 7b: Pause file NOT found (status={resp.status})")

                # Step 8: Wait 2 seconds
                await asyncio.sleep(2)

                # Step 9: Resume
                resumed_expires = None
                if ws.closed:
                    raise AssertionError("[Step 9] Expected WebSocket open, got closed")
                try:
                    await ws.send_json(act("resume"))
                    msg6 = await asyncio.wait_for(ws.receive(), timeout=3)
                    data6 = json.loads(msg6.data)
                    resumed_expires = max(0, data6.get("sessionEndMillis", 0) - int(time.time() * 1000))
                    assert resumed_expires > 0, f"[Step 9] Expected expiresInMillis>0 after resume, got {resumed_expires}"
                    if paused_remaining is not None:
                        tolerance_ms = paused_remaining * 0.05
                        time_diff = abs(resumed_expires - paused_remaining)
                        assert time_diff <= tolerance_ms, (
                            f"[Step 9] Expected resumed time within 5% of paused time (±{tolerance_ms:.0f}ms), got diff={time_diff}ms"
                        )
                    print(f"[HAPPY] Step 9: Resumed -> Expected=expiresInMillis>0, Got={resumed_expires} ✓")
                except Exception as e:
                    print(f"[HAPPY] Step 9: resume/recv error: {e}")
                    raise

                # Step 10: Verify time ticks down
                await asyncio.sleep(3)
                if ws.closed:
                    raise AssertionError("[Step 10] Expected WebSocket open, got closed")
                try:
                    await ws.send_json(act("getStatus"))
                    msg7 = await asyncio.wait_for(ws.receive(), timeout=3)
                    data7 = json.loads(msg7.data)
                    remaining = max(0, data7.get("sessionEndMillis", 0) - int(time.time() * 1000))
                    assert remaining > 0, f"[Step 10] Expected expiresInMillis>0, got {remaining}"
                    if resumed_expires is not None:
                        assert remaining < resumed_expires, f"[Step 10] Expected remaining<{resumed_expires}, got {remaining}"
                        elapsed_ms = 3000
                        expected_remaining = resumed_expires - elapsed_ms
                        tolerance_ms = resumed_expires * 0.05
                        assert abs(remaining - expected_remaining) <= tolerance_ms, (
                            f"[Step 10] Expected ~{expected_remaining:.0f}ms remaining (±5%={tolerance_ms:.0f}ms), got {remaining}ms"
                        )
                        print(f"[HAPPY] Step 10: Final status -> Expected=~{expected_remaining:.0f}ms (±5%), Got={remaining}ms ✓")
                    else:
                        print(f"[HAPPY] Step 10: Final status -> Got={remaining}ms ✓")
                except Exception as e:
                    print(f"[HAPPY] Step 10: getStatus error: {e}")
                    raise

                # Step 11: Verify session is still active after resume (Omada cache is source of truth)
                await asyncio.sleep(0.5)
                await ws.send_json(act("getStatus"))
                msg11 = await asyncio.wait_for(ws.receive(), timeout=3)
                data11 = json.loads(msg11.data)
                expires11 = max(0, data11.get("sessionEndMillis", 0) - int(time.time() * 1000))
                assert expires11 > 0, f"[Step 11] Expected sessionEndMillis in future after resume, got {data11.get('sessionEndMillis')}"
                print(f"[HAPPY] Step 11: Session active after resume -> expiresInMillis={expires11} ✓")

        print("\n[HAPPY] ✓✓✓ All steps passed ✓✓✓\n")
        return True
    except Exception as e:
        # report error with full details if message is empty
        import traceback
        if not str(e):
            print(f"\n[HAPPY] ✗ Error: {type(e).__name__} (empty message)")
            print("[HAPPY] Full traceback:")
            traceback.print_exc()
        else:
            print(f"\n[HAPPY] ✗ Error: {e}")
        return False


if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(description="EllaFi happy path test")
    parser.add_argument("--host", default="ellafi.local", help="server host (default: ellafi.local)")
    parser.add_argument("--port", type=int, default=80, help="server port (default 80)")
    parser.add_argument("--mac", help="client MAC for testing (optional)", default=None)
    parser.add_argument("--ip", help="client IP to present via query param (optional)", default=None)
    parser.add_argument("--finalization-wait", type=int, default=11,
                        help="seconds to wait for coin session finalization (default 11)")
    args = parser.parse_args()

    result = asyncio.run(run_happy_path_test(args.host, args.port,
                                            fake_ip=args.ip,
                                            fake_mac=args.mac,
                                            finalization_wait=args.finalization_wait))
    exit(0 if result else 1)
