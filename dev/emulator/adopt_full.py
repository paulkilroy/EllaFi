#!/usr/bin/env python3
"""adopt_full.py — end-to-end V2 adopt driver: learn the device account THROUGH the protocol.

Flow (all confirmed empirically against the lab controller, DEBUG-instrumented):
  1. Discovery keepalive on ONE persistent UDP socket (advertising ip=192.168.65.1 = this host as the
     Docker controller sees it) → device Pending. Same socket LISTENS for the reply.
  2. User taps Adopt → controller sends PRE_ADOPT_REQUEST (UDP) {adoptPort}. We receive it.
  3. We connect the ADOPT channel (TLS to adoptPort) and send PRE_CONNECT_INFO{needUsername}. Because
     the device is in ADOPTING_PENDING_PRE_ADOPT, the controller pushes ADOPT_REQUEST (type 16):
     body.data = base64(RSA_priv(username + 0x00 + MD5(password))). We hold the controller key, so we
     rsa_decrypt_pub it → LEARN username + MD5(password). No DB, no manual matching — through the wire.
  4. (next) ADOPT_RESPONSE + DEVICE_VERIFY with the learned account → INFORM → SET_REQUEST.

Lab only — localhost Docker controller.
"""
import base64
import json
import socket
import ssl
import struct
import sys
import threading
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from discovery_beacon import build_discovery, FAKE_MAC, FW_VERSION, DEVICE_TYPE
import os
from manage_crypto import ecsp2_auth, ecsp2_auth_md5, rsa_decrypt_pub, rsa_encrypt_pub, rc4

DEVICE_NEGOTIATION, VERIFY_RESULT_ACK, SYSTEM_NEGOTIATION = 1048580, 1048585, 1048581

# The emulator's "factory default" account — what a real blank device knows and you type into the
# adopt dialog. The controller then pushes the real site account+config via SET_REQUEST post-adopt.
FACTORY_USER = os.environ.get("VERIFY_USER", "admin")
FACTORY_PASS = os.environ.get("VERIFY_PASS", "admin")

HOST, DISC_PORT = "127.0.0.1", 29810
DEVICE_IP = "192.168.65.1"
PRE_CONNECT_INFO, PRE_ADOPT_REQUEST, ADOPT_REQUEST, DEVICE_VERIFY_INFO = 3, 2, 16, 1048577
DEVICE_VERIFY_RESPONSE, SYSTEM_VERIFY_RESULT = 1048578, 1048579
INIT_SYNC, INIT_SYNC_RESULT = 4352, 1048582
INFORM_REQUEST, INFORM_RESPONSE = 256, 512
SET_REQUEST, SET_RESPONSE = 4096, 8192

_seq = 0
def _next_seq():
    global _seq; _seq += 1; return _seq

def frame(obj):
    b = json.dumps(obj, separators=(",", ":")).encode()
    return struct.pack(">I", len(b)) + b

def msg(mtype, body=None, error=0):
    return {"header": {"seq": _next_seq(), "version": FW_VERSION, "device": DEVICE_TYPE,
                       "mac": FAKE_MAC, "type": mtype, "timestamp": int(time.time()), "error": error},
            "body": body or {}}

_INFORM_START = time.time()

def inform_body():
    """Full LIVE vitals INFORM → drives the device Health spider graph and keeps the manager's
    heartbeat fresh. Its dimensions come from the per-radio WirelessInfo (busyUti=channel usage,
    interUti=interference, txUti/rxUti) plus deviceInfo cpuUti/memUti; temp[] is the die temperature.
    Values must PROGRESS each inform (upTime climbs, vitals jitter) or the manager flags informIdle and
    marks the device Heartbeat-Missed. For EllaFi these map to real resources (busyUti←socket-pool%,
    memUti←heap, temp←ESP32-S3 die temp)."""
    up = 3600 + int(time.time() - _INFORM_START)     # real, increasing uptime
    j = up % 7                                        # small jitter so each inform differs
    def winfo(ch, bw, txp, busy, inter):
        b = busy + (j % 3)
        return {"region": 1, "ch": ch, "bw": bw, "rdMode": "11ax", "txR": "0", "txPower": txp,
                "txUti": max(0, b - inter - 5), "rxUti": max(0, b - inter - 8),
                "interUti": inter, "busyUti": b, "aiRoamingOffset": 0}
    return {
        "deviceInfo": {"name": "EllaFi-Piso", "model": "EAP225-Outdoor",
                       "firmwareVersion": "1.0.0 Build 20260101 Rel.00000", "hardwareVersion": "1.0",
                       "upTime": str(up), "ip": "192.168.65.1", "cpuUti": 10 + j, "memUti": 40 + (j % 4),
                       "txRate": 0, "rxRate": 0, "wirelessLinked": True, "temp": [40 + (j % 5)],
                       "nf": [-95, -92]},
        "wSettings_2G": winfo("6", "20", "20", 25, 5),
        "wSettings_5G": winfo("36", "80", "23", 30, 4),
    }


