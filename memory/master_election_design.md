# Master Election & Monitoring Design

**Status: NOT YET IMPLEMENTED** — deploying config-based master to Philippines first. Come back to this after validating the deployment.

> Note: healthchecks.io ping is tracked separately in TODO.md and can be implemented independently of the election refactor.

---

## Why

If the master node loses power mid-day, the whole business halts. Can't wait for the 3am reboot.
Self-healing in ~5 seconds vs potential hours of downtime justifies the complexity.

---

## Protocol: Gossip + Preferred MAC + Uptime-weighted election

Every node (master and slave) broadcasts a HELLO every **300ms**.

**HELLO wire format** (binary, 17 bytes):
```
[uint16_t type][uint64_t psk][uint8_t mac[6]][uint8_t is_master]
```

The HELLO body already includes `uptime_sec` and `rssi` (carried in the existing HeartbeatBody; HELLO reuses or extends this).

### Election priority (highest wins)

1. **Preferred MAC** (`master_mac` in config) — always wins if present. When it returns after a failover, it reclaims master by announcing `is_master=true`; the temporary master demotes itself.
2. **Highest uptime** — a node that has been up for days is demonstrably more stable than one that just rebooted. Uptime is already broadcast in HeartbeatBody.
3. **Best RSSI** — tiebreaker when uptimes are close (within ~30s). Already broadcast in HeartbeatBody.
4. **Lowest MAC** — final deterministic tiebreaker.

> Why not traceroute or hop count? All three nodes are on the same flat LAN subnet via Omada APs — every node is 1 IP hop to the gateway. RSSI + uptime are better proxies and are already on the wire.

If no master HELLO heard for **2 seconds** (~6 missed packets):
- Each node collects all HELLOs heard in the last 2s (including own)
- Each node independently runs the priority ranking above and identifies the winner
- Winner self-promotes; all others stand by
- Tie-breaking delay before promoting: `(ownMac[5]) * 5ms` — prevents simultaneous promotion when ranking is tied

On promotion, new master:
1. Sets `IS_MASTER = true`
2. Reconfigures to static IP: `WiFi.config(masterIp, WiFi.gatewayIP(), WiFi.subnetMask())`
3. Registers `ellafi.local` via mDNS
4. Calls `startMasterServices()` — extracted from `setup()`'s `if (IS_MASTER)` block
5. Begins broadcasting HELLO with `is_master = true`
6. Immediately pings healthchecks.io

---

## Message Types

| Message | Direction | Notes |
|---------|-----------|-------|
| `NET_MSG_HELLO` | all nodes → all, every 300ms | carries MAC + is_master flag; replaces master-only heartbeat |
| `NET_MSG_COIN_INSERTED` | slave → all | unchanged |
| `NET_MSG_SESSION_START` | master → all | unchanged |
| `NET_MSG_SESSION_END` | master → all | unchanged |

ELECTION and ELECTED message types are **not needed** — gossip presence is sufficient.

---

## Edge Cases

**Sequential boot (normal):** First node hears nothing for 2s, promotes. Others hear its HELLO, stay slave.

**Simultaneous boot:** All hear nothing for 2s, all collect same MAC list, all pick same winner (lowest MAC). Only winner promotes; others stand down when they hear winner's HELLO.

**Master reboots:** Stops broadcasting. After 2s, highest-priority remaining slave promotes (uptime → RSSI → lowest MAC). Old master comes back and runs the ranking: if it is the preferred MAC, it announces `is_master=true` and the current master demotes (calls `esp_restart()`); otherwise it stays slave.

**Two masters detected:** If a node that thinks it's master hears a lower-MAC HELLO with `is_master=true`, it calls `esp_restart()`. On reboot it hears the real master immediately and stays slave.

---

## Code Changes Required

### `network.h` / `network.cpp`
- Add `NET_MSG_HELLO` to `NetMsgType` enum
- Replace `isMaster()` with gossip state machine
- Add 300ms broadcast timer (FreeRTOS timer or tracked in `networkLoop()`)
- Track `lastMasterHeardMillis` — reset on any HELLO with `is_master=true`
- Export `bool networkWonElection()` flag — polled by `loop()` in main.cpp

### `main.cpp`
- Extract the `if (IS_MASTER)` block from `setup()` into `startMasterServices()`
- `loop()` checks `networkWonElection()` once and calls `startMasterServices()`
- Add `extern String MASTER_IP`

### `globals.h`
- Add `extern String MASTER_IP`

### `files.cpp`
- Load `MASTER_IP` from config: `MASTER_IP = doc["master_ip"] | ""`
- Keep `MASTER_MAC` loading — used as election priority hint
- Add `HEALTHCHECKS_URL` loading: `HEALTHCHECKS_URL = doc["healthchecks_url"] | ""`

### `config.sample.json` / `config.json`
- Remove `master_mac`
- Add `master_ip` (e.g. `"192.168.1.50"`)
- Add `healthchecks_url` (empty string = disabled)

---

## Healthchecks.io Monitoring

The master pings a healthchecks.io URL every **60 seconds**.

Configure the check on healthchecks.io as:
- **Period:** 1 minute
- **Grace:** 5 minutes
- **Alert fires:** within ~6 minutes of total system failure

Key behavior: when a slave promotes to master, it immediately starts pinging — healthchecks stays green through normal failover. An alert means **everything is dead**, not just one node rebooted.

**POST payload** (small JSON, well under 10KB limit):
```json
{"mac":"AA:BB:CC:DD:EE:FF","uptime_s":12345,"active_sessions":1}
```
Lets you see which node is currently master and whether a failover occurred.

