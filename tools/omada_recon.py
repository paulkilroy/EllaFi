#!/usr/bin/env python3
"""omada_recon.py — extract the device-plane wire truth from an Omada SW controller's jars.

Drives the Week-1 go/no-go: is the management-channel AES key a FLAT CONSTANT (UniFi-style,
weeks to an emulator) or PER-DEVICE (reassess scope)? Also dumps protobuf schema + the Model
enum so the emulator fills exactly the right fields.

The OC220 is a closed appliance (no shell, no jars) — run this against the LAB SW controller only:
    docker cp omada-controller:/opt/tplink/EAPController/lib ./_jars
    tools/omada_recon.py ./_jars              # hunt
    tools/omada_recon.py ./_jars --jadx       # + decompile the crypto/proto classes

Findings are printed AND written to omada_recon_report.md (redact keys before committing — this
repo is public; see memory/api_captures/README.md).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

# What we're hunting, grouped so the report reads like an investigation.
CRYPTO = [r"Cipher\.getInstance", r"SecretKeySpec", r"IvParameterSpec", r"AES/[A-Z]+/[A-Za-z0-9]+",
          r"PBKDF2", r"\bMD5\b", r"SHA-?256", r"KeyGenerator", r"\bECB\b", r"\bCBC\b", r"\bGCM\b"]
KEY_HINTS = [r"getKey", r"deviceKey", r"managementKey", r"informKey", r"defaultKey",
             r"[\"']([A-Fa-f0-9]{16,64})[\"']"]           # inline hex key literals
PORTS = [r"\b2981[0-4]\b"]
PROTO = [r"com\.tplink\.[\w.]*proto[\w.]*", r"\.proto\b", r"FileDescriptorProto", r"DiscoveryV\d",
         r"InformV\d", r"[A-Za-z]+(?:Discover|Inform|Adopt|Heartbeat)[A-Za-z]*"]


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600)


def collect_jars(path):
    if os.path.isfile(path) and path.endswith(".jar"):
        return [path]
    return [os.path.join(r, f) for r, _, fs in os.walk(path) for f in fs if f.endswith(".jar")]


def relevant(jars):
    """The device-plane logic lives in a couple of jars, not all ~80. Rank by name + size."""
    scored = []
    for j in jars:
        name = os.path.basename(j).lower()
        s = 3 if any(k in name for k in ("eap", "device", "omada", "controller", "manage", "discovery")) else 0
        s += 1 if os.path.getsize(j) > 200_000 else 0
        scored.append((s, os.path.getsize(j), j))
    return [j for _, _, j in sorted(scored, reverse=True)]


def hunt(jars, out):
    """strings-level pass across class bytes — fast, finds the needles without decompiling."""
    tmp = tempfile.mkdtemp(prefix="omada_recon_")
    hits = {"crypto": {}, "keys": {}, "ports": {}, "proto": {}}
    try:
        for j in jars:
            try:
                with zipfile.ZipFile(j) as z:
                    blob = b""
                    for n in z.namelist():
                        if n.endswith((".class", ".proto", ".desc")):
                            blob += z.read(n)
                    text = blob.decode("latin-1", "ignore")
            except Exception as e:
                out(f"  ! {os.path.basename(j)}: {e}")
                continue
            for group, pats in (("crypto", CRYPTO), ("keys", KEY_HINTS), ("ports", PORTS), ("proto", PROTO)):
                for p in pats:
                    for m in re.findall(p, text):
                        key = m if isinstance(m, str) else m[0]
                        if len(key) < 3:
                            continue
                        hits[group].setdefault(key, set()).add(os.path.basename(j))
        return hits
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def jadx_dump(jars, out):
    """Decompile only the top-ranked jars and print the crypto + key-derivation methods verbatim —
    this is where the FLAT-vs-PER-DEVICE answer actually lives."""
    if not shutil.which("jadx"):
        out("  jadx not installed (brew install jadx) — skipping decompile")
        return
    outdir = tempfile.mkdtemp(prefix="omada_jadx_")
    for j in jars[:3]:
        sh(["jadx", "-d", os.path.join(outdir, os.path.basename(j)), "--no-res", j])
    src = [os.path.join(r, f) for r, _, fs in os.walk(outdir) for f in fs if f.endswith(".java")]
    out(f"  decompiled {len(src)} .java from {min(3, len(jars))} jars")
    KEYLINE = re.compile(r"(SecretKeySpec|Cipher\.getInstance|IvParameterSpec|"
                         r"[\"'][A-Fa-f0-9]{16,64}[\"']|getBytes\(\)|md5|MessageDigest)")
    for f in src:
        try:
            lines = open(f, encoding="utf-8", errors="ignore").read().splitlines()
        except Exception:
            continue
        for i, ln in enumerate(lines):
            if KEYLINE.search(ln):
                ctx = "\n      ".join(lines[max(0, i - 2):i + 3])
                out(f"  {os.path.relpath(f, outdir)}:{i+1}\n      {ctx}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("jars", help="dir of extracted controller jars (or a single .jar)")
    ap.add_argument("--jadx", action="store_true", help="also decompile top jars and dump crypto methods")
    args = ap.parse_args()

    lines = []
    def out(s=""):
        print(s)
        lines.append(s)

    jars = relevant(collect_jars(args.jars))
    if not jars:
        sys.exit(f"no jars under {args.jars}")
    out(f"# omada_recon — {len(jars)} jars, top candidates:")
    for j in jars[:6]:
        out(f"  - {os.path.basename(j)} ({os.path.getsize(j)//1024} KB)")
    out()

    hits = hunt(jars, out)
    for group, title in [("crypto", "CRYPTO primitives"), ("keys", "KEY material / derivation (← go/no-go)"),
                         ("ports", "device-plane PORTS"), ("proto", "PROTOBUF / message names")]:
        out(f"## {title}")
        if not hits[group]:
            out("  (none found)")
        for val, where in sorted(hits[group].items(), key=lambda kv: -len(kv[1]))[:25]:
            out(f"  {val:38s} ← {', '.join(sorted(where)[:3])}")
        out()

    if args.jadx:
        out("## Decompiled crypto sites (read these for FLAT vs PER-DEVICE)")
        jadx_dump(jars, out)

    report = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dev", "omada_recon_report.md")
    open(report, "w").write("\n".join(lines))
    print(f"\n→ {os.path.relpath(report)}  (REDACT any real keys before committing — public repo)")


if __name__ == "__main__":
    main()