def tls_connect(port):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    raw = socket.create_connection((HOST, port), timeout=6)
    return ctx.wrap_socket(raw, server_hostname="127.0.0.1")

def read_frame(sock):
    def recv_all(n):
        buf = b""
        while len(buf) < n:
            c = sock.recv(n - len(buf))
            if not c: return None
            buf += c
        return buf
    lb = recv_all(4)
    if lb is None: return None
    body = recv_all(struct.unpack(">I", lb)[0])
    if body is None: return None
    try: return json.loads(body.decode("utf-8", "replace"))
    except Exception: return {"_raw": body[:300].decode("latin-1", "replace")}


def _recv_all(sock, n):
    buf = b""
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c:
            return None
        buf += c
    return buf


def read_frame_adaptive(sock, key=None):
    """Read one frame. If key is given, the channel may have switched to RC4 (reinitRc4Handler):
    RC4(4-byte total-len) + RC4(payload). Detect by trying plaintext length first; if absurd, RC4."""
    lb = _recv_all(sock, 4)
    if lb is None:
        return None
    pn = struct.unpack(">I", lb)[0]
    if 0 < pn < 2_000_000:                       # plausible plaintext length
        body = _recv_all(sock, pn)
        if body is None:
            return None
        try:
            return json.loads(body.decode("utf-8", "replace"))
        except Exception:
            pass  # fall through to RC4 interpretation on the same bytes is impossible; report raw
        # If we get here the "plaintext" parse failed but we already consumed body; try RC4 on it
        if key is not None:
            try:
                total = struct.unpack(">I", rc4(lb, key))[0]
                # body was pn bytes = total-4 payload expected; rc4 the body we read
                return json.loads(rc4(body, key).decode("utf-8", "replace"))
            except Exception:
                return {"_raw": body[:200].hex()}
        return {"_raw": body[:200].hex()}
    if key is not None:                          # length absurd → RC4 framed
        total = struct.unpack(">I", rc4(lb, key))[0]
        n = total - 4
        if not (0 < n < 2_000_000):
            return {"_badlen": total}
        body = _recv_all(sock, n)
        if body is None:
            return None
        try:
            return json.loads(rc4(body, key).decode("utf-8", "replace"))
        except Exception:
            return {"_raw_rc4": rc4(body, key)[:200].hex()}
    return {"_badlen": pn}


def _read_or_none(sock, key=None):
    try:
        return read_frame_adaptive(sock, key)
    except socket.timeout:
        return "TIMEOUT"
    except Exception as e:
        return f"ERR:{e}"


