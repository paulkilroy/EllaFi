#!/usr/bin/env python3
"""adopt_full.py — Omada ECSP v2 device emulator, rewritten to the protocol map.

Implements omada-lab/omada-protocol-reference.md §0 (the emulator contract). The whole device-plane is a
state machine over ONE persistent TLS socket on 29814. Correctness points that were each a hard-won
bug before the full protocol map (see the reference for citations):

  1. header.timestamp is MILLISECONDS. The manager sets a connected device's lastSeen straight from
     the inform timestamp; sending seconds → lastSeen≈1970 → Heartbeat-Missed flap. (THE flap fix.)
  2. ONE socket per session; every INFORM goes on the socket that finished the handshake, else the
     controller's connected-context guard closes the channel (context/c.java:334 → a/b/d.java).
  3. Drive all the way to CONNECTED (send INIT_SYNC_RESULT), not just ADOPT_SUCCESS.
  4. INFORM carries a ms timestamp + a changing sequenceId (equal seq = idle → eventually missed).
  5. SET_RESPONSE echoes the SET_REQUEST's header.seq (config RPC correlates by getSeq()).
  6. RC4 is asymmetric on v2: decode controller→device frames with RC4 after DEVICE_NEGOTIATION;
     send device→controller frames as PLAINTEXT length-prefixed JSON inside TLS.
  7. Stop the discovery beacon once adopted (a real AP does); resume only on a genuine drop.
  8. Reconnect = a fresh PRE_CONNECT_INFO verifying with the LEARNED site Device Account; back off,
     never storm (a silent close is a manager null-branch a retry can't fix).

Lab only — localhost Docker controller.
"""
import base64
import json
import os
import random
import socket
import ssl
import struct
import sys
import threading
import time
import uuid

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from discovery_beacon import build_discovery, FAKE_MAC, FW_VERSION, DEVICE_TYPE, FAKE_MODEL, EMU_NAME, EMU_FW
from manage_crypto import ecsp2_auth, ecsp2_auth_md5, rsa_decrypt_pub, rsa_encrypt_pub, rc4

# ── wire constants (omada-lab/omada-protocol-reference.md §3) ───────────────────────────────────────────
PRE_CONNECT_INFO, PRE_ADOPT_REQUEST, ADOPT_REQUEST = 3, 2, 16
PRE_CONNECT_INFO_RESPONSE = 1048576
DEVICE_VERIFY_INFO, DEVICE_VERIFY_RESPONSE, SYSTEM_VERIFY_RESULT = 1048577, 1048578, 1048579
DEVICE_NEGOTIATION, SYSTEM_NEGOTIATION, INIT_SYNC_RESULT = 1048580, 1048581, 1048582
VERIFY_RESULT_ACK, INIT_SYNC_RESULT_ACK, INIT_SYNC = 1048585, 1048586, 4352
INFORM_REQUEST, INFORM_RESPONSE = 256, 512
SET_REQUEST, SET_RESPONSE = 4096, 8192

# 6.2 binds discovery UDP IPv6-only; Docker Desktop for Mac won't forward host→container UDP, so on 6.2 the
# emulator MUST run INSIDE the docker net and reach the controller by service name. Set OMADA_HOST=controller.
HOST = os.environ.get("OMADA_HOST", "127.0.0.1")
DISC_PORT, ADOPT_PORT = 29810, 29814
DEVICE_IP = os.environ.get("OMADA_DEVICE_IP", "192.168.65.1")   # this device's IP as the controller sees it

# The device's FACTORY default — its built-in password, used ONLY for the first handshake (the
# controller always includes it as a candidate for a fresh device). NOT typed by the operator.
FACTORY_USER = os.environ.get("VERIFY_USER", "admin")
FACTORY_PASS = os.environ.get("VERIFY_PASS", "admin")

# Genuine EAP225-Outdoor(US) v1.0 hardware identity (from the controller's own model template —
# initDeviceModelTemplateData.json adopt_resp). The cloud firmware check matches on these; fake ones
# never match, so no upgrade target is ever created. Sent RC4-encrypted in DEVICE_NEGOTIATION.
EMU_HWID  = os.environ.get("EMU_HWID",  "25D5A049380DDBA7AF3A96CC2DCB5986")
EMU_OEMID = os.environ.get("EMU_OEMID", "4AA4229D24E7E3DD29CE0F6334559DB9")

# The LEARNED site Device Account: after a clean adoption the controller pushes it as
# userAccount{newUsername, newPassword=UPPER MD5} in SYSTEM_NEGOTIATION. A real device stores it (NVS)
# and verifies EVERY reconnect with it. We persist it to a gitignored file and reload on (re)start so a
# driver restart == a device reboot.
LEARNED_ACCOUNT = None                # {"user": str, "md5": UPPER hex}
ACCOUNT_FILE = os.path.join(os.path.dirname(__file__), f"device-account-{FAKE_MAC}.local")  # per-MAC; *.local gitignored

