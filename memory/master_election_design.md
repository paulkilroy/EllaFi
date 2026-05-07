# Master Election & Monitoring Design

**Status: NOT YET IMPLEMENTED** — deploying config-based master to Philippines first. Come back to this after validating the deployment.

> Note: healthchecks.io ping is tracked separately in TODO.md and can be implemented independently of the election refactor.

---

## Why

If the master node loses power mid-day, the whole business halts. Can't wait for the 3am reboot.
Self-healing in ~5 seconds vs potential hours of downtime justifies the complexity.

---

## Protocol: Gossip + Lowest MAC wins

Every node (master and slave) broadcasts a HELLO every **300ms**.

**HELLO wire format** (binary, 17 bytes):
```
[uint16_t type][uint64_t psk][uint8_t mac[6]][uint8_t is_master]
```

If no master HELLO heard for **2 seconds** (~6 missed packets):
- Each node collects all MACs heard in the last 2s (including own)
- Node with the **lowest MAC** self-promotes
- Tie-breaking delay before promoting: `(ownMac[5]) * 5ms` — prevents simultaneous promotion on exact same timeout

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

**Master reboots:** Stops broadcasting. After 2s, lowest remaining slave promotes. Old master comes back, hears new master, stays slave (unless it has lower MAC — in which case it promotes and the current master demotes on hearing the lower-MAC HELLO with `is_master=true`).

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
- Remove `MASTER_MAC` loading and validation
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
| `master_mac` | **Remove** |
| `master_ip` | **Add** — static IP claimed by master on election (e.g. `"192.168.1.50"`) |
| `healthchecks_url` | **Add** — ping URL from healthchecks.io dashboard (empty = disabled) |
| `network_key` | Keep |
| `minutes_per_coin` | Keep (already added) |