def drive_adopt_channel(adopt_port):
    """Connect the channel, verify with the factory default, then capture the post-adopt push
    (DEVICE_VERIFY_RESPONSE, and — the goal — SET_REQUEST with the site config). Also decrypts any
    V1-style ADOPT_REQUEST if the controller pushes one."""
    print(f"→ connecting adopt/manage channel TLS :{adopt_port}")
    try:
        sock = tls_connect(adopt_port)
    except Exception as e:
        print(f"  TLS connect failed: {e}"); return None
    try:
        sock.sendall(frame(msg(PRE_CONNECT_INFO, {"needUsername": True})))
        print("  → PRE_CONNECT_INFO(needUsername)")
        sock.settimeout(6)
        r = _read_or_none(sock)
        if not isinstance(r, dict):
            print(f"  ◀ no pre-connect response ({r})"); return None
        body = r.get("body") or {}
        rkdv = body.get("randomKeyForDeviceVerify", "")
        user = body.get("username") or FACTORY_USER
        print(f"  ◀ PRE_CONNECT_INFO_RESPONSE: username='{user}' rkdv={rkdv[:8]}…")

        # Verify with the factory-default password (what you type in the dialog for a blank device).
        auth = ecsp2_auth(user, FACTORY_PASS, rkdv)
        rnd = os.urandom(16).hex()
        sock.sendall(frame(msg(DEVICE_VERIFY_INFO, {"auth": auth, "randomKeyForSystemVerify": rnd})))
        print(f"  → DEVICE_VERIFY_INFO (user='{user}', factory pass)")

        # Our chosen RC4 session key — RSA-wrapped to the controller in DEVICE_NEGOTIATION. After the
        # controller decrypts it (RsaCipher.decryptByPrivateKey), the channel's config traffic
        # (INIT_SYNC/SET_REQUEST) is RC4'd with it (NettyChannel.reinitRc4Handler).
        session_key = os.urandom(16)

        def negotiation_body():
            # ApAdoptRespV2Body: {key, devCap, deviceInfo, ...}. The controller builds the config from
            # devCap (ApDevCapV2) — it NPEs if devCap/logNotification are missing, so provide them.
            dev_cap = {
                "supportPa": 0, "meshChainNum": 0, "supportAIRoaming": 0,
                "supportGPON": False, "supportVoip": False,
                "lanBandwidthControlPorts": [], "voipPorts": [],
                "supportPPSK": 0, "ppskNum": 0, "ppskNumV3": 0, "supportLldp": 0,
                "supportRadioMode2G": 1, "supportRadioMode5G": 1, "supportRadioMode6G": 0,
                "supportOuiMacFilter": 0, "maxTemps": [], "supportOwe": False,
                "supportBackground": 0, "supportSfp": False, "availableUplinkPorts": [],
                "powerLimits": [], "notSupportLanClient": 0, "p2pRoleDefault": 0,
                "supportP2pRoleEdit": False, "supportAfc": False, "supportEIRP": 0,
                "noSupportFeatures": [],
                "logNotification": {"supportLogKey": []},
            }
            return {
                "key": base64.b64encode(rsa_encrypt_pub(session_key)).decode(),
                "configVersion": globals().get("APPLIED_CONFIG_VERSION", 0),  # 0=fresh; N after we applied SET_REQUEST vN
                "devCap": dev_cap,
                "controllerSetting": {"controllerId": "c21f969b5f03d33d43e04f8f136e7682", "destOmadacId": ""},
                # Declare the config components we "support" so the controller pushes their config
                # (wlanBasic/ssidInform carry the SSID; portal the captive portal; etc.).
                "components": {},
                "components_v2": {"wlanBasic": "2.2", "ssidInform": "2.0", "portal": "2.1",
                                  "portalAct": "2.0", "macfilter": "2.2", "multicastFilter": "1.0",
                                  "qos": "1.0", "led": "1.0", "ssh": "1.0", "loopback": "1.0",
                                  "p2pSetting": "1.1", "anteGain": "1.0", "ipv6Group": "1.0",
                                  "wifiLedCtrl": "1.0", "upgrade": "1.0"},
                "notSupportComponents": {},   # Map<String,String>, not an array
                # Declare radios so the controller pushes the WLAN/SSID config (radioId 0=2.4G, 1=5G).
                "radioCap": [
                    {"radioId": 0, "minPow": 1, "maxPow": 20, "maxPowOd": 20, "limitMaxPow": 20,
                     "mimo": 2, "ofdma": 0, "ofdmaEnable": 0, "mcsLevel": 9, "supportSsidNum": 8,
                     "supportMaxClient": 128, "supportMlo": False, "supportMaxAssocClient": 128,
                     "supportAnteGainSetting": 0, "supportAntennaDirection": []},
                    {"radioId": 1, "minPow": 1, "maxPow": 23, "maxPowOd": 23, "limitMaxPow": 23,
                     "mimo": 2, "ofdma": 0, "ofdmaEnable": 0, "mcsLevel": 11, "supportSsidNum": 8,
                     "supportMaxClient": 128, "supportMlo": False, "supportMaxAssocClient": 128,
                     "supportAnteGainSetting": 0, "supportAntennaDirection": []},
                ],
                "deviceMisc": {"support2g": True, "support5g": True, "support5g2": False,
                               "support6g": False, "support_11ac": True, "support_lag": False,
                               "supportMesh": 1, "customizeRegion": 0, "lanPortsNum": 1,
                               "support_channelLimit": False, "supportDfs": 0, "supportRoaming": 1},
                "deviceInfo": {
                    "name": "EllaFi-Piso", "model": "EAP225-Outdoor", "modelVersion": "1.0",
                    "firmwareVersion": "1.0.0 Build 20260101 Rel.00000", "hardwareVersion": "1.0",
                    "upTime": "3600", "cpuUtil": 5, "memUtil": 37, "wirelessLinked": False,
                    "encryptedHwId": base64.b64encode(rc4(b"EllaFiPiso01", session_key)).decode(),
                    "encryptedOemId": base64.b64encode(rc4(b"TP-LINK", session_key)).decode()},
            }

        # After reinitRc4Handler, the controller's DECODER also expects RC4 from us — so once we've
        # sent DEVICE_NEGOTIATION, outgoing frames must be RC4-framed with our session key too.
        # SEND_RC4 toggles how we frame outbound after negotiation. The controller's inbound decoder
        # may or may not have flipped to RC4 (reinit timing) — try plaintext first (env SEND_RC4=1
        # forces RC4-total, =2 forces RC4-continuous).
        send_rc4 = os.environ.get("SEND_RC4", "0")
        def send(mtype, body=None, error=0):
            payload = json.dumps(msg(mtype, body, error), separators=(",", ":")).encode()
            if rc4_mode and send_rc4 == "1":
                sock.sendall(rc4(struct.pack(">I", 4 + len(payload)), session_key) + rc4(payload, session_key))
            elif rc4_mode and send_rc4 == "2":
                sock.sendall(rc4(struct.pack(">I", 4 + len(payload)) + payload, session_key))
            else:
                sock.sendall(struct.pack(">I", len(payload)) + payload)

        # Drive the post-verify sequence, replying to each step so the controller advances to the
        # SET_REQUEST provisioning push. Keep the socket alive (short reads, respond promptly).
        captured = []
        rc4_mode = False
        connected = False
        sock.settimeout(10)
        while True:                      # stay on the channel; heartbeat keeps the device online
            r = _read_or_none(sock, session_key if rc4_mode else None)
            if r == "TIMEOUT":
                if connected:            # periodic INFORM heartbeat so the controller keeps us green
                    send(INFORM_REQUEST, inform_body())
                    print("  ♥ INFORM heartbeat")
                continue
            if not isinstance(r, dict):
                print(f"  ◀ channel ended ({r})"); break
            t = r.get("header", {}).get("type")
            seq = r.get("header", {}).get("seq")
            print(f"  ◀ type={t}{' [rc4]' if rc4_mode else ''}: {json.dumps(r)[:600]}")
            captured.append(r)
            if t in (SYSTEM_NEGOTIATION, INIT_SYNC, SET_REQUEST):
                # dump the FULL decrypted config to a gitignored .local file (may hold credential hashes)
                with open(os.path.join(os.path.dirname(__file__), "..", "captured-config.local"), "a") as fh:
                    fh.write(json.dumps(r) + "\n")
                print(f"  (full type={t} body saved to dev/captured-config.local)")

            if t == DEVICE_VERIFY_RESPONSE:
                send(SYSTEM_VERIFY_RESULT, error=0)
                print("  → SYSTEM_VERIFY_RESULT (accept controller)")
            elif t == VERIFY_RESULT_ACK:
                # Provide our RC4 session key (plaintext), then the channel flips to RC4.
                send(DEVICE_NEGOTIATION, negotiation_body())
                rc4_mode = True
                print(f"  → DEVICE_NEGOTIATION (RC4 session key wrapped, {session_key.hex()[:8]}…)")
            elif t in (INIT_SYNC, SYSTEM_NEGOTIATION):
                # ack the initial config sync → device goes CONNECTED. Do NOT INFORM immediately:
                # INIT_SYNC_RESULT sets CONNECTED asynchronously, and an INFORM racing ahead of it hits
                # status ADOPT_SUCCESS in the keepalive → the controller closes the channel. Let the
                # heartbeat send the first INFORM after a short delay, once CONNECTED has settled.
                send(INIT_SYNC_RESULT, error=0)
                print(f"  → INIT_SYNC_RESULT (ok) [ack of {t}] — device CONNECTED")
                connected = True   # start heartbeating so we stay online
                sock.settimeout(3)  # fast heartbeat keeps the connected route refreshed (real devices
                                    # inform ~1s; a slow heartbeat lets the ~60s route TTL lapse).
            elif t == SET_REQUEST:
                # Confirm we APPLIED the config: echo sequenceId + configVersion back, else the
                # controller stays in "Configuring" waiting for the device to report the new version.
                rb = r.get("body") or {}
                cv, sid = rb.get("configVersion"), rb.get("sequenceId")
                print(f"  ★★★ SET_REQUEST (provisioning) CAPTURED — configVersion={cv} seq={sid} ★★★")
                globals()["APPLIED_CONFIG_VERSION"] = cv if cv is not None else globals().get("APPLIED_CONFIG_VERSION", 0)
                send(SET_RESPONSE, {"sequenceId": sid, "configVersion": cv}, error=0)
                print(f"  → SET_RESPONSE (applied configVersion={cv})")
            elif t == INFORM_REQUEST:
                send(INFORM_RESPONSE, error=0)
                print("  → INFORM_RESPONSE (ok)")
            elif t == ADOPT_REQUEST:
                try:
                    payload = rsa_decrypt_pub(base64.b64decode((r.get("body") or {}).get("data")))
                    u, _, m = payload.partition(b"\x00")
                    print(f"  *** ACCOUNT DECRYPTED: username='{u.decode()}' MD5(pass)={m.decode()} ***")
                except Exception as e:
                    print(f"  (decrypt failed: {e})")
        return captured
    finally:
        sock.close()


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    print(f"UDP :{s.getsockname()[1]} bound; device ip={DEVICE_IP}. Discovery keepalive + listen.")
    print(">>> tap ADOPT — will receive PRE_ADOPT_REQUEST and decrypt the account <<<")
    pkt = build_discovery(DEVICE_IP)
    stop = threading.Event()
    def beacon():
        while not stop.is_set():
            s.sendto(pkt, (HOST, DISC_PORT)); stop.wait(6)
    threading.Thread(target=beacon, daemon=True).start()

    def wait_pre_adopt(timeout):
        s.settimeout(2)
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                data, _ = s.recvfrom(65535)
            except socket.timeout:
                continue
            if len(data) < 4:
                continue
            n = struct.unpack(">I", data[:4])[0]
            try:
                obj = json.loads(data[4:4 + n].decode("utf-8", "replace"))
            except Exception:
                continue
            if obj.get("header", {}).get("type") == PRE_ADOPT_REQUEST:
                return obj.get("body", {}).get("adoptPort", 29814)
        return None

    while not stop.is_set():
        # 1) Proactive re-link: a real adopted device reconnects on its own after going offline —
        #    it opens the manage channel itself (no manual re-adopt). Works once adopted; for an
        #    un-adopted device the controller closes it quickly (adopt info null) and we fall through.
        print("↻ proactive re-link attempt on 29814 (auto-reconnect, no tap needed)")
        got = [c.get("header", {}).get("type") for c in (drive_adopt_channel(29814) or [])]
        if SYSTEM_NEGOTIATION in got or SET_REQUEST in got:
            print(f"  (was connected; channel dropped {got}; re-linking)")
            continue
        # 2) Not adopted yet → wait for a manual Adopt tap (PRE_ADOPT_REQUEST) to do first adoption.
        print("  (proactive re-link declined — not adopted yet; waiting for an Adopt tap)")
        ap = wait_pre_adopt(25)
        if ap:
            print(f"◀◀ PRE_ADOPT_REQUEST — adoptPort={ap}; driving first adoption")
            drive_adopt_channel(ap)
    stop.set()


if __name__ == "__main__":
    main()
