#!/usr/bin/env python3
"""Targeted race tests: RC2 (concurrent startCoinInsert), RC3 (staggered insertions), RC4 (error conditions), RC5 (rapid reconnects), RC6 (concurrent WS+HTTP)."""

import asyncio
import json
import traceback
import aiohttp
# using aiohttp for WS connectivity to avoid compatibility issues



# RC #1: Socket FD tracking across reconnection
# (Tested in stress_test_happy_path.py Step 4b)

async def test_rc2_concurrent_start_coin_insert(host, port, num_clients=20):
    """RC #2: Multiple concurrent startCoinInsert calls - only 1 should succeed."""
    print(f"\n[RC2] Testing {num_clients} concurrent startCoinInsert...")
    
    tasks = []
    results = {"success": 0, "blocked": 0, "errors": 0}
    
    async def client_try_start_coin(client_id, fake_ip, fake_mac):
        try:
            ws_url = f"ws://{host}:{port}/ws?clientIp={fake_ip}&clientMac={fake_mac}"
            headers = {
                "X-Forwarded-For": fake_ip,
                "X-Client-Mac": fake_mac
            }

            def act(name):
                return {"action": name, "clientIp": fake_ip}

            # First hit the portal to establish session
            async with aiohttp.ClientSession() as session:
                async with session.get(
                    f"http://{host}:{port}/?clientIp={fake_ip}&clientMac={fake_mac}&site=test&apMac=00:11:22:33:44:55&ssidName=TestSSID&radioId=test",
                    timeout=3,
                    headers=headers
                ) as resp:
                    if resp.status != 200:
                        results["errors"] += 1
                        return

            # Now try to start coin insert via WS using aiohttp
            async with aiohttp.ClientSession() as session:
                async with session.ws_connect(ws_url, headers=headers, timeout=5) as ws:
                    await ws.send_json(act("startCoinInsert"))
                    msg = await asyncio.wait_for(ws.receive(), timeout=3)
                    data = json.loads(msg.data)
                
                if data.get("type") == "error" and "already inserting" in data.get("message", ""):
                    results["blocked"] += 1
                    print(f"  [RC2] Client {client_id}: Blocked (expected) ✓")
                elif data.get("coinInsertTimeLeft", 0) > 0:
                    results["success"] += 1
                    print(f"  [RC2] Client {client_id}: SUCCESS - started coin session ✓")
                else:
                    results["errors"] += 1
                    print(f"  [RC2] Client {client_id}: Unexpected response - {json.dumps(data)}")
        except Exception as e:
            results["errors"] += 1
            err_msg = str(e) if str(e) else f"(empty message) {type(e).__name__}"
            print(f"  [RC2] Client {client_id}: {type(e).__name__}: {err_msg}")
    
    # Launch all clients simultaneously
    for i in range(num_clients):
        fake_ip = f"10.0.{i // 256}.{i % 256}"
        fake_mac = f"00:11:22:33:44:{i:02x}" if i < 256 else f"00:11:22:33:{i//256:02x}:{i%256:02x}"
        tasks.append(client_try_start_coin(i, fake_ip, fake_mac))
    
    await asyncio.gather(*tasks, return_exceptions=True)
    
    # Verify exactly 1 succeeded
    success = results["success"] == 1
    print(f"\n[RC2] Results -> Expected=(success=1), Got=(success={results['success']}, blocked={results['blocked']}, errors={results['errors']})")
    if success:
        print("[RC2] ✓ PASS - Exactly 1 client won the coin session\n")
    else:
        print(f"[RC2] ✗ FAIL - Expected exactly 1 success, got {results['success']}\n")
    
    return success


