#!/usr/bin/env python3
"""adopt_client.py — stateful device emulator driving discovery → verify (dev/adopt-handshake.md).

Keeps the device Pending (discovery keepalive) AND repeatedly attempts the manage-channel
DEVICE_VERIFY_INFO. Most verify attempts fail while the device is merely Pending; the one that lands
during the ADOPTING_PRE_CONNECT window (right after you tap Adopt) should draw a DEVICE_VERIFY_RESPONSE
— the first successful manage-channel exchange. Framing tried plaintext first (see spec's open q).

Lab only — localhost Docker controller.
"""
import json
import socket
import struct
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from discovery_beacon import build_discovery, FAKE_MAC, FW_VERSION, DEVICE_TYPE
from manage_crypto import ecsp2_auth, rc4_frame, rc4_unframe

HOST = "127.0.0.1"
DISC_PORT, MGMT_PORT = 29810, 29814  # v2 manage channel
DEVICE_VERIFY_INFO = 1048577


def frame(obj) -> bytes:
    b = json.dumps(obj, separators=(",", ":")).encode()
    return struct.pack(">I", len(b)) + b


def unframe(data: bytes):
    if len(data) < 4:
        return None
    n = struct.unpack(">I", data[:4])[0]
    try:
        return json.loads(data[4:4 + n].decode("utf-8", "replace"))
    except Exception:
        return {"_raw": data[4:4 + n][:200].decode("latin-1", "replace")}


def device_verify_msg():
    rnd = "".join("%02x" % b for b in __import__("os").urandom(16))
    body = {"auth": ecsp2_auth("admin", "admin", rnd), "randomKeyForSystemVerify": rnd}
    header = {"seq": 1, "version": FW_VERSION, "device": DEVICE_TYPE, "mac": FAKE_MAC,
              "type": DEVICE_VERIFY_INFO, "timestamp": int(time.time())}
    return {"header": header, "body": body}       # RC4-framed by the caller (v2 channel)


def try_verify(dsock):
    try:
        m = socket.create_connection((HOST, MGMT_PORT), timeout=4)
    except Exception as e:
        return f"connect failed: {e}"
    def recv_all(n):
        buf = b""
        while len(buf) < n:
            chunk = m.recv(n - len(buf))
            if not chunk:
                break
            buf += chunk
        return buf
    try:
        m.sendall(rc4_frame(device_verify_msg()))     # RC4-framed for the v2 manage channel
        m.settimeout(5)
        msg = rc4_unframe(recv_all)
        if msg is None:
            return "channel closed (no context / status not ADOPTING — tap Adopt during the window)"
        return "◀ RESPONSE: " + json.dumps(msg)[:400]
    except socket.timeout:
        return "no response (waiting / rejected)"
    finally:
        m.close()


def main():
    dsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    disc = build_discovery("127.0.0.1")
    print(f"stateful client: {FAKE_MAC} — discovery keepalive + manage-verify attempts. Tap Adopt now.")
    for cycle in range(30):                       # ~2.5 min
        dsock.sendto(disc, (HOST, DISC_PORT))     # keep Pending
        res = try_verify(dsock)
        print(f"  [{cycle:2d}] verify → {res}")
        if "RESPONSE" in res:
            print("  *** DEVICE_VERIFY_RESPONSE received — handshake advancing! ***")
            break
        time.sleep(5)
    dsock.close()


if __name__ == "__main__":
    main()
