# Omada device-plane integration — firmware port design (Phase B)

How the proven ECSP device protocol lands on the ESP32 so each EllaFi node appears as a managed
device in the operator's Omada controller (health, activity logs, alerts, GitHub-driven firmware).

- **Requirements + live status:** [omada_requirements.md](omada_requirements.md)
- **Wire contract (authoritative):** [omada-protocol-reference.md](omada-protocol-reference.md) · [adopt-handshake.md](adopt-handshake.md) · [findings-crypto.md](findings-crypto.md)
- This doc is the *firmware* design — it references the wire details, it does not re-derive them.

Proven end-to-end in the emulator (`omada-lab/emulator/adopt_full.py`) against a 6.2.14.11 controller.
Phase B = porting that into `src/omada_device.cpp`.

---

## 1. Two planes to the same controller — keep them decoupled

| Plane | Module | Transport | Purpose |
|---|---|---|---|
| **API client** (exists) | `omada.cpp` | HTTPS :8043 + cookie | client auth (extPortal), extend/disconnect, vouchers, operator data |
| **Device** (NEW) | `omada_device.cpp` | ECSP RC4/RSA :29810/:29814 | node monitoring: adoption, health, logs, alerts, firmware |

They share only the controller address. The device plane must **never** touch the coin/auth path or
the inbound web pool. It is outbound-only and best-effort.

## 2. Module & task structure
- New `src/omada_device.{h,cpp}`; one low-priority FreeRTOS task `omadaDeviceTask` (own stack), never
  the web or coin task.
- Best-effort, non-blocking ([[boot_never_blocks_on_runtime_deps]]): starts *after* the web server is
  up; controller unreachable → exponential backoff; failure never blocks boot or selling.
- Outbound TLS/informs serialize behind the existing `omadaTlsInFlight()` gate so device traffic +
  API calls can't spike concurrent sockets. Net socket cost: **+1 transient outbound**, 0 on the
  portal pool.

## 3. Identity — device-only, NOT committed
The node presents a genuine EAP225-Outdoor(US) v1.0 identity (hwId, oemId, model, compoundModel, real
`5.x` version). These constants are reverse-engineered from TP-Link and **must stay on-device** —
stored in a gitignored FS file (`data/omada_identity.json`, added to the FS-upload PRESERVE list) or
NVS, never in the repo. *The flow below is committable; the identity constants and the controller RSA
pubkey are not.* Each physical node needs a unique device MAC (its own Wi-Fi MAC) → see §7.

### 3a. Why EAP225-Outdoor is the model to fake (catalog-driven, not arbitrary)
From the controller's own model catalog (`manager-core .../json/buildindata/initDeviceModelTemplateData.json`
— 191 unique AP models, 239 rows). Two field sets matter:
- **`deviceInfo`** = identity + live health, the **same 16 fields for every AP**: `model, hwId, oemId,
  firmwareVersion, hardwareVersion, modelVersion, name, ip, mask, isFactory, upTime, cpuUti, memUti,
  txRate, rxRate, wirelessLinked`. The ESP32 can fill all of these; identity is cheap on any model.
- **`components_v2`** = the capability manifest (48–77 per model). Every capability advertised is
  something the controller may expect the device to support/report — so more caps = more spoof surface.

**The deciding capability is `abnormalDetect`** — it drives the Device Health **spider graph**. It is
rare: only **7 AP models** carry it (AP7650, AP9665, EAP223, EAP225, **EAP225-Outdoor**, EAP245,
EAP265 HD), and **EAP225-Outdoor is the only *outdoor* one**. Newer outdoor models (EAP650/670/772-
Outdoor) dropped it.

