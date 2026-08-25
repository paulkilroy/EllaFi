# Omada device-plane emulation — feasibility runbook

**Goal:** make an emulated Omada device (Python now, ESP32 later) get **adopted** by an Omada
controller and show real state in the **Omada app**. Because the controller↔TP-Link-cloud channel
is already outbound + always-on, an adopted device is reachable **through the app from anywhere** —
that is the remote-access win, with no tunnel, no VPS, no inbound (Starlink CGNAT stays irrelevant).

**This is Mac-local lab only.** The throwaway SW controller in Docker is the adopt target and the
jar source. The real Bakhaw OC220 and the ESP are never touched here. ESP firmware is Phase B.

---

## THE GATE — Week-1 go/no-go (do this before anything else)

**Is the management-channel AES key a FLAT CONSTANT or PER-DEVICE?**
- **Flat** (UniFi-style, e.g. `md5("ubnt")`): a Python emulator is weeks of work → GO.
- **Per-device** seeded only during adopt on an already-encrypted channel: much harder → reassess.

Everything downstream is wasted effort until this is known. Drive straight at it:

```
docker compose -f omada-lab/docker-compose.yml up -d controller     # throwaway controller
# wait for it to be healthy (https://localhost:8043), then pull its jars:
docker cp omada-controller:/opt/tplink/EAPController/lib ./dev/_jars
tools/omada_recon.py ./dev/_jars --jadx                        # hunt crypto + proto + Model enum
```

Read `omada-lab/omada_recon_report.md` → the "KEY material / derivation" section is the answer.

Cross-check dynamically: sniff the adoption handshake on the Docker bridge (`tcpdump` in the
emulator container on 29810–29814) and see whether the **pre-adopt** bytes decrypt under a guessed
flat key. Static + dynamic agreeing = confident go/no-go.

---

## Phases

- **A — reference emulator (Python), against the Docker controller.** Prove adopt end-to-end;
  capture the wire truth (discovery beacon → adopt cmd → device-account accept → inform/heartbeat).
  Fill the app's device screen from a script. **← we are here.**
- **B — port into firmware** as `src/omada_device.cpp`, its own FreeRTOS task, reading real node
  state (uptime, free heap→"memory", client count from `SessionCache`, coin-box health). Spoof an
  ancient always-supported model (EAP225/EAP245); MAC on a TP-Link OUI (the real 603's `8C-86-DD`
  is a valid one to borrow — never the Espressif OUI, it flags the registry check).

## Emulator identity (lab)
- Model: spoof **EAP225-Outdoor** or **EAP245** (old, always in the controller's model registry).
- MAC: a TP-Link OUI + random tail; keep it stable across restarts (adoption binds to it).
- Reach the controller by service name `controller` (directed/L3 inform — no broadcast on Mac Docker).

## Open questions (parked until schema is out — see the doc's client-association analysis)
- Which device→client relations are **device-settable in an inform** vs controller-derived? Only a
  non-radio settable relation (wired-port / downlink / reported-via) lets the piso node list clients
  without colliding with the real EAP that owns the radio association.
- Is `clientCount` a standalone vital (report the number, empty table) — the safe fallback?

## Honest ceiling
The app renders **EAP-shaped fields only**. Coin-jam / peso rates / firmware-version have no native
EAP field → they stay smuggled (status string in a fake client hostname, mgmt-drop as a disconnect
alert) or invisible. If those signals are the real goal, REST + an MQTT/push side-channel carries
them first-class for far less. Device-plane adoption is the "unify into the app + free remote
reach" play, chosen with eyes open.
