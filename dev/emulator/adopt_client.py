#!/usr/bin/env python3
"""adopt_client.py — stateful device emulator driving the v2 adopt handshake (dev/adopt-handshake.md).

BREAKTHROUGH (2026-08-21): the v2 manage/adopt channel (TCP 29814) is **TLS-wrapped**. Inside the
TLS tunnel the frames are plaintext length-prefixed JSON (4-byte BE length + {header,body}); RC4 only
turns on later, after adopt, via reinitRc4Handler(sessionKey). My earlier "silent close" was the TLS
handshake failing on raw bytes — nothing reached the decoder. Ports 29811/29812 are the plaintext v1
channels; 29814 is TLS v2.

Sequence (device ⇄ controller), all inside one TLS connection to 29814:
  1. discovery keepalive (UDP 29810) keeps the device Pending (NO_MANAGED, context alive)
  2. user taps Adopt → controller: context → ADOPTING_PRE_CONNECT, "watch adopt result"
  3. device sends PRE_CONNECT_INFO (type 3) → adopt subscriber advances state, replies
     PRE_CONNECT_INFO_RESPONSE / AdoptRequest (account + deviceKey, RC4+RSA-signed)
  4. device sends DEVICE_VERIFY_INFO (type 1048577) with the assigned account → verify passes
     (status now ADOPTING_DEVICE_VERIFY) → DEVICE_VERIFY_RESPONSE
  5. ADOPT_RESPONSE → INFORM loop → SET_REQUEST (type 4096) = the site WiFi/WAN provisioning push.

This client keeps discovery alive on a thread and drives the manage channel, DUMPING every frame the
controller sends so we read the real payloads (no guessing — DEBUG on the controller confirms parse).
Lab only — localhost Docker controller.
"""
import json
import os
import socket
import ssl
import struct
import sys
import threading
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from discovery_beacon import build_discovery, FAKE_MAC, FW_VERSION, DEVICE_TYPE
from manage_crypto import ecsp2_auth

HOST = "127.0.0.1"
DISC_PORT, MGMT_PORT = 29810, 29814          # 29814 = TLS v2 manage/adopt

PRE_CONNECT_INFO   = 3
DEVICE_VERIFY_INFO = 1048577

_seq = 0
def _next_seq():
    global _seq
    _seq += 1
    return _seq


def frame(obj) -> bytes:
    b = json.dumps(obj, separators=(",", ":")).encode()
    return struct.pack(">I", len(b)) + b


def msg(mtype, body=None, error=0):
    return {"header": {"seq": _next_seq(), "version": FW_VERSION, "device": DEVICE_TYPE,
                       "mac": FAKE_MAC, "type": mtype, "timestamp": int(time.time()), "error": error},
            "body": body or {}}


def discovery_keepalive(stop):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    pkt = build_discovery("127.0.0.1")
    while not stop.is_set():
        s.sendto(pkt, (HOST, DISC_PORT))
        stop.wait(8)
    s.close()


def tls_connect():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    raw = socket.create_connection((HOST, MGMT_PORT), timeout=6)
    return ctx.wrap_socket(raw, server_hostname="127.0.0.1")


def read_frame(sock):
    def recv_all(n):
        buf = b""
        while len(buf) < n:
            c = sock.recv(n - len(buf))
            if not c:
                return None
            buf += c
        return buf
    lb = recv_all(4)
    if lb is None:
        return None
    n = struct.unpack(">I", lb)[0]
    body = recv_all(n)
    if body is None:
        return None
    try:
        return json.loads(body.decode("utf-8", "replace"))
    except Exception:
        return {"_raw": body[:300].decode("latin-1", "replace")}


def drive_manage_channel():
    """One TLS session: PRE_CONNECT_INFO → read → VERIFY → read, dumping everything."""
    try:
        sock = tls_connect()
    except Exception as e:
        return f"TLS connect failed: {e}"
    log = []
    try:
        # 1) PRE_CONNECT_INFO — the device's opening move on the adopt channel
        sock.sendall(frame(msg(PRE_CONNECT_INFO)))
        log.append("→ sent PRE_CONNECT_INFO")
        sock.settimeout(6)
        r = read_frame(sock)
        if r is None:
            return " | ".join(log) + " | ◀ channel closed after PRE_CONNECT (state not ADOPTING? tap Adopt)"
        htype = r.get("header", {}).get("type")
        log.append(f"◀ RESP type={htype}: {json.dumps(r)[:500]}")

        # 2) DEVICE_VERIFY_INFO — factory admin/admin first; real account comes from the AdoptRequest
        rnd = os.urandom(16).hex()
        sock.sendall(frame(msg(DEVICE_VERIFY_INFO,
                               {"auth": ecsp2_auth("admin", "admin", rnd),
                                "randomKeyForSystemVerify": rnd})))
        log.append("→ sent DEVICE_VERIFY_INFO")
        r = read_frame(sock)
        if r is not None:
            log.append(f"◀ VERIFY RESP: {json.dumps(r)[:500]}")
    except socket.timeout:
        log.append("(timeout waiting for controller)")
    except Exception as e:
        log.append(f"(error: {e})")
    finally:
        sock.close()
    return " | ".join(log)


def main():
    stop = threading.Event()
    t = threading.Thread(target=discovery_keepalive, args=(stop,), daemon=True)
    t.start()
    print(f"stateful client {FAKE_MAC}: discovery keepalive up. TLS adopt-channel drive on 29814.")
    print(">>> TAP ADOPT in the Omada app now. <<<")
    time.sleep(3)
    try:
        for cycle in range(40):                  # ~4 min window
            res = drive_manage_channel()
            print(f"[{cycle:2d}] {res}")
            if "VERIFY RESP" in res or "type=1048576" in res or "type=16" in res:
                print("*** manage channel advanced — capturing! ***")
            time.sleep(5)
    finally:
        stop.set()


if __name__ == "__main__":
    main()
