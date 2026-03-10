# EllaFi Test Suite

Comprehensive testing framework for the EllaFi ESP32 coin-operated WiFi hotspot portal.

## Test Components

### 1. **stress_test.py** - Concurrent Client Load Testing
Simulates multiple concurrent clients with unique IPs/MACs stressing the portal.

**Features:**
- Configurable number of concurrent clients (default 10)
- IP/MAC spoofing via X-Forwarded-For headers

> No local network aliases are required when the ESP32 server is remote; tests simply pass a `clientIp` query parameter which the firmware honors.

**Usage:**
```bash
# Run 20 concurrent clients for 30 seconds each
python3 test/stress_test.py --host localhost --clients 20 --duration 30

# Optional IP/MAC overrides via query parameters (no local aliases required)
sudo python3 test/stress_test.py --host localhost --clients 20 \
  --start-ip 10.0.0.2 --start-mac 00:11:22:33:44:00
```

**Arguments:**
- `--host` (required): Server address
- `--port` (default 80): HTTP/WS port
- `--clients` (default 10): Number of concurrent clients
- `--duration` (default 10): Seconds per client
- `--start-ip` (optional): Base source IP used in query params (e.g., 10.0.0.2). Only the query parameter is needed; no local loopbacks are necessary when the ESP32 is remote.
- `--start-mac` (optional): First MAC address (e.g., 00:11:22:33:44:00)

---

### 2. **stress_test_happy_path.py** - Happy Path Validation
Complete end-to-end test with WebSocket reconnection verification.

