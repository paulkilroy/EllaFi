#!/usr/bin/env python3
"""
Deploy firmware and/or filesystem to EllaFi master via web OTA.

Auth: the device's admin gate is cookie-only (HTTP Basic no longer works). We do the challenge-response
login — GET /admin/challenge -> sha256(nonce:omada_password) -> POST /admin/login -> ellafi_admin cookie —
and the (IP-bound) session cookie then carries the OTA uploads. The session table is RAM, so it's cleared
by each reboot; we re-login after the FS flash before pushing firmware.

Default (no flags): firmware only — fast, no data loss, slaves auto-update.
Use --flash-fs to also re-flash the filesystem. NOTE: the FS-upload path PRESERVES /config.json (it backs
it up and restores it), so it does NOT push config changes — use the admin config editor (/admin/config) for that.

Usage:
    python scripts/deploy.py [--host 192.168.1.3] [--flash-fs] [--fs-only]
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import requests

BUILD_DIR = ".pio/build/esp32-s3-devkitc-1"
PIO = os.path.expanduser("~/.platformio/penv/bin/platformio")
CONFIG_PATH = "data/config.json"


def load_config():
    if not os.path.exists(CONFIG_PATH):
        return {}
    with open(CONFIG_PATH) as f:
        return json.load(f)


def login(session, base_url, password):
    """Challenge-response admin login; stores the ellafi_admin cookie on the session. Returns True on success."""
    try:
        nonce = session.get(f"{base_url}/admin/challenge", timeout=8).json()["nonce"]
        digest = hashlib.sha256(f"{nonce}:{password}".encode()).hexdigest()
        r = session.post(f"{base_url}/admin/login", json={"nonce": nonce, "digest": digest}, timeout=8)
        if r.ok and r.json().get("ok"):
            return True
        print(f"  login rejected: HTTP {r.status_code} {r.text.strip()[:120]}")
    except (requests.exceptions.RequestException, ValueError, KeyError) as e:
        print(f"  login error: {e}")
    return False


def upload(session, url, path, label, retries=3):
    size = os.path.getsize(path)
    for attempt in range(1, retries + 1):
        try:
            print(f"  Uploading {label} ({size:,} bytes)... ", end="", flush=True)
            with open(path, "rb") as f:
                r = session.post(url, data=f, headers={"Content-Type": "application/octet-stream"}, timeout=180)
            if r.ok:
                print("OK")                     # the device returns 200 ONLY after Update.end() commits
                return
            # A real HTTP error (e.g. 500 "Error processing upload", or 401 unauthorized) means the flash
            # FAILED — the device did NOT update. Never treat it as success.
            print(f"FAILED ({r.status_code}): {r.text.strip()[:120]}")
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            # A successful OTA returns 200 BEFORE the 500ms reboot timer, so it never lands here. A dropped
            # connection with no 200 means the upload aborted or the socket died mid-transfer: a failure.
            print("connection dropped (no 200 — upload did not complete)")
        if attempt < retries:
            print(f"  Retrying in 5s (attempt {attempt+1}/{retries})...")
            time.sleep(5)
        else:
            print("  All retries failed.")
            sys.exit(1)


def wait_for_host(base_url, timeout=120, settle=3):
    print("  Waiting for master to reboot...", end="", flush=True)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if requests.get(f"{base_url}/status", timeout=3).ok:   # /status is public — no auth needed
                print(" online", end="", flush=True)
                if settle:
                    print(f", settling {settle}s...", end="", flush=True)
                    time.sleep(settle)
                print()
                return
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            pass
        print(".", end="", flush=True)
        time.sleep(2)
    print(" timed out — is master on the network?")
    sys.exit(1)


def step(n, total, msg):
    print(f"\n[{n}/{total}] {msg}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host",     default=None, help="Master hostname or IP (default: ellafi.local)")
    parser.add_argument("--flash-fs", action="store_true", help="Also re-flash filesystem (preserves config.json)")
    parser.add_argument("--fs-only",  action="store_true", help="Flash filesystem only, skip firmware")
    args = parser.parse_args()

    do_fw = not args.fs_only
    do_fs = args.flash_fs or args.fs_only

    config = load_config()
    host = args.host or config.get("deploy_host", "ellafi.local")
    password = config.get("omada_password", "")     # == the device's ADMIN_PASSWORD (cfg("omada_password"))
    base_url = f"http://{host}"

    fw_path = os.path.join(BUILD_DIR, "firmware.bin")
    fs_path = os.path.join(BUILD_DIR, "littlefs.bin")

    print("Building...")
    if do_fw:
        result = subprocess.run([PIO, "run", "-e", "esp32-s3-devkitc-1"], capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout[-2000:])
            print(result.stderr[-1000:])
            print("Firmware build failed — aborting")
            sys.exit(1)
    if do_fs:
        result = subprocess.run([PIO, "run", "-e", "esp32-s3-devkitc-1", "-t", "buildfs"], capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout[-2000:])
            print(result.stderr[-1000:])
            print("Filesystem build failed — aborting")
            sys.exit(1)
    print("Build OK")

    total = (1 if do_fs else 0) + (1 if do_fs and do_fw else 0) + (1 if do_fw else 0) + 1
    n = 0

    print(f"\nDeploying to {base_url}")
    session = requests.Session()
    if not login(session, base_url, password):
        print("Admin login failed — check omada_password in config.json and that the host is reachable. Aborting.")
        sys.exit(1)

    if do_fs:
        n += 1
        step(n, total, "Uploading filesystem (config.json PRESERVED — not changed)")
        upload(session, f"{base_url}/update-fs", fs_path, "littlefs.bin")

        if do_fw:
            n += 1
            step(n, total, "Waiting for master to come back up")
            wait_for_host(base_url)
            if not login(session, base_url, password):   # the reboot cleared the RAM session table
                print("Re-login after FS reboot failed — aborting before firmware.")
                sys.exit(1)

    if do_fw:
        n += 1
        step(n, total, "Uploading firmware")
        print(  "  Master will: flash new firmware → save fw_size to NVS → reboot")
        upload(session, f"{base_url}/update", fw_path, "firmware.bin")

    n += 1
    step(n, total, "Done")
    if do_fw:
        print("  Master rebooting. Slaves will auto-update on next heartbeat (~15s).")
    if do_fs:
        print("  Filesystem re-flashed (config.json preserved).")


if __name__ == "__main__":
    main()