**Implementation:**
- `TimerHandle_t HEALTHCHECK_TIMER` — 60s repeating, created in `startMasterServices()`
- Callback POSTs to `HEALTHCHECKS_URL` using existing HTTPS infrastructure
- If URL is empty, skip silently

---

## Config Changes Summary

| Key | Change |
|-----|--------|
| `master_mac` | **Keep** — preferred master; always wins election when present |
| `master_ip` | **Add** — static IP claimed by master on election (e.g. `"192.168.1.50"`) |
| `healthchecks_url` | **Add** — ping URL from healthchecks.io dashboard (empty = disabled) |
| `network_key` | Keep |
| `minutes_per_coin` | Keep (already added) |

---

## Config Propagation (admin UI → all nodes)

Because any slave can be promoted to master, all nodes need an up-to-date full config including Omada credentials.

When admin saves config via the admin UI:
1. Master validates and writes its own `config.json`
2. Master broadcasts `{"type":"config_update","config":{...}}` over UDP
3. Each slave receiving it writes `config.json` to its LittleFS and reboots
4. Master reboots last (after a short delay to give slaves time to receive)

**Broadcast reliability:** Master has no roster of connected slaves and can't wait for ACKs. Mitigation: broadcast 3× with ~500ms gap. On the same LAN with 2–3 nodes packet loss is near zero.

**Missed broadcast (slave offline during push):** Slave can pull config from master on boot — `GET http://<master_ip>/admin/config`. If config differs from local copy, save and reboot. Also useful for freshly flashed slaves that only have minimal config.

### The chicken-and-egg fields

Some config changes cannot self-propagate:

| Field | Problem | Mitigation |
|-------|---------|------------|
| `wifi_ssid` / `wifi_password` | Master switches network; slaves still on old network can't receive broadcast | Manual reflash of all slaves required |
| `network_key` | The broadcast is validated by PSK — slaves with old key will reject a broadcast carrying a new key | Manual reflash of all slaves required |
| All other fields | Safe to propagate via UDP | — |

Admin UI must warn clearly when either of these fields is changed. *Exact wording and UX to be decided before implementing.*

### New firmware endpoints needed

| Endpoint | Purpose |
|----------|---------|
| `POST /admin/config` | Validate, save, broadcast to slaves, schedule reboot |
| `GET /admin/config` | Serve current config (for admin UI load + slave pull-on-boot) |

`/config.json` already returns 403 — `/admin/config` is a separate, intentionally accessible endpoint. Decide whether to add any auth before exposing it.

---

## Persistent State Gaps on Failover

When a slave promotes to master it starts with a clean LittleFS. The following state from the old master is gone:

### Session cache (HOTSPOT_SESSION_CACHE)
**No gap.** Already designed: `refreshHotspotSessionCache()` is called in `startMasterServices()` and rebuilds from Omada. Active sessions are unaffected from the client's perspective.

### Paused sessions (`pause_<MAC>.dat`)
**Decision: Option C — broadcast pause/resume events as they happen.** Slaves maintain a mirror in RAM. On promotion, new master loads the RAM snapshot. Any slave that was online during the pause has the data; only a slave reboot between pause and failover loses it (narrow window, acceptable).

New UDP message types needed:
- `{"type":"pause_mirror","mac":"...","remaining_ms":12345}` — master → all, on each pause
- `{"type":"resume_mirror","mac":"..."}` — master → all, on each resume (slaves discard the entry)

### errors.log / refunds.log / sales.log (planned)
**Decision: Accept loss.** Logs are operational aids. Historical data stays on the old node's LittleFS; admin can pull before decommissioning. Not worth the broadcast overhead.

### Slave error logs
**Decision: Forward to master via UDP.** Serial access isn't practical in a deployed village. Master stores with a `source` field so admin UI can show which node generated each error.

New UDP message type needed:
- `{"type":"log_error","source":"AA:BB:CC:DD:EE:FF","tag":"...","msg":"..."}` — slave → master

### In-flight coin session on failover
If the old master dies mid-coin-insert, `COIN_INSERT_ACTIVE` and `COIN_COUNT` are RAM state — gone. The new master starts clean. The client's UI will be in an inconsistent state; the coin insert timeout will fire on the client side with no server response.

This is an inherently narrow window (within a 10s coin insert) and a known acceptable gap. No action proposed.

---

## Chicken-and-Egg Config Fields

WiFi creds and `network_key` cannot self-propagate (slaves can't receive a broadcast they're not connected to / can't validate). Mitigation: **SoftAP fallback**.

If a node fails to connect to WiFi after N retries, it starts a fallback AP (`EllaFi-Setup-XXXX`, hardcoded password baked into firmware). Admin connects phone to it and pushes new config via HTTP. Works for both WiFi creds and `network_key` changes — no USB reflash required.

Admin UI must show a hard warning when either field is changed: "WiFi credentials / network key changes require connecting to each node's setup AP to apply."

---

## Decisions

| Topic | Decision |
|-------|----------|
| Config propagation | Broadcast from master on save; slaves write + reboot |
| Paused sessions on failover | Option C — broadcast pause/resume events; slaves mirror in RAM |
| Log mirroring (errors/refunds/sales) | Accept loss — not worth broadcast overhead |
| Slave error forwarding | Yes — UDP `log_error` to master; stored with `source` field |
| `/admin/config` auth | Yes — same admin token check as other admin endpoints |
| WiFi/network_key UI | Hard warning + SoftAP fallback for applying changes |
| Slave FS OTA | Deferred — LittleFS partition is ~100% full with static files, no room to store the 3.5MB image for redistribution. Slaves currently receive firmware OTA only. Revisit when gossip/voting is implemented: options are per-file HTTP sync (manifest + individual GETs) or a dedicated storage partition. |