| Candidate | #caps | outdoor | mesh | spider graph | verdict |
|---|---|---|---|---|---|
| **EAP225-Outdoor** | 73 | ✅ | ✅ | ✅ | **chosen** — unique intersection of outdoor ∩ health graph ∩ proven live firmware indicator (cloud served 5.2.2) |
| EAP113/110-Outdoor | 50 | ✅ | — | ❌ | simplest + zero mesh risk, but no health graph and WiFi-4/EOL → cloud firmware likely frozen (indicator may never light) |
| EAP225 (indoor) | 65 | — | ✅ | ✅ | same silicon, but indoor identity — semantic mismatch for a piso |
| EAP650/772-Outdoor | 71–77 | ✅ | ✅ | ❌ | newer outdoor but no spider graph, more caps |

Choosing EAP225-Outdoor buys the two headline features (health spider graph + live firmware indicator)
that evaporate on nearly every alternative. The `mesh` capability it carries is the only downside —
mitigated by reporting `wirelessLinked:false` / no mesh uplink (§5), so the controller never tries to
manage a mesh. The one alternative worth remembering: **EAP113-Outdoor** if we ever drop the health-
graph + firmware-indicator requirements in exchange for a minimal, mesh-free spoof.

## 4. Adoption lifecycle (see protocol-reference §4)
`UNADOPTED → DISCOVERING → NEGOTIATING → ADOPTED/CONNECTED → (RECONNECT | FORGET)`
- **Discovery:** UDP beacon on :29810 → node shows as *pending* in Omada. (Real LAN broadcast works;
  the lab used L3-directed inform only because Docker Desktop's L2 broadcast is broken.)
- **Negotiation:** `DEVICE_NEGOTIATION` with **plain** `hwId`/`oemId`; RSA-wrap the RC4 session key
  with the controller's netty pubkey; `randomKeyForSystemVerify` ≥36 chars (uuid) — the real 6.2
  gotcha ([[omada_62_adopt]]).
- **Credential:** factory `admin`/`admin` on first adopt; `SYSTEM_NEGOTIATION` pushes the site Device
  Account (`userAccount{newUsername,newPassword=MD5}`) → **persist it** (NVS/file), present on every
  reconnect. Survives reboot.
- **Operator action:** node shows *pending* → operator taps Adopt once. Automatic thereafter.

## 5. Inform loop (steady state)
Periodic `INFORM` POST carrying device state — almost all of which the firmware **already holds**:
- **health:** uptime, free heap/PSRAM (the sampler we just added), CPU load (approx), temperature
  (ESP32 internal sensor), reset/boot reason.
- **spider/health graph:** advertise `supportAnomaly` component; the controller's scheduled job fills
  it after a baseline window.
- **clients:** intentionally **none** — AP-reported clients are ghosts and real clients roam→flap
  (decided in requirements). The node's own RSSI is free via its real EAP client record.
- **format trap:** `upTime`/client `activeTime` = `"<D> days HH:MM:SS"` (StringTokenizer) — format the
  string, never send an int (aborts persistence).

## 6. Logs & alerts — where what we just built plugs in
The inform `log` field = `EapSysLog` = `{"logs":[{"k","c","t"}]}`. This is the **single hook** the
local logs feed:
- **Activity log → Device Logs/Events:** the same AUTH/EXTEND/REFUND/PAUSE/RESUME events that
  `appendActivityLog()` writes to `/activity.log` get queued into the next inform's `log` array.
  Content leads with the action; key = a device Event type (`AP_CH_C`); Type column borrowed
  (unavoidable — no AP-postable neutral type).
- **W/E → Alerts:** the `SOCKETS high` warning, `OUT OF SERVICE`, and `BOOT: last reset=…` lines
  (already on the W/E path via `logCore`) mirror as `alert:true` keys → the controller's Alerts tab.
  The existing `LOG_FORWARD_FN` hook (slave→master forward) is the natural tap point — add an Omada
  sink alongside it.
- **Mechanism:** a small bounded outbound queue (ring) of pending log lines; each inform drains it
  one-shot (like the emulator's `_inject.jsonl`); oldest dropped if the controller is down. **Zero
  extra sockets** — it rides an inform we're already sending.

## 7. Multinode — DECIDED: each node reports
Every node adopts as its own EAP225 device (its own Wi-Fi MAC = device identity). Operator sees every
node in Omada with per-node health/logs. Why it's easy:
- **Same code on every role.** `omada_device.cpp` runs on master *and* slaves — no aggregation, no
  relay. The current `IS_MASTER` gate around `setupOmada()`/`setupWeb()` does NOT apply; the device
  task is role-independent.
- **MAC is the identity.** hwId/oemId are model-level (all nodes share the EAP225 identity); the
  controller distinguishes nodes by MAC, exactly like a real site with several identical APs. No
  per-node identity constants.
- **No controller URL needed on slaves.** Discovery is a UDP broadcast; the adoption response teaches
  the node its inform URL. A slave doesn't need `CONTROLLER_BASE_URL` pre-configured.
- **Cheaper on slaves.** Slaves run no inbound web pool, so the device plane's +1 outbound socket has
  even more headroom there.
- Site Device Account is site-wide → all nodes learn the same pushed credential; each still adopts
  individually (operator taps Adopt on each pending node — N taps, one-time).

**Where each kind of event lands (natural split):**
- **Session activity** (AUTH/EXTEND/REFUND/PAUSE/RESUME) is produced on the **master** (it runs the
  API plane + `appendActivityLog`) → lands in the **master's** device logs.
- **Per-node health/hardware** (boot reason, coin-jam, temperature; socket alarms are master-only
  anyway) → lands in **each node's own** device logs.

**One thing to test before trusting it:** each node is simultaneously a *real Wi-Fi client* of its
physical EAP (for the free RSSI record) **and** a *spoofed AP device* — the same MAC in two roles on
one controller. The dual-AP test showed a client on two APs doesn't crash the controller, but
same-MAC-as-both-client-and-managed-device wasn't exercised. Verify on the lab controller first.

## 8. Firmware upgrade — the one genuinely unbuilt piece
1. Hourly GitHub check finds a newer EllaFi build → the inform **reports a LOW version** → controller
   cloud-lights the native upgrade indicator (cloud match is model-level; needs the real EAP225 id).
2. Operator Approves → controller sends `UPGRADE_REQUEST_V2` (type 69632) with a `downloadLink` to
   TP-Link's signed image.
3. **Handler IGNORES the pushed link**, pulls the EllaFi `.bin` from GitHub via the existing
   `updateFromGithub()` OTA path, flashes, then **reports the HIGH version** to clear the prompt.

New code = just the `UPGRADE_REQUEST_V2` handler + the version-sentinel the inform reports; the actual
flashing reuses shipped OTA. Fallback with no TP-Link cloud: write `last_fw_ver`/`fw_url` into the
controller's `modelfw` collection directly (self-hosted controllers only).

## 9. Crypto on ESP32 (feasible)
RC4 (trivial), RSA public-key wrap (`mbedtls_pk`/`mbedtls_rsa`), MD5 (`mbedtls_md`) — all in mbedtls,
already linked (base64/md are used today). Controller netty RSA pubkey = device-only constant (§3).

## 10. Config push (SET_REQUEST) — decide honored vs echoed
Controller can push SSID/wireless config via `SET_REQUEST`; the device applies + echoes. Most fields
are cosmetic for a fixed-radio piso node → accept + echo to stay "in sync", apply only a safe subset.
Which fields are honored vs echo-only is a decision.

## 11. Phasing (each milestone shippable on its own)
1. **Adopt + stay connected** (health only) — node is green in Omada.
2. **Log/alert injection** — activity log + socket alarms land in Device Logs/Alerts (§6).
3. **Firmware upgrade** indicator + `UPGRADE_REQUEST_V2` handler (§8).
4. **Config push** handling (§10).

## Open decisions & risks
- **Decided:** multinode = each node reports (§7).
- Still open: SET_REQUEST honored fields (§10); identity storage NVS vs gitignored FS file (§3).
- Risks: same-MAC-as-both-client-and-device (§7, test first); controller may try to manage/mesh the
  node; controller-upgrade version-scheme drift; identity spoofing is fragile if TP-Link changes cloud
  matching (the `modelfw` fallback removes that dependency on self-hosted controllers).