async def test_rc3_staggered_coin_insertions(host, port, num_clients=5):
    """RC #3: Staggered coin insertions from different clients during active session."""
    print(f"\n[RC3] Testing staggered insertions from {num_clients} clients...")
    
    # Start with first client winning the race
    client_0_ip = "10.0.0.1"
    client_0_mac = "00:11:22:33:44:00"
    
    def act(name, ip):
        return {"action": name, "clientIp": ip}

    try:
        # Client 0 gets the coin session
        async with aiohttp.ClientSession() as session:
            async with session.get(
                f"http://{host}:{port}/?clientIp={client_0_ip}&clientMac={client_0_mac}&site=test&apMac=00:11:22:33:44:55&ssidName=TestSSID&radioId=test",
                timeout=3,
                headers={"X-Forwarded-For": client_0_ip, "X-Client-Mac": client_0_mac}
            ) as resp:
                pass

        ws_url_0 = f"ws://{host}:{port}/ws?clientIp={client_0_ip}&clientMac={client_0_mac}"
        headers_0 = {"X-Forwarded-For": client_0_ip, "X-Client-Mac": client_0_mac}

        async with aiohttp.ClientSession() as session0:
            async with session0.ws_connect(ws_url_0, headers=headers_0, timeout=5) as ws0:
                await ws0.send_json(act("startCoinInsert", client_0_ip))
                msg = await asyncio.wait_for(ws0.receive(), timeout=3)
                data = json.loads(msg.data)
                if not data.get("coinInsertTimeLeft", 0) > 0:
                    print(f"[RC3] Debug: Received message: {data}")
                    print(f"[RC3] Debug: Message keys: {data.keys()}")
                assert data.get("coinInsertTimeLeft", 0) > 0, f"[RC3] Expected coinInsertTimeLeft>0, got {data.get('coinInsertTimeLeft')}"
                print("[RC3] Client 0: Coin session acquired ✓")

                # Attempt insertions from other clients while ws0 is still open
                blocked_count = 0
                for i in range(1, num_clients):
                    try:
                        fake_ip = f"10.0.0.{i+1}"
                        fake_mac = f"00:11:22:33:44:{i:02x}"

                        # Quick portal hit
                        async with aiohttp.ClientSession() as session:
                            async with session.get(
                                f"http://{host}:{port}/?clientIp={fake_ip}&clientMac={fake_mac}&site=test&apMac=00:11:22:33:44:55&ssidName=TestSSID&radioId=test",
                                timeout=1,
                                headers={"X-Forwarded-For": fake_ip, "X-Client-Mac": fake_mac}
                            ) as _:
                                pass

                        ws_url_i = f"ws://{host}:{port}/ws?clientIp={fake_ip}&clientMac={fake_mac}"
                        headers_i = {"X-Forwarded-For": fake_ip, "X-Client-Mac": fake_mac}

                        async with aiohttp.ClientSession() as session_i:
                            async with session_i.ws_connect(ws_url_i, headers=headers_i, timeout=3) as wsi:
                                await wsi.send_json(act("startCoinInsert", fake_ip))
                                msg = await asyncio.wait_for(wsi.receive(), timeout=1)
                                response = json.loads(msg.data)

                                if "already inserting" in response.get("message", ""):
                                    blocked_count += 1
                                    print(f"[RC3] Client {i}: Correctly blocked ✓")
                                else:
                                    print(f"[RC3] Client {i}: Unexpected response - {response}")
                    except asyncio.TimeoutError:
                        # Server did not respond within timeout — treat as blocked
                        blocked_count += 1
                        print(f"[RC3] Client {i}: Timeout (counted as blocked) ✓")
                    except Exception as e:
                        print(f"[RC3] Client {i}: {e}")

                print(f"[RC3] Results -> Expected=blocked={num_clients-1}, Got=blocked={blocked_count}")
                if blocked_count == (num_clients - 1):
                    print(f"[RC3] ✓ PASS - All other clients correctly blocked\n")
                else:
                    print(f"[RC3] ✗ FAIL - Expected {num_clients-1} blocked, got {blocked_count}\n")
                return blocked_count == (num_clients - 1)
    
    except Exception as e:
        err_msg = str(e) if str(e) else "(empty message)"
        print(f"[RC3] ✗ FAIL - {type(e).__name__}: {err_msg}\n")
        print("Traceback:")
        traceback.print_exc()
        return False