RELINK_BACKOFF_SECS = 15              # a real AP retries a dropped link with delay; never spin (was 45k-attempt storm)

IS_CONNECTED = threading.Event()      # set while we hold a live CONNECTED manage channel → pauses the discovery beacon


# ── message framing (§1) ──────────────────────────────────────────────────────────────────────────
_seq = 0
def _next_seq():
    global _seq; _seq += 1; return _seq

def _now_ms():
    return int(time.time() * 1000)    # header.timestamp is Long MILLISECONDS (contract §0.1 — the flap fix)

def msg(mtype, body=None, error=0, seq=None):
    """Build a {header, body}. seq: for a RESPONSE to a controller-initiated REQUEST (SET_RESPONSE),
    pass the REQUEST's header.seq so the controller's RPC waiter (keyed by getSeq()) matches. For
    device-initiated messages pass nothing → our own counter."""
    return {"header": {"seq": seq if seq is not None else _next_seq(), "version": FW_VERSION,
                       "device": DEVICE_TYPE, "mac": FAKE_MAC, "type": mtype,
                       "timestamp": _now_ms(), "error": error},
            "body": body or {}}

def _frame(obj):
    b = json.dumps(obj, separators=(",", ":")).encode()
    return struct.pack(">I", len(b)) + b     # 4-byte big-endian length + UTF-8 JSON


def _recv_all(sock, n):
    buf = b""
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c:
            return None
        buf += c
    return buf

def read_frame(sock, rc4_key=None):
    """Read one controller→device frame. Before negotiation frames are plaintext; after
    DEVICE_NEGOTIATION the controller RC4s its OUTBOUND (asymmetric v2 reinit), per-field with the
    keystream reset each field: RC4(len4) ++ RC4(body). Try plaintext length first; if implausible and
    we hold the session key, interpret as RC4."""
    lb = _recv_all(sock, 4)
    if lb is None:
        return None
    n = struct.unpack(">I", lb)[0]
    if 0 < n < 2_000_000:                    # plausible plaintext length
        body = _recv_all(sock, n)
        if body is None:
            return None
        try:
            return json.loads(body.decode("utf-8", "replace"))
        except Exception:
            if rc4_key is not None:          # consumed bytes weren't plaintext JSON → try RC4 on the body
                try:
                    return json.loads(rc4(body, rc4_key).decode("utf-8", "replace"))
                except Exception:
                    return {"_raw": body[:200].hex()}
            return {"_raw": body[:200].hex()}
    if rc4_key is not None:                   # length implausible as plaintext → RC4-framed
        total = struct.unpack(">I", rc4(lb, rc4_key))[0]
        m = total - 4
        if not (0 < m < 2_000_000):
            return {"_badlen": total}
        body = _recv_all(sock, m)
        if body is None:
            return None
        try:
            return json.loads(rc4(body, rc4_key).decode("utf-8", "replace"))
        except Exception:
            return {"_raw_rc4": rc4(body, rc4_key)[:200].hex()}
    return {"_badlen": n}

def _read_or_none(sock, rc4_key=None):
    try:
        return read_frame(sock, rc4_key)
    except socket.timeout:
        return "TIMEOUT"
    except Exception as e:
        return f"ERR:{e}"


def tls_connect(port):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE          # controller uses server cert only, no client auth (§1)
    raw = socket.create_connection((HOST, port), timeout=6)
    return ctx.wrap_socket(raw, server_hostname="127.0.0.1")


# ── learned Device Account (§4 cred c) ────────────────────────────────────────────────────────────
def _load_account():
    global LEARNED_ACCOUNT
    try:
        with open(ACCOUNT_FILE) as fh:
            LEARNED_ACCOUNT = json.load(fh)
        print(f"  (loaded stored device account: user='{LEARNED_ACCOUNT['user']}' md5={LEARNED_ACCOUNT['md5'][:8]}…)")
    except Exception:
        LEARNED_ACCOUNT = None

def _save_account(user, md5_hex):
    global LEARNED_ACCOUNT
    LEARNED_ACCOUNT = {"user": user, "md5": md5_hex.upper()}
    with open(ACCOUNT_FILE, "w") as fh:
        json.dump(LEARNED_ACCOUNT, fh)

def _find_user_account(obj):
    """Recursively locate userAccount{newUsername,newPassword} in a SYSTEM_NEGOTIATION / SET_REQUEST body."""
    if isinstance(obj, dict):
        ua = obj.get("userAccount")
        if isinstance(ua, dict) and ua.get("newUsername") is not None and ua.get("newPassword"):
            return ua
        for v in obj.values():
            r = _find_user_account(v)
            if r:
                return r
    elif isinstance(obj, list):
        for v in obj:
            r = _find_user_account(v)
            if r:
                return r
    return None


