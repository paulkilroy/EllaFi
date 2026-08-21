#!/usr/bin/env python3
"""adopt_client.py — bidirectional device emulator: send discovery AND listen for the controller's
reply. Step 1 of the adopt build (dev/adopt-handshake.md): discover what the controller sends back
after Adopt is tapped (pre-connect / adopt instruction), which drives the TCP manage-channel step.

Lab only — talks to the Docker controller on localhost.
"""
import json
import socket
import struct
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from discovery_beacon import build_discovery, FAKE_MAC  # reuse the proven discovery packet


def parse_frame(data: bytes):
    """[4-byte BE len][JSON] — same framing both directions."""
    if len(data) < 4:
        return None, data
    n = struct.unpack(">I", data[:4])[0]
    if len(data) - 4 < n:
        return None, data
    try:
        return json.loads(data[4:4 + n].decode("utf-8", "replace")), data[4 + n:]
    except Exception:
        return {"_raw": data[4:4 + n].decode("latin-1", "replace")}, data[4 + n:]


def main():
    host, port = ("127.0.0.1", 29810)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    s.settimeout(3)
    pkt = build_discovery("127.0.0.1")
    src_port = s.getsockname()[1]
    print(f"listening on udp/{src_port}; discovering as {FAKE_MAC} → {host}:{port}")

    got = 0
    for cycle in range(6):                       # ~30s: discover, then listen for a reply
        s.sendto(pkt, (host, port))
        print(f"  [{cycle}] discovery sent")
        deadline = time.time() + 4
        while time.time() < deadline:
            try:
                data, addr = s.recvfrom(65535)
            except socket.timeout:
                break
            got += 1
            msg, _ = parse_frame(data)
            print(f"  ◀ REPLY from {addr[0]}:{addr[1]}  ({len(data)} B)")
            print("    " + json.dumps(msg)[:300])
        time.sleep(1)
    print(f"done — {got} reply datagram(s) received")
    s.close()


if __name__ == "__main__":
    main()