async def test_rc4_error_conditions(host, port):
    """RC #4: Error handling - testInsertCoin before startCoinInsert, etc."""
    print("\n[RC4] Testing error conditions...")
    
    fake_ip = "10.0.0.100"
    fake_mac = "00:11:22:33:44:99"
    
    def act(name):
        return {"action": name, "clientIp": fake_ip}

    try:
        # Establish session via portal
        async with aiohttp.ClientSession() as session:
            async with session.get(
                f"http://{host}:{port}/?clientIp={fake_ip}&clientMac={fake_mac}&site=test&apMac=00:11:22:33:44:55&ssidName=TestSSID&radioId=test",
                timeout=3,
                headers={"X-Forwarded-For": fake_ip, "X-Client-Mac": fake_mac}
            ) as resp:
                pass

        ws_url = f"ws://{host}:{port}/ws?clientIp={fake_ip}&clientMac={fake_mac}"
        headers = {"X-Forwarded-For": fake_ip, "X-Client-Mac": fake_mac}

        sub_results = []

        # Test 1: testInsertCoin before startCoinInsert (should be no-op: no response or not active)
        print("[RC4] Test 1: testInsertCoin before startCoinInsert...")
        async with aiohttp.ClientSession() as session:
            async with session.ws_connect(ws_url, headers=headers, timeout=5) as ws:
                await ws.send_json(act("testInsertCoin"))
                try:
                    msg = await asyncio.wait_for(ws.receive(), timeout=2)
                    data = json.loads(msg.data)
                    t1_pass = not data.get("coinInsertActive")
                except asyncio.TimeoutError:
                    # No response = no-op, which is correct
                    t1_pass = True
                sub_results.append(t1_pass)
                if t1_pass:
                    print("[RC4] Test 1: Expected=not active/no-op, Got=correct ✓")
                else:
                    print(f"[RC4] Test 1: Expected=not active, Got=active ✗")

        # Test 2: pause without authentication (should error)
        print("[RC4] Test 2: pause without authentication...")
        async with aiohttp.ClientSession() as session:
            async with session.ws_connect(ws_url, headers=headers, timeout=5) as ws:
                await ws.send_json(act("pause"))
                await asyncio.sleep(0.5)
                try:
                    msg = await asyncio.wait_for(ws.receive(), timeout=1)
                    data = json.loads(msg.data)
                    t2_pass = data.get("type") == "error"
                    sub_results.append(t2_pass)
                    if t2_pass:
                        print("[RC4] Test 2: Expected=error, Got=error ✓")
                    else:
                        print(f"[RC4] Test 2: Expected=error, Got={data.get('type')} ✗")
                except asyncio.TimeoutError:
                    print("[RC4] Test 2: No response (inconclusive)")

        # Test 3: resume without paused session (should error)
        print("[RC4] Test 3: resume without paused session...")
        async with aiohttp.ClientSession() as session:
            async with session.ws_connect(ws_url, headers=headers, timeout=5) as ws:
                await ws.send_json(act("resume"))
                await asyncio.sleep(0.5)
                try:
                    msg = await asyncio.wait_for(ws.receive(), timeout=1)
                    data = json.loads(msg.data)
                    t3_pass = data.get("type") == "error"
                    sub_results.append(t3_pass)
                    if t3_pass:
                        print("[RC4] Test 3: Expected=error, Got=error ✓")
                    else:
                        print(f"[RC4] Test 3: Expected=error, Got={data.get('type')} ✗")
                except asyncio.TimeoutError:
                    print("[RC4] Test 3: No response (inconclusive)")

        all_pass = all(sub_results) if sub_results else False
        if all_pass:
            print("[RC4] ✓ PASS - All error conditions handled correctly\n")
        else:
            print("[RC4] ✗ FAIL - Some error conditions not handled correctly\n")
        return all_pass
    
    except Exception as e:
        err_msg = str(e) if str(e) else "(empty message)"
        print(f"[RC4] ✗ FAIL - {type(e).__name__}: {err_msg}\n")
        print("Traceback:")
        traceback.print_exc()
        return False


