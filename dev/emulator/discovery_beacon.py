#!/usr/bin/env python3
"""discovery_beacon.py — announce an emulated Omada device to the lab controller.

Wire format (from dev/protocol-ecsp.md, extracted): a UDP datagram to controller:29810 =
    [4-byte BIG-ENDIAN length][ plaintext UTF-8 JSON {"header":{...}, "body":{}} ]
DISCOVERY is header.type=1; BaseDiscovery body is empty; discovery is NOT encrypted (RC4 is the
TCP manage channel only). Identity (spoofed EAP) lives in the header.

Lab only — talks to the throwaway Docker controller on localhost. Iterate the HEADER fields below
against `docker logs omada-controller` until the device shows Pending in the app.
"""
import argparse
import json
import os
import socket
import struct
import time

# --- spoofed device identity (per memory/omada_app_reference/field-map.md) ---
# header.device is the DeviceType ENUM ("ap"/"switch"/"gateway", validated by DeviceType.resolve()),
# NOT the model. The model (EAP225…) belongs in the later inform body, not discovery.
FAKE_MAC    = os.environ.get("EMU_MAC", "8C-86-DD-00-00-33")   # override per instance (2nd AP, mesh parent)
DEVICE_TYPE = "ap"                   # DeviceType.AP — must resolve or the msg is "invalid device message"
FAKE_MODEL  = os.environ.get("EMU_MODEL", "EAP225-Outdoor")   # old model, always in the registry (inform time)
EMU_NAME    = os.environ.get("EMU_NAME", "EllaFi-Piso")       # display name
EMU_FW      = os.environ.get("EMU_FW", "1.0.0 Build 20260101 Rel.00000")  # reported FIRMWARE version (drives the upgrade indicator)
FW_VERSION  = "2.3.0"                # header.version = ECSP PROTOCOL version (EAP_PROTO_VERSION2=V2).
                                     # NOT firmware — "5.0.0" parsed as v5 → HIGH_IN_COMPATIBLE.
OMADAC_ID   = "108eb4715331245bff3eff56e949f37f"   # THIS lab controller's omadacId (from its logs)


def build_discovery(ip: str) -> bytes:
    header = {
        "seq": 1,
        "version": FW_VERSION,
        "device": DEVICE_TYPE,
        "mac": FAKE_MAC,
        "type": 1,                    # DISCOVERY
        "timestamp": int(time.time()),
        "ip": ip,
    }
    # ApDiscoveryV2Body: deviceInfo (EapDiscoveryDeviceInfo) + deviceMisc (ApDeviceMiscV2 caps).
    # REAL ESP values eventually fill upTime/cpuUti/memUti; identity is the spoof.
    body = {
        "deviceInfo": {
            "name": EMU_NAME,
            "model": FAKE_MODEL,
            "modelVersion": "1.0",
            "firmwareVersion": EMU_FW,
            "hardwareVersion": "1.0",
            "upTime": "3600",           # String
            "cpuUti": 5,                # Integer
            "memUti": 37,               # Integer
            "wirelessLinked": False,    # Boolean
        },
        "deviceMisc": {                 # support_* flags: some Boolean, some Integer(0/1) — types matter
            "support_11ac": True,       # Boolean
            "support_lag": False,       # Boolean
            "supportMesh": 1,           # Integer
            "customizeRegion": 0,       # Integer
            "lanPortsNum": 1,           # Integer
            "support_channelLimit": False,  # Boolean
            "supportDfs": 0,            # Integer
            "supportRoaming": 1,        # Integer
            "countryCode": 1,           # Integer — REQUIRED on 6.2 (DiscoveryApplicationService.d NPEs if null)
        },
        # controllerId = md5("default") is the FACTORY sentinel: the controller's decision code treats
        # reportedControllerId==md5("default") as a factory device → NO_MANAGED = PENDING/adoptable.
        "controllerSetting": {"controllerId": "c21f969b5f03d33d43e04f8f136e7682", "destOmadacId": ""},
    }
    msg = json.dumps({"header": header, "body": body}, separators=(",", ":")).encode("utf-8")
    return struct.pack(">I", len(msg)) + msg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1", help="controller (default: localhost published port)")
    ap.add_argument("--port", type=int, default=29810)
    ap.add_argument("--ip", default="127.0.0.1", help="value for header.ip (the device's claimed IP)")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--keepalive", type=int, default=0,
                    help="resend every N seconds forever (device context times out ~40s without it)")
    args = ap.parse_args()

    pkt = build_discovery(args.ip)
    print(f"discovery packet: {len(pkt)} bytes (4-byte len + {len(pkt)-4} JSON)")
    print("  " + pkt[4:].decode())
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if args.keepalive:
        print(f"keepalive: resending every {args.keepalive}s — device stays visible. Ctrl-C to stop.")
        n = 0
        while True:
            s.sendto(pkt, (args.host, args.port))
            n += 1
            print(f"  beacon #{n} → {args.host}:{args.port}")
            time.sleep(args.keepalive)
    for i in range(args.repeat):
        s.sendto(pkt, (args.host, args.port))
        print(f"sent → {args.host}:{args.port}  (#{i+1})")
        if i + 1 < args.repeat:
            time.sleep(1)
    s.close()


if __name__ == "__main__":
    main()
