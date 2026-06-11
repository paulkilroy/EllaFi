# Enhancements / Future Work

Tracked ideas. Newest at top.

---

## ✅ SHIPPED — Flasher: "Read device MAC" button (with a master-confirmation guard)

Implemented in `docs/flasher.html` (read-MAC button next to Master Node MAC + confirm
popup + Linux serial troubleshooting `<details>`). Kept here for the rationale behind the
confirmation guard:

**What:** A button in `docs/flasher.html` that reads the connected chip's MAC over USB
(esptool-js exposes it — `await loader.chip.readMac(loader)` after `loader.main()`), so a
non-technical user (e.g. Ella in the field) can get a node's MAC without a serial console.

**Why it's useful:** self-service MAC retrieval during the flash they're already doing; also
the easiest way to recover/confirm any node's MAC for `master_mac`.

**Why it's DANGEROUS — and the required guard:**
- The chip MAC is only the correct `master_mac` **if this specific device is the master**.
- `master_mac` is written to **every** node; only the master's MAC belongs there. Blindly
  auto-filling `master_mac` from whatever board is plugged in could designate the wrong node
  as master (or create two masters / no master).
- **Requirement:** reading the MAC is fine to show freely, but using it to fill `master_mac`
  must be a deliberate **button + confirmation popup** — e.g. *"Set this device as the MASTER?
  Its MAC (AC:A7:04:26:4A:D4) will be written as master_mac on every node you flash. Only do
  this for the one master node."* Never silently auto-fill `master_mac`.
- Display the MAC **colon-separated** (`AA:BB:CC:DD:EE:FF`). Omada shows MACs dash-separated;
  the firmware's `isMaster()` does a plain string compare against `WiFi.macAddress()` (colons),
  so a dash-formatted `master_mac` silently makes the node a slave.