# ── INFORM body (§0.4) — LIVE vitals; drives the health graph + keeps lastSeen fresh ───────────────
_INFORM_START = time.time()
_inform_seq = 0
APPLIED_SSIDS = {}
APPLIED_CONFIG_VERSION = 0

# ── synthetic wireless clients — a live, believable AP load for the dashboards + health graph ───────
# A population that PERSISTS across informs: cumulative down/up climb, RSSI drifts, clients occasionally
# join/leave. EapInformBody.clients = List<ClientStats>; radioTraffic_2G/5G + ssidStats aggregate from it.
EMU_CLIENT_COUNT = int(os.environ.get("EMU_CLIENTS", "6"))   # 0 disables synthetic clients
EMU_SSID = os.environ.get("EMU_SSID", "EllaFi-WiFi")         # fallback SSID name when none configured yet
# Option (A): pin a specific client into this AP's client list — e.g. the piso node showing up as a
# real Wi-Fi client (downlink) of the upstream AP, with a stable MAC/name and its uplink RSSI.
EMU_DOWNLINK_MAC  = os.environ.get("EMU_DOWNLINK_MAC")
EMU_DOWNLINK_NAME = os.environ.get("EMU_DOWNLINK_NAME", "Downlink-Device")
EMU_DOWNLINK_RSSI = int(os.environ.get("EMU_DOWNLINK_RSSI", "-50"))
_CLIENTS = None
_VENDOR_OUI = ["F0:18:98", "A4:83:E7", "3C:22:FB", "DC:A6:32", "B8:27:EB",
               "50:32:37", "1C:57:DC", "AC:37:43", "F4:F5:D8", "00:1A:2B"]
_HOSTNAMES = ["Juan-iPhone", "Maria-Galaxy", "kiosk", "Ana-Laptop", "cctv-cam",
              "Pedro-Redmi", "frontdesk-PC", "Lola-iPad", "guest", "vendo-piso"]