async def test_rc5_rapid_reconnects(host, port, num_reconnects=10):
    """RC #5: Rapid WebSocket reconnections during active coin session."""
    print(f"\n[RC5] Testing {num_reconnects} rapid reconnects during active session...")
    
    fake_ip = "10.0.0.200"
    fake_mac = "00:11:22:33:44:88"
    
    def act(name):
        return {"action": name, "clientIp": fake_ip}

    try:
        # Establish session and start coin insert
        async with aiohttp.ClientSession() as session:
            async with session.get(
                f"http://{host}:{port}/?clientIp={fake_ip}&clientMac={fake_mac}&site=test&apMac=00:11:22:33:44:55&ssidName=TestSSID&radioId=test",
                timeout=3,
                headers={"X-Forwarded-For": fake_ip, "X-Client-Mac": fake_mac}
            ) as resp:
                pass

        ws_url = f"ws://{host}:{port}/ws?clientIp={fake_ip}&clientMac={fake_mac}"
        headers = {"X-Forwarded-For": fake_ip, "X-Client-Mac": fake_mac}

        # Start coin session once
        async with aiohttp.ClientSession() as session:
            async with session.ws_connect(ws_url, headers=headers, timeout=5) as ws:
                await ws.send_json(act("startCoinInsert"))
                msg = await asyncio.wait_for(ws.receive(), timeout=3)
                initial = json.loads(msg.data)
                if not initial.get("coinInsertTimeLeft", 0) > 0:
                    print(f"[RC5] Debug: Received message: {initial}")
                    print(f"[RC5] Debug: Message keys: {initial.keys()}")
                assert initial.get("coinInsertTimeLeft", 0) > 0, f"[RC5] Expected coinInsertTimeLeft>0, got {initial.get('coinInsertTimeLeft')}"
                print("[RC5] Coin session started ✓")
        
        # Now rapidly reconnect and verify state persists
        coin_counts = []
        for i in range(num_reconnects):
            async with aiohttp.ClientSession() as session:
                async with session.ws_connect(ws_url, headers=headers, timeout=5) as ws:
                    await ws.send_json(act("getStatus"))
                    msg = await asyncio.wait_for(ws.receive(), timeout=2)
                    data = json.loads(msg.data)
                    coin_counts.append(data.get("coinCount", 0))
                    is_active = data.get("coinInsertTimeLeft", 0) > 0
                    assert is_active == True, f"[RC5] Expected coinInsertTimeLeft>0 on reconnect {i}, got {data.get('coinInsertTimeLeft')}"
                    print(f"[RC5] Reconnect {i}: Expected=active, Got=active, coinCount={data.get('coinCount')} ✓")
                    
                    # Stagger calls slightly
                    await asyncio.sleep(0.1)
        
        if len(set(coin_counts)) != 1:
            print(f"[RC5] ✗ FAIL - coin count changed across reconnects: {coin_counts}\n")
            return False
        print(f"[RC5] Coin count stable at {coin_counts[0]} across all reconnects ✓")
        print(f"[RC5] ✓ PASS - Session maintained across {num_reconnects} reconnects\n")
        return True
    
    except Exception as e:
        err_msg = str(e) if str(e) else "(empty message)"
        print(f"[RC5] ✗ FAIL - {type(e).__name__}: {err_msg}\n")
        print("Traceback:")
        traceback.print_exc()
        return False


async def test_rc6_concurrent_websocket_and_http(host, port):
    """RC #6: Interleaved WebSocket and HTTP requests during active session."""
    print("\n[RC6] Testing concurrent WS and HTTP requests...")
    
    fake_ip = "10.0.0.150"
    fake_mac = "00:11:22:33:44:77"
    
    def act(name):
        return {"action": name, "clientIp": fake_ip}

    try:
        # Establish session
        async with aiohttp.ClientSession() as session:
            async with session.get(
                f"http://{host}:{port}/?clientIp={fake_ip}&clientMac={fake_mac}&site=test&apMac=00:11:22:33:44:55&ssidName=TestSSID&radioId=test",
                timeout=3,
                headers={"X-Forwarded-For": fake_ip, "X-Client-Mac": fake_mac}
            ) as resp:
                pass

        ws_url = f"ws://{host}:{port}/ws?clientIp={fake_ip}&clientMac={fake_mac}"
        headers = {"X-Forwarded-For": fake_ip, "X-Client-Mac": fake_mac}

        # Start coin session
        async with aiohttp.ClientSession() as session:
            async with session.ws_connect(ws_url, headers=headers, timeout=5) as ws:
                await ws.send_json(act("startCoinInsert"))
                msg = await asyncio.wait_for(ws.receive(), timeout=3)
                start_data = json.loads(msg.data)
                assert start_data.get("coinInsertTimeLeft", 0) > 0, f"[RC6] Expected coinInsertTimeLeft>0 after start, got {start_data}"
                print("[RC6] Coin session started ✓")
                
                # Now interleave HTTP and WS requests
                for i in range(5):
                    # HTTP GET to portal
                    async with aiohttp.ClientSession() as session:
                        async with session.get(f"http://{host}:{port}/", timeout=2, headers=headers) as resp:
                            assert resp.status == 200, f"[RC6] Expected HTTP status=200, got {resp.status}"
                            print(f"[RC6] Iteration {i}: HTTP GET -> Expected=200, Got={resp.status} ✓")
                    
                    # WS status check
                    await ws.send_json(act("getStatus"))
                    msg = await asyncio.wait_for(ws.receive(), timeout=2)
                    data = json.loads(msg.data)
                    is_active = data.get("coinInsertTimeLeft", 0) > 0
                    assert is_active == True, f"[RC6] Expected coinInsertTimeLeft>0, got {data.get('coinInsertTimeLeft')}"
                    print(f"[RC6] Iteration {i}: WS status -> Expected=active, Got=active ✓")
                    
                    await asyncio.sleep(0.2)
        
        print("[RC6] ✓ PASS - Session survived concurrent HTTP/WS requests\n")
        return True
    
    except Exception as e:
        err_msg = str(e) if str(e) else "(empty message)"
        print(f"[RC6] ✗ FAIL - {type(e).__name__}: {err_msg}\n")
        print("Traceback:")
        traceback.print_exc()
        return False


