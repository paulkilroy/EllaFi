#!/usr/bin/env python3
"""
Adversary test for the EllaFi node mesh (port 4210). Injects malformed / forged / stale UDP
packets to confirm the master's HMAC + freshness checks reject them.

Wire format (must match src/network.cpp):
    [type: uint16 LE][timestamp_ms: uint64 LE][body][HMAC-SHA256(network_key)[:16]]
The HMAC key is the 8 little-endian bytes of `network_key` (a uint64), read from data/config.json.

Watch the MASTER's serial monitor while this runs (CORE_DEBUG_LEVEL=3 shows the rejections):
    ~/.platformio/penv/bin/platformio device monitor

Usage:
    python3 test/hmac_inject.py                 # broadcast the 3 safe attacks
    python3 test/hmac_inject.py --ip 192.168.0.5   # unicast straight at the master
    python3 test/hmac_inject.py bad-hmac        # just one
    python3 test/hmac_inject.py valid-coin      # POSITIVE control — actually credits a coin!

Safe (no side effects, default):  bad-packet  bad-hmac  stale
Has side effects (opt-in only):   valid-coin  replay     # these credit a real coin on the master
"""
import socket, struct, hashlib, hmac, time, re, os, sys, random

CFG  = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "config.json")
PORT = 4210
COIN_INSERTED = 1           # NetMsgType — see src/network.h
FAKE_MAC = "DE:AD:BE:EF:00:01"

SAFE        = ["bad-packet", "bad-hmac", "stale"]
SIDE_EFFECT = ["valid-coin", "replay"]


def load_key():
    txt = open(CFG).read()
    m = re.search(r'"network_key"\s*:\s*(\d+)', txt)
    if not m:
        sys.exit(f"network_key not found in {CFG}")
    return struct.pack("<Q", int(m.group(1)))            # 8 LE bytes, as the firmware keys mbedTLS


def coin_body(mac=FAKE_MAC):
    b = mac.encode()
    return b + b"\x00" * (18 - len(b))                   # char mac[18], null-terminated


def build(key, msgtype, body, ts_ms=None, bad_tag=False):
    if ts_ms is None:
        ts_ms = int(time.time() * 1000)
    msg = struct.pack("<H", msgtype) + struct.pack("<Q", ts_ms) + body
    tag = bytes(random.randrange(256) for _ in range(16)) if bad_tag \
          else hmac.new(key, msg, hashlib.sha256).digest()[:16]
    return msg + tag


def main():
    args   = [a for a in sys.argv[1:] if not a.startswith("--")]
    ip_opt = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--ip")), None)
    dest   = ip_opt or "255.255.255.255"
    tests  = args or SAFE

    key = load_key()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    def send(pkt): s.sendto(pkt, (dest, PORT))

    print(f"Target {dest}:{PORT}  ·  watch the master serial monitor for the rejections\n")
    for t in tests:
        if t == "bad-packet":
            send(b"\x01\x02\x03\x04")
            print("→ bad-packet : 4 random bytes (under header+HMAC)")
            print('  expect log:  "UDP bad packet: 4 bytes from <you>"')
        elif t == "bad-hmac":
            send(build(key, COIN_INSERTED, coin_body(), bad_tag=True))
            print("→ bad-hmac   : well-formed coin packet, random 16-byte tag (an attacker w/o the key)")
            print('  expect log:  "rejected: bad HMAC"   (and NO coin credited)')
        elif t == "stale":
            send(build(key, COIN_INSERTED, coin_body(), ts_ms=int(time.time() * 1000) - 10000))
            print("→ stale      : VALID HMAC but timestamp 10s in the past (a replayed capture)")
            print('  expect log:  "rejected: stale by ~10000ms"   (rejected before any credit)')
        elif t == "valid-coin":
            send(build(key, COIN_INSERTED, coin_body()))
            print("→ valid-coin : fully valid coin insert  ⚠️  POSITIVE CONTROL — master credits a real coin")
            print('  expect:      master logs a coin from DE:AD:BE:EF:00:01 and starts a session')
        elif t == "replay":
            pkt = build(key, COIN_INSERTED, coin_body())     # one ts, sent twice
            send(pkt); time.sleep(0.2); send(pkt)
            print("→ replay     : same valid coin packet sent twice  ⚠️  credits ONE coin")
            print('  expect:      first credited, second dropped by the replay ring (same mac+ts)')
        else:
            print(f"→ {t}: unknown test (pick from {SAFE + SIDE_EFFECT})")
        time.sleep(0.4)

    print("\nDone. A correct master shows bad-packet/bad-hmac/stale all rejected with NO coin credited.")


if __name__ == "__main__":
    main()