def _dur(sec):   # ClientStats.activeTime ("time") is parsed by X.k as "<D> days HH:MM:SS" (StringTokenizer)
    return "%d days %02d:%02d:%02d" % (sec // 86400, sec % 86400 // 3600, sec % 3600 // 60, sec % 60)

def _rand_mac():
    return random.choice(_VENDOR_OUI) + ":" + ":".join("%02X" % random.randint(0, 255) for _ in range(3))

def _new_client():
    return {"mac": _rand_mac(), "rid": random.choice([0, 0, 1]),   # weight toward 2.4G
            "name": random.choice(_HOSTNAMES) + "-%02d" % random.randint(1, 99),
            "ip": "192.168.16.%d" % random.randint(20, 240),
            "rssi": random.randint(-72, -38),
            "down": random.randint(2_000_000, 80_000_000), "up": random.randint(500_000, 20_000_000),
            "rxP": random.randint(5_000, 200_000), "txP": random.randint(4_000, 150_000),
            "time": random.randint(60, 7200), "rate": random.choice([144, 300, 433, 585, 866, 1200])}

def _synth_clients():
    """Evolve the population in place: traffic accrues, signal drifts, rare join/leave churn."""
    global _CLIENTS
    if _CLIENTS is None:
        _CLIENTS = [_new_client() for _ in range(EMU_CLIENT_COUNT)]
        if EMU_DOWNLINK_MAC:                 # pin the downlink device as client #0 (stable mac/name)
            _CLIENTS.insert(0, {"mac": EMU_DOWNLINK_MAC.replace("-", ":").upper(), "rid": 1,
                                "name": EMU_DOWNLINK_NAME, "ip": "192.168.16.10", "rssi": EMU_DOWNLINK_RSSI,
                                "down": 50_000_000, "up": 30_000_000, "rxP": 100_000, "txP": 80_000,
                                "time": 3600, "rate": 866, "pinned": True})
    for c in _CLIENTS:
        c["down"] += random.randint(20_000, 3_000_000); c["up"] += random.randint(5_000, 800_000)
        c["rxP"] += random.randint(50, 3_000);          c["txP"] += random.randint(40, 2_500)
        c["time"] += 3
        c["rssi"] = max(-85, min(-30, c["rssi"] + random.randint(-2, 2)))
    # churn only non-pinned clients
    pop_idx = [i for i, c in enumerate(_CLIENTS) if not c.get("pinned")]
    if len(pop_idx) > 1 and random.random() < 0.05:
        _CLIENTS.pop(random.choice(pop_idx))
    if random.random() < 0.05:
        _CLIENTS.append(_new_client())
    return _CLIENTS

def _client_band_ssid(rid):
    band = "2G" if rid == 0 else "5G"
    lst = APPLIED_SSIDS.get(band, [])
    return band, (lst[0]["ssidName"] if lst else EMU_SSID), (lst[0]["id"] if lst else 0)

# ── device log / error injection (Tier 1: rides the inform "log" = EapSysLog) ───────────────────────
# Drop JSON lines {"k":"<catalogKey>","c":"<free-text message>"} into INJECT_FILE; the next inform
# carries them as EapSysLogEntry{k,c,t}. Controller shows content VERBATIM (C0022w:526, not templated);
# key must be a valid catalog type or it's dropped ("Ignore unknown eap log type"). One-shot: file is
# consumed each inform. This is the harness for surfacing EllaFi critical errors in Device → Logs.
INJECT_FILE = os.path.join(os.path.dirname(__file__), f"_inject-{FAKE_MAC}.jsonl")

def _pending_logs():
    if not os.path.exists(INJECT_FILE):
        return []
    out = []
    try:
        with open(INJECT_FILE) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                e = json.loads(line)
                out.append({"k": e["k"], "c": e.get("c", ""), "t": _now_ms()})
        os.remove(INJECT_FILE)               # one-shot: consume the queue
        if out:
            print(f"  ⚠ injecting {len(out)} log entr{'y' if len(out)==1 else 'ies'}: {[e['k'] for e in out]}")
    except Exception as ex:
        print(f"  (inject read error: {ex})")
    return out

# ── mesh topology (2-node): report a wireless uplink (child) or downlink (parent) → the "Link" section ──
# Child self-reports candidateParents[parent+rssi] + meshInfo.status!=0 (wireless-linked) → controller
# stores WirelessUplinkInfo{mac,rssi,snr} and draws child→parent in topology (parent must be managed).
EMU_MESH_PARENT = os.environ.get("EMU_MESH_PARENT")   # this device is a wireless CHILD of this parent MAC
EMU_MESH_CHILD  = os.environ.get("EMU_MESH_CHILD")    # this device is the mesh PARENT of this child MAC
EMU_MESH_RSSI   = int(os.environ.get("EMU_MESH_RSSI", "-55"))
def _colon(mac):
    return mac.replace("-", ":").upper()

def _mesh_info():
    if EMU_MESH_PARENT:      # child: advertise the parent as a candidate with the link RSSI
        return {"status": 1, "meshRid": 1,
                "candidateParents": {"status": 0, "parentList": [
                    {"mac": _colon(EMU_MESH_PARENT), "rssi": EMU_MESH_RSSI, "snr": max(5, EMU_MESH_RSSI + 95),
                     "ch": 36, "meshVer": 1, "radioId": 1}]},
                "childAPs": [], "isolatedAPs": [], "childAPRec": []}
    if EMU_MESH_CHILD:      # parent: report the downlink child with the link RSSI
        return {"status": 0, "meshRid": 1,
                "childAPs": [{"mac": _colon(EMU_MESH_CHILD), "txR": "300", "rxR": "300", "up": 0, "down": 0,
                              "upP": 0, "downP": 0, "rssi": EMU_MESH_RSSI, "snr": max(5, EMU_MESH_RSSI + 95),
                              "channelWidth": 2}],
                "candidateParents": {"status": 0, "parentList": []}, "isolatedAPs": [], "childAPRec": []}
    return None

def inform_body():
    """Per-radio WirelessInfo (busyUti/interUti/txUti/rxUti) → the device Health spider graph; deviceInfo
    cpu/mem/temp; ssidStats = configured SSIDs 'on the air'. MUST progress each inform (upTime climbs +
    changing sequenceId) or the manager flags informIdle. header.timestamp (ms, via msg()) sets lastSeen."""
    global _inform_seq
    _inform_seq += 1
    up = 3600 + int(time.time() - _INFORM_START)
    j = up % 7
    def winfo(ch, bw, txp, busy, inter):
        b = busy + (j % 3)
        # txR/txPower MUST match the controller's ApWlanInformDecoder parsers or it throws and ABORTS the
        # whole inform (no lastSeen persist → Heartbeat-Missed). txRate: h.a() does substring(0,lastIndexOf('.'))
        # → needs a decimal point. txPower: h.b() does substring(0,len-3) → blank is safe (R.a→0). (found via Arthas)
        return {"region": 1, "ch": ch, "bw": bw, "rdMode": "11ax", "txR": "0.0", "txPower": "",
                "txUti": max(0, b - inter - 5), "rxUti": max(0, b - inter - 8),
                "interUti": inter, "busyUti": b, "aiRoamingOffset": 0}
    def _bssid(offset):
        parts = FAKE_MAC.replace("-", ":").split(":")
        parts[-1] = "%02X" % ((int(parts[-1], 16) + offset) & 0xFF)
        return ":".join(parts)
    # build the live client population, aggregating per-band traffic for ssidStats + radioTraffic
    ap_mac = FAKE_MAC.replace("-", ":")
    clients = _synth_clients() if EMU_CLIENT_COUNT else []
    _empty = lambda: {"n": 0, "down": 0, "up": 0, "rxP": 0, "txP": 0}
    band_agg = {"2G": _empty(), "5G": _empty()}
    client_entries = []
    for c in clients:
        band, ssid, _sid = _client_band_ssid(c["rid"])
        rate = c["rate"]
        client_entries.append({
            "mac": c["mac"], "rid": c["rid"], "ap": ap_mac, "ssid": ssid,
            "rssi": c["rssi"], "snr": max(5, c["rssi"] + 95), "ccq": max(40, min(99, 140 + c["rssi"])),
            "rate": rate, "down": c["down"], "up": c["up"], "time": _dur(c["time"]), "ip": c["ip"],
            "name": c["name"], "type": 0, "pm": 5, "txR": rate * 1000, "rxR": rate * 1000,
            "txT": random.randint(1000, 50000), "rxP": c["rxP"], "txP": c["txP"],
            "aTime": _now_ms() - c["time"] * 1000, "bw": (1 if band == "2G" else 3), "guest": 0})
        a = band_agg[band]
        a["n"] += 1; a["down"] += c["down"]; a["up"] += c["up"]; a["rxP"] += c["rxP"]; a["txP"] += c["txP"]

    def ssid_stats(band, base):
        out = []
        for i, s in enumerate(APPLIED_SSIDS.get(band, [])):
            a = band_agg[band] if i == 0 else _empty()   # attribute the band's clients to its first SSID
            out.append({"id": s["id"], "ssid": s["ssidName"], "clntNum": a["n"], "down": a["down"],
                        "up": a["up"], "downPkts": a["rxP"], "upPkts": a["txP"],
                        "bssid": _bssid(base + i), "rxS": 0, "txS": 0})
        return out

    def radio_traffic(band):
        a = band_agg[band]
        return {"tx": a["down"], "rx": a["up"], "txP": a["txP"], "rxP": a["rxP"],
                "txRP": int(a["txP"] * 0.02), "rxRP": int(a["rxP"] * 0.01), "txEP": 0, "rxEP": 0, "badfcs": 0}

    return {
        # sequenceId changes every inform so the manager never treats it as an idle repeat (§0.4).
        "sequenceId": _inform_seq,
        "timeStamp": _now_ms(),
        "deviceInfo": {"name": EMU_NAME, "model": FAKE_MODEL,
                       "firmwareVersion": EMU_FW, "hardwareVersion": "1.0",
                       # stats subscriber R.k() parses uptime as "<days> days HH:MM:SS" (StringTokenizer);
                       # a bare seconds string throws NoSuchElementException (swallowed, breaks stats/health graph).
                       "upTime": "%d days %02d:%02d:%02d" % (up // 86400, up % 86400 // 3600, up % 3600 // 60, up % 60),
                       "ip": DEVICE_IP, "cpuUti": 10 + j, "memUti": 40 + (j % 4),
                       "txRate": 0, "rxRate": 0, "wirelessLinked": True, "temp": [40 + (j % 5)],
                       "nf": [-95, -92]},
        "wSettings_2G": winfo("6", "20", "20", 25, 5),
        "wSettings_5G": winfo("36", "80", "23", 30, 4),
        "ssidStats_2G": ssid_stats("2G", 0),
        "ssidStats_5G": ssid_stats("5G", 8),
        "radioTraffic_2G": radio_traffic("2G"),
        "radioTraffic_5G": radio_traffic("5G"),
        "clients": client_entries,     # EapInformBody.clients = List<ClientStats> → the Clients page + counts
        "log": {"logs": _pending_logs()},   # EapSysLog → Device→Logs (Alerts/Events); content verbatim
        "mesh": _mesh_info(),          # None unless EMU_MESH_* set → topology uplink/downlink + link RSSI
    }


# ── DEVICE_NEGOTIATION body (proven; §4 step 7) ───────────────────────────────────────────────────
def negotiation_body(session_key):
    """ApAdoptRespV2Body: {key=RSA-wrapped RC4 session key, devCap, radioCap, components, deviceInfo}.
    The controller builds the config from devCap/radioCap (NPEs if devCap/logNotification missing)."""
    dev_cap = {
        "supportPa": 0, "meshChainNum": 0, "supportAIRoaming": 0, "supportGPON": False, "supportVoip": False,
        "lanBandwidthControlPorts": [], "voipPorts": [], "supportPPSK": 0, "ppskNum": 0, "ppskNumV3": 0,
        "supportLldp": 0, "supportRadioMode2G": 1, "supportRadioMode5G": 1, "supportRadioMode6G": 0,
        "supportOuiMacFilter": 0, "maxTemps": [], "supportOwe": False, "supportBackground": 0,
        "supportSfp": False, "availableUplinkPorts": [], "powerLimits": [], "notSupportLanClient": 0,
        "p2pRoleDefault": 0, "supportP2pRoleEdit": False, "supportAfc": False, "supportEIRP": 0,
        "noSupportFeatures": [], "logNotification": {"supportLogKey": []},
    }
    return {
        "key": base64.b64encode(rsa_encrypt_pub(session_key)).decode(),
        "configVersion": APPLIED_CONFIG_VERSION,      # 0 fresh; N after we applied SET_REQUEST vN
        "devCap": dev_cap,
        "controllerSetting": {"controllerId": "c21f969b5f03d33d43e04f8f136e7682", "destOmadacId": ""},
        "components": {},
        "components_v2": {"wlanBasic": "2.2", "ssidInform": "2.0", "portal": "2.1", "portalAct": "2.0",
                          "macfilter": "2.2", "multicastFilter": "1.0", "qos": "1.0", "led": "1.0",
                          "ssh": "1.0", "loopback": "1.0", "p2pSetting": "1.1", "anteGain": "1.0",
                          "ipv6Group": "1.0", "wifiLedCtrl": "1.0", "upgrade": "1.0",
                          # supportAnomaly (→ Device Health score/graph) = ap/d.java k(): V2 &&
                          # componentInfo.isSupportByComponentVer(ABNORMAL_DETECT="abnormalDetect", 1, 0)
                          "abnormalDetect": "1.1"},
        "notSupportComponents": {},
        "radioCap": [
            {"radioId": 0, "minPow": 1, "maxPow": 20, "maxPowOd": 20, "limitMaxPow": 20, "mimo": 2,
             "ofdma": 0, "ofdmaEnable": 0, "mcsLevel": 9, "supportSsidNum": 8, "supportMaxClient": 128,
             "supportMlo": False, "supportMaxAssocClient": 128, "supportAnteGainSetting": 0, "supportAntennaDirection": []},
            {"radioId": 1, "minPow": 1, "maxPow": 23, "maxPowOd": 23, "limitMaxPow": 23, "mimo": 2,
             "ofdma": 0, "ofdmaEnable": 0, "mcsLevel": 11, "supportSsidNum": 8, "supportMaxClient": 128,
             "supportMlo": False, "supportMaxAssocClient": 128, "supportAnteGainSetting": 0, "supportAntennaDirection": []},
        ],
        "deviceMisc": {"support2g": True, "support5g": True, "support5g2": False, "support6g": False,
                       "support_11ac": True, "support_lag": False, "supportMesh": 1, "customizeRegion": 0,
                       "lanPortsNum": 1, "support_channelLimit": False, "supportDfs": 0, "supportRoaming": 1},
        "deviceInfo": {"name": EMU_NAME, "model": FAKE_MODEL, "modelVersion": "1.0",
                       "firmwareVersion": EMU_FW, "hardwareVersion": "1.0",
                       "upTime": "3600", "cpuUtil": 5, "memUtil": 37, "wirelessLinked": False,
                       # V2 deviceInfo (ApAdoptDeviceInfoV2) reads PLAIN "hwId"/"oemId" keys and encrypts
                       # them itself (getEncryptedOemId = b.c(oemId)). Sending "encryptedOemId" (old bug) →
                       # V2 parser ignores it → oemId null → cloud firmware query can't run.
                       "hwId": EMU_HWID,
                       "oemId": EMU_OEMID},
    }


# ── the adoption / connected state machine over ONE socket (§4, §5) ────────────────────────────────
def drive_channel(port):
    """Open ONE TLS socket and run the whole state machine on it: PRE_CONNECT → verify → negotiation →
    init-sync → CONNECTED → INFORM keepalive + SET handling. Returns the list of message types seen
    (so the caller can tell a real adoption/reconnect from a rejected pre-connect). All device→controller
    frames are PLAINTEXT; controller→device frames are RC4 after DEVICE_NEGOTIATION."""
    global APPLIED_CONFIG_VERSION
    print(f"→ TLS connect manage/adopt :{port}")
    try:
        sock = tls_connect(port)
    except Exception as e:
        print(f"  TLS connect failed: {e}"); return []
    session_key = os.urandom(16)         # our RC4 session key (RSA-wrapped to the controller in DEVICE_NEGOTIATION)
    rc4_key = None                       # set once we start expecting RC4 inbound (after we send DEVICE_NEGOTIATION)
    connected = False
    seen = []
    try:
        # step 2/3 — PRE_CONNECT_INFO (needUsername so the controller returns the synced username)
        sock.sendall(_frame(msg(PRE_CONNECT_INFO, {"needUsername": True})))
        sock.settimeout(6)
        r = _read_or_none(sock)
        if not isinstance(r, dict) or r.get("header", {}).get("type") != PRE_CONNECT_INFO_RESPONSE:
            print(f"  ◀ no PRE_CONNECT_INFO_RESPONSE ({r if not isinstance(r, dict) else r.get('header',{}).get('type')})"
                  " — not adoptable / not yet adopted; giving up this attempt")
            return seen
        body = r.get("body") or {}
        rkdv = body.get("randomKeyForDeviceVerify", "")
        user = body.get("username") or (LEARNED_ACCOUNT or {}).get("user") or FACTORY_USER
        seen.append(PRE_CONNECT_INFO_RESPONSE)
        print(f"  ◀ PRE_CONNECT_INFO_RESPONSE: username='{user}' rkdv={rkdv[:8]}…")

        # step 4 — DEVICE_VERIFY. First adoption: factory default. Reconnect (LEARNED_ACCOUNT set): the
        # pushed site Device Account — the factory default is no longer accepted once adopted.
        if LEARNED_ACCOUNT:
            auth = ecsp2_auth_md5(LEARNED_ACCOUNT["user"], LEARNED_ACCOUNT["md5"], rkdv)
            print(f"  → DEVICE_VERIFY_INFO (learned site account user='{LEARNED_ACCOUNT['user']}')")
        else:
            auth = ecsp2_auth(user, FACTORY_PASS, rkdv)
            print(f"  → DEVICE_VERIFY_INFO (factory default user='{user}')")
        # randomKeyForSystemVerify: 6.2 rejects len<36 BEFORE any credential check
        # ("cause getRandomKeyForSystemVerify is empty"). A UUID string is exactly 36 chars.
        rksv = str(uuid.uuid4())
        sock.sendall(_frame(msg(DEVICE_VERIFY_INFO, {"auth": auth, "randomKeyForSystemVerify": rksv})))

        sock.settimeout(10)
        while True:                       # stay on the channel; INFORM keepalive keeps us CONNECTED
            r = _read_or_none(sock, rc4_key)
            if r == "TIMEOUT":
                if connected:
                    im = msg(INFORM_REQUEST, inform_body())
                    sock.sendall(_frame(im))
                    print(f"  ♥ INFORM header.ts={im['header']['timestamp']} body.ts={im['body'].get('timeStamp')} seq={im['header']['seq']}")
                continue
            if not isinstance(r, dict):
                print(f"  ◀ channel ended ({r})"); break
            t = r.get("header", {}).get("type")
            seq = r.get("header", {}).get("seq")
            seen.append(t)
            print(f"  ◀ type={t}{' [rc4]' if rc4_key else ''}: {json.dumps(r)[:400]}")

            if t == DEVICE_VERIFY_RESPONSE:
                sock.sendall(_frame(msg(SYSTEM_VERIFY_RESULT, error=0)))
                print("  → SYSTEM_VERIFY_RESULT")
            elif t == VERIFY_RESULT_ACK:
                # step 7 — hand the controller our RC4 session key; its OUTBOUND flips to RC4 after this.
                sock.sendall(_frame(msg(DEVICE_NEGOTIATION, negotiation_body(session_key))))
                rc4_key = session_key
                print(f"  → DEVICE_NEGOTIATION (RC4 session key {session_key.hex()[:8]}…; inbound now RC4)")
            elif t in (SYSTEM_NEGOTIATION, INIT_SYNC, SET_REQUEST):
                # persist the full decrypted config (may hold credential hashes) to a gitignored file
                with open(os.path.join(os.path.dirname(__file__), "..", "captured-config.local"), "a") as fh:
                    fh.write(json.dumps(r) + "\n")
                ua = _find_user_account(r.get("body") or {})
                if ua:
                    _save_account(ua["newUsername"], ua["newPassword"])
                    print(f"  *** LEARNED site Device Account: user='{ua['newUsername']}' "
                          f"md5(pass)={ua['newPassword']} — stored, verifies on reconnect ***")
                if t in (SYSTEM_NEGOTIATION, INIT_SYNC):
                    # step 9 — ack the initial config sync → context promotes to CONNECTED on THIS socket.
                    sock.sendall(_frame(msg(INIT_SYNC_RESULT, error=0)))
                    connected = True
                    IS_CONNECTED.set()      # adopted+connected → stop the discovery beacon (§0.7)
                    sock.settimeout(3)      # fast INFORM keepalive; interval must stay < informChannelTimeout(5m)
                    print("  → INIT_SYNC_RESULT — device CONNECTED")
                if t == SET_REQUEST:
                    rb = r.get("body") or {}
                    cv, sid = rb.get("configVersion"), rb.get("sequenceId")
                    if cv is not None:
                        APPLIED_CONFIG_VERSION = cv
                    for band, k in (("2G", "ssid_2G"), ("5G", "ssid_5G")):
                        ss = (rb.get(k) or {}).get("ssid")
                        if ss is not None:
                            APPLIED_SSIDS[band] = [{"id": s["id"], "ssidName": s.get("ssidName", "")}
                                                   for s in ss if s.get("operation", 1) != 0 and s.get("enable", True)]
                    # step 10 — SET_RESPONSE MUST echo the request's header.seq so the config RPC completes
                    # (Configuring→Connected). errcode:0 + header error:0 required.
                    sock.sendall(_frame(msg(SET_RESPONSE, {"sequenceId": sid, "errcode": 0, "configVersion": cv},
                                             error=0, seq=seq)))
                    print(f"  ★ SET_REQUEST configVersion={cv} seq={seq} → SET_RESPONSE (header.seq={seq} echoed)"
                          f" | SSIDs={ {b:[s['ssidName'] for s in v] for b,v in APPLIED_SSIDS.items()} }")
            elif t == INFORM_REQUEST:
                sock.sendall(_frame(msg(INFORM_RESPONSE, error=0)))
            elif t == ADOPT_REQUEST:
                try:
                    payload = rsa_decrypt_pub(base64.b64decode((r.get("body") or {}).get("data")))
                    u, _, m = payload.partition(b"\x00")
                    print(f"  *** v1 ADOPT_REQUEST account: user='{u.decode()}' MD5={m.decode()} ***")
                except Exception as e:
                    print(f"  (v1 adopt decrypt failed: {e})")
        return seen
    finally:
        IS_CONNECTED.clear()             # channel gone → allow discovery beacons again (re-adopt fallback)
        sock.close()


def wait_pre_adopt(sock, timeout):
    """Listen on the discovery socket for the controller's PRE_ADOPT_REQUEST (UDP) → return adoptPort."""
    sock.settimeout(2)
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            data, _ = sock.recvfrom(65535)
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
            return obj.get("body", {}).get("adoptPort", ADOPT_PORT)
    return None


def main():
    _load_account()                      # restore the pushed site Device Account (like a device NVS boot)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    print(f"UDP :{s.getsockname()[1]} bound; device ip={DEVICE_IP}. Discovery keepalive + listen.")
    pkt = build_discovery(DEVICE_IP)
    stop = threading.Event()
    def beacon():                        # pauses while IS_CONNECTED (a real AP stops beaconing once adopted, §0.7)
        while not stop.is_set():
            if not IS_CONNECTED.is_set():
                s.sendto(pkt, (HOST, DISC_PORT))
            stop.wait(6)
    threading.Thread(target=beacon, daemon=True).start()

    # A device that already holds a stored account was adopted in a PRIOR run (a reboot) — it must
    # auto-reconnect on its own (proactive PRE_CONNECT), not wait for a re-adopt tap. The controller
    # keeps our creds in its managed model after a clean adoption, so PRE_CONNECT succeeds (§0.8).
    adopted_once = LEARNED_ACCOUNT is not None
    if adopted_once:
        print(f"↺ stored account present (user='{LEARNED_ACCOUNT['user']}') — reboot: auto-reconnecting")

    connected = lambda seen: SYSTEM_NEGOTIATION in seen or INIT_SYNC in seen or SET_REQUEST in seen
    while not stop.is_set():
        # 1) If we're already in the controller's managed model, a proactive PRE_CONNECT re-links us.
        if adopted_once:
            print("↻ proactive re-link (auto-reconnect, no tap)")
            seen = drive_channel(ADOPT_PORT)          # verifies with the Device Account we hold (never admin/admin)
            if connected(seen):
                print(f"  (re-link ended; saw {seen}) — backing off {RELINK_BACKOFF_SECS}s")
                stop.wait(RELINK_BACKOFF_SECS); continue
            # Fresh controller (not in its model yet): reconnect can't work → fall through to first-adopt,
            # but STILL present the Device Account we hold, exactly like a provisioned real device.
            print("  (not in controller model — first-adopt with the Device Account we hold)")
        # 2) First-adopt: wait for PRE_ADOPT, then present whatever credential this device holds.
        #    A real device NEVER asks the operator: if we hold the Device Account we use it; only a truly
        #    factory-fresh device (no stored account) uses the factory default.
        cred = "Device Account" if LEARNED_ACCOUNT else "factory default"
        print(f"  … Pending: beaconing, waiting for PRE_ADOPT (will verify with {cred})")
        ap = wait_pre_adopt(s, 25)
        if ap:
            print(f"◀◀ PRE_ADOPT_REQUEST adoptPort={ap} — driving adoption ({cred})")
            seen = drive_channel(ap)
            if connected(seen):
                adopted_once = True
    stop.set()


if __name__ == "__main__":
    main()