**Test Flow (11 steps):**
1. Portal GET → verify 200
2. WS connect → getStatus
3. startCoinInsert → begin coin session
4. Portal GET refresh (stress test)
5. Insert 1 test coin
6. **RECONNECT WebSocket** (Race Condition #1 test)
7. Verify session persists (coin count, socket FD)
8. Portal GET again
9. Insert 2nd coin
10. Wait for finalization (10s timeout) → Auth with Omada
11. Pause → Wait 2s → Resume → Verify countdown
12. Check pause file on disk
13. Check auth_sessions.json on disk

**Usage:**
```bash
# Basic run (random IP/MAC)
python3 test/stress_test_happy_path.py --host localhost

# Specific client using MAC and/or IP
python3 test/stress_test_happy_path.py --host localhost --mac 00:11:22:33:44:55
python3 test/stress_test_happy_path.py --host localhost --ip 10.0.0.5 --mac 00:11:22:33:44:55

> When testing against a remote ESP32, supplying `--ip` or `--mac` ensures
the server can correctly associate requests with a session.
```

**Arguments:**
- `--host` (required): Server address
- `--port` (default 80): HTTP/WS port
- `--mac` (optional): Client MAC for testing

**What it Verifies:**
- Portal page loads
- WebSocket connection establishment
- Coin insertion state transitions
- Socket FD tracking across reconnection
- Finalization timing and Omada authentication
- Pause/resume time accuracy
- Pause file persistence to disk
- Auth sessions JSON serialization

---

### 3. **stress_test_race_conditions.py** - Race Condition Testing
Systematic validation of 6 identified race conditions.

#### RC #1: Socket FD Tracking Across Reconnection
**Tested in:** stress_test_happy_path.py Step 4b
- Verifies coin session persists when client reconnects
- Checks COIN_SOCKET_FD is updated to new FD
- Ensures coin count survives reconnection

#### RC #2: Concurrent StartCoinInsert (20 clients)
- 20 clients simultaneously attempt to start coin session
- **Expected:** Exactly 1 succeeds, 19 blocked
- Verifies mutual exclusion on coin session

#### RC #3: Staggered Coin Insertions During Active Session
- 1 client starts coin session
- 4 other clients attempt startCoinInsert concurrently
- **Expected:** All should be blocked/rejected

#### RC #4: Error Conditions
- testInsertCoin before startCoinInsert (should not be active)
- Pause without authentication (should error)
- Resume without paused session (should error)

#### RC #5: Rapid WebSocket Reconnections
- 10 rapid reconnects during active coin session
- **Expected:** Session state persists, coin status maintains

#### RC #6: Concurrent WebSocket + HTTP Requests
- Interleaved HTTP GETs and WebSocket getStatus during coin session
- **Expected:** Session survives mixed access patterns

**Usage:**
```bash
# Run all race condition tests
python3 test/stress_test_race_conditions.py --host localhost

# Or via master runner
python3 test/run_all_tests.py --host localhost --test race-conditions
```

---

### 4. **run_all_tests.py** - Master Test Runner
Orchestrates all test suites with flexible configuration.

**Usage:**
```bash
# Run everything
python3 test/run_all_tests.py --host localhost

# Run specific tier
python3 test/run_all_tests.py --host localhost --test stress
python3 test/run_all_tests.py --host localhost --test happy-path
python3 test/run_all_tests.py --host localhost --test race-conditions

# Stress test with custom parameters
python3 test/run_all_tests.py --host localhost --test stress \
  --clients 50 --duration 20 --start-ip 10.0.0.2 --start-mac 00:11:22:33:44:00

# Happy path with specific MAC
python3 test/run_all_tests.py --host localhost --test happy-path \
  --mac 00:11:22:33:44:FF
```

**Arguments:**
- `--host` (required): Server address
- `--port` (default 80): HTTP/WS port
- `--test` (default "all"): Test tier to run
  - `all`: All tests
  - `stress`: Concurrent client load test
  - `happy-path`: End-to-end validation
  - `race-conditions`: Race condition suite
- `--clients` (default 10): Number of stress test clients
- `--duration` (default 10): Stress test duration per client
- `--start-ip`: Base source IP used by clients in tests
- `--start-mac`: MAC address range for stress test
- `--mac`: Client MAC for happy path test

---

## Test Tiers & Strategy

### Tier 1: Concurrent Client Load (stress_test.py)
- **Goal:** Portal handles multiple simultaneous clients
- **Validation:** Responses complete, no server crashes
- **Use Case:** General stability under load

### Tier 1.5: Happy Path (stress_test_happy_path.py)
- **Goal:** Complete user workflow end-to-end
- **Validation:** All state transitions work, files persist
- **Use Case:** Regression testing, integration verification

### Tier 2: Race Condition #1 (in happy path)
- **Goal:** Socket FD tracking correct during reconnection
- **Validation:** Coin session preserved across WS reconnect
- **Critical:** Affects all clients who disconnect/reconnect

### Tier 2+: Race Conditions #2-6 (stress_test_race_conditions.py)
- **Goal:** Mutual exclusion, error handling, concurrent access
- **Validation:** Only 1 coin session active, errors handled gracefully
- **Critical:** Prevents multiple simultaneous coin sessions or data corruption

---

## Running Against Real ESP32

### Prerequisites
1. ESP32 with EllaFi firmware loaded
2. Network connectivity to ESP32 IP
3. (Optional) Omada Controller running if testing auth flow

### Example
```bash
# Assuming ESP32 at 192.168.1.100
python3 test/run_all_tests.py --host 192.168.1.100 --port 80

# Just happy path validation
python3 test/stress_test_happy_path.py --host 192.168.1.100 --mac 00:11:22:33:44:FF

# Heavy load test (50 concurrent clients)
python3 test/stress_test.py --host 192.168.1.100 --clients 50 --duration 30
```

### Monitoring Output
- Look for **assertion failures** → test detected invalid state
- Look for **timeouts** → server not responding in time
- Look for **socket errors** → server crashes or closes unexpectedly
- Check SerialMonitor for ESP32 logs during test execution

---

## File Verification During Tests

Happy path test verifies:
1. **Pause file** (`/pause_<MAC>.dat`) - Created and readable via HTTP
2. **Auth sessions** (`/auth_sessions.json`) - Persisted via LittleFS

These are accessed via `serveStatic("/", LittleFS, ...)` in main.cpp:
```bash
curl http://192.168.1.100/pause_001122334455.dat
curl http://192.168.1.100/auth_sessions.json
```

---

## Expected Results

### Happy Path Test PASS
```
[HAPPY] Step 1: Portal GET -> 200
[HAPPY] Step 2: Initial status -> authenticated=False
[HAPPY] Step 3: Coin session started
[HAPPY] Step 4: 1 coin inserted
[HAPPY] Step 4b: After reconnect, coinSessionActive=True, coinCount=1
[HAPPY] Step 5: Finalization -> type=coinSessionComplete, success=True, minutesGranted=10
[HAPPY] Step 6: Authenticated, expiresInMillis=600000
[HAPPY] Step 7: Paused, pausedRemainingMillis=600000
[HAPPY] Step 7b: Pause file found (4 bytes)
[HAPPY] Step 9: Resumed, expiresInMillis=600000
[HAPPY] Step 11: auth_sessions.json found
[HAPPY] ✓ Happy path with reconnection test PASSED
```

### Race Condition Tests PASS
```
[RC2] Results: 1 succeeded, 19 blocked, 0 errors
[RC2] ✓ PASS - Exactly 1 client won the coin session

[RC3] Client 0: Coin session acquired
[RC3] Client 1-4: Correctly blocked
[RC3] ✓ PASS - 4 clients correctly blocked

[RC4] Test 1: ✓ Correctly not active (no startCoinInsert)
[RC4] Test 2: ✓ Correctly rejected pause (not authenticated)
[RC4] Test 3: ✓ Correctly rejected resume (no paused session)
[RC4] ✓ PASS - Error conditions handled

[RC5] Maintained session across 10 reconnects
[RC5] ✓ PASS

[RC6] Iteration 0-4: HTTP GET -> 200, WS status -> active=True
[RC6] ✓ PASS - Session survived concurrent HTTP/WS
```

---

## Troubleshooting

### Tests timeout waiting for server response
- Check server is running: `curl http://host:port/`
- Check WiFi connectivity
- Look for ESP32 serial output (may indicate crash)

### Pause file not found (HTTP 404)
- Verify `server.serveStatic("/", LittleFS, ...)` in main.cpp
- Check LittleFS is mounted: `ESP_LOGI(TAG, "LittleFS Mounted Successfully")`

### Authentication fails in happy path
- Verify Omada controller credentials in main.cpp
- Check controller is reachable from ESP32
- May need to mock Omada if testing locally

### Race condition tests show unexpected concurrent sessions
- Check COIN_STATE_MUTEX is properly protecting COIN_SESSION_ACTIVE
- Verify xSemaphoreTake/Give calls in handleWsRequest around startCoinInsert

---

## Summary

The EllaFi test suite validates:
- ✅ Multiple concurrent clients
- ✅ Complete user workflows  
- ✅ WebSocket reconnection handling
- ✅ Mutual exclusion on coin sessions
- ✅ Proper error handling
- ✅ File persistence (pause files, auth sessions)
- ✅ Time accuracy (pause/resume)
- ✅ Socket FD tracking

Run `python3 test/run_all_tests.py --host <esp32-ip>` to validate the entire system.
