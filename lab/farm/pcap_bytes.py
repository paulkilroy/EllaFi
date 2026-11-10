#!/usr/bin/env python3
"""Sum IP total-length bytes going TO the given IPv4 addresses in a pcap (Ethernet, any snap length).
Runs on the shaper box (no tshark there). Prints: <bytes> <packets>
Usage: pcap_bytes.py <file.pcap> <ip> [ip...]"""
import socket, struct, sys
path, ips = sys.argv[1], {socket.inet_aton(x) for x in sys.argv[2:]}
f = open(path, "rb")
magic = f.read(24)[:4]
end = "<" if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1") else ">"
total = pkts = 0
while True:
    h = f.read(16)
    if len(h) < 16:
        break
    caplen = struct.unpack(end + "IIII", h)[2]
    d = f.read(caplen)
    if len(d) < 34 or d[12:14] != b"\x08\x00":
        continue
    if d[30:34] in ips:
        total += struct.unpack(">H", d[16:18])[0]; pkts += 1
print(total, pkts)