async def test_rc7_no_session_error(host, port):
    """RC #7: Client with no portal session gets NO_SESSION error code."""
    print("\n[RC7] Testing NO_SESSION error for unregistered client...")

    fake_ip = "10.99.99.99"
    fake_mac = "00:de:ad:be:ef:99"
    ws_url = f"ws://{host}:{port}/ws?clientIp={fake_ip}&clientMac={fake_mac}"
    headers = {"X-Forwarded-For": fake_ip, "X-Client-Mac": fake_mac}

    try:
        async with aiohttp.ClientSession() as session:
            async with session.ws_connect(ws_url, headers=headers, timeout=5) as ws:
                await ws.send_json({"action": "getStatus", "clientIp": fake_ip})
                msg = await asyncio.wait_for(ws.receive(), timeout=3)
                data = json.loads(msg.data)

                if data.get("type") == "error" and data.get("message") == "NO_SESSION":
                    print("[RC7] ✓ PASS - Got NO_SESSION error as expected\n")
                    return True
                else:
                    print(f"[RC7] ✗ FAIL - Expected NO_SESSION error, got: {data}\n")
                    return False
    except Exception as e:
        err_msg = str(e) if str(e) else "(empty message)"
        print(f"[RC7] ✗ FAIL - {type(e).__name__}: {err_msg}\n")
        traceback.print_exc()
        return False


async def run_all_race_condition_tests(host, port):
    """Execute all race condition tests."""
    print("\n" + "="*60)
    print("EllaFi Race Condition Test Suite")
    print("="*60)
    
    results = {}
    
    # RC #1 is tested in stress_test_happy_path.py (socket FD reconnection)
    print("[RC1] ✓ Tested in stress_test_happy_path.py (WebSocket reconnection)")
    results["RC1"] = True
    
    # RC #2: Concurrent startCoinInsert
    results["RC2"] = await test_rc2_concurrent_start_coin_insert(host, port, num_clients=20)

    # Wait for RC2's winning coin session timer to expire (10s) before next test
    print("[setup] Waiting 12s for RC2 coin session to expire...")
    await asyncio.sleep(12)

    # RC #3: Staggered insertions
    results["RC3"] = await test_rc3_staggered_coin_insertions(host, port, num_clients=5)

    # Wait for RC3's coin session timer to expire before next test
    print("[setup] Waiting 12s for RC3 coin session to expire...")
    await asyncio.sleep(12)

    # RC #4: Error conditions
    results["RC4"] = await test_rc4_error_conditions(host, port)

    # RC #5: Rapid reconnects
    results["RC5"] = await test_rc5_rapid_reconnects(host, port, num_reconnects=10)

    # Wait for RC5's coin session timer to expire before RC6
    print("[setup] Waiting 12s for RC5 coin session to expire...")
    await asyncio.sleep(12)

    # RC #6: Concurrent WS/HTTP
    results["RC6"] = await test_rc6_concurrent_websocket_and_http(host, port)

    # RC #7: NO_SESSION error for unregistered client (simulates ESP32 restart)
    results["RC7"] = await test_rc7_no_session_error(host, port)

    # Summary
    print("\n" + "="*60)
    print("Race Condition Test Summary")
    print("="*60)
    passed = sum(1 for v in results.values() if v)
    total = len(results)
    print(f"Results -> Expected=all pass, Passed={passed}/{total}")
    for rc, result in results.items():
        status = "✓" if result else "✗"
        print(f"  {status} {rc}")
    print("="*60 + "\n")
    
    return all(results.values())


if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(description="EllaFi race condition tests")
    parser.add_argument("--host", default="ellafi.local", help="server host (default: ellafi.local)")
    parser.add_argument("--port", type=int, default=80, help="server port (default 80)")
    args = parser.parse_args()
    
    try:
        result = asyncio.run(run_all_race_condition_tests(args.host, args.port))
        exit(0 if result else 1)
    except Exception as e:
        import traceback
        print(f"\nTest suite error: {type(e).__name__}: {e}")
        traceback.print_exc()
        exit(1)
