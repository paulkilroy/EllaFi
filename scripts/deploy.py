#!/usr/bin/env python3
"""
Deploy firmware and/or filesystem to EllaFi master via web OTA.

Default (no flags): firmware only — fast, no data loss, slaves auto-update.
Use --flash-fs to also update the filesystem (only needed when config.json changes).

Usage:
    python scripts/deploy.py [--host ellafi.local] [--flash-fs] [--fs-only]
"""

import argparse
import json
import os
import subprocess
import sys
import time
import requests
from requests.auth import HTTPBasicAuth

BUILD_DIR = ".pio/build/esp32-s3-devkitc-1"
PIO = os.path.expanduser("~/.platformio/penv/bin/platformio")
CONFIG_PATH = "data/config.json"


def load_config():
    if not os.path.exists(CONFIG_PATH):
        return {}
    with open(CONFIG_PATH) as f:
        return json.load(f)


def upload(url, path, label, auth, retries=3):
    size = os.path.getsize(path)
    for attempt in range(1, retries + 1):
        try:
            print(f"  Uploading {label} ({size:,} bytes)... ", end="", flush=True)
            with open(path, "rb") as f:
                r = requests.post(url, data=f, auth=auth,
                                  headers={"Content-Type": "application/octet-stream"},
                                  timeout=120)
            if r.ok:
                print("OK")
                return
            # A real HTTP error (e.g. 500 "Error processing upload") means the flash FAILED.
            # Never treat this as success — the device did NOT update.
            print(f"FAILED ({r.status_code}): {r.text.strip()}")
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            # The device sends 200 {"ok":true} BEFORE it reboots (500ms timer), so a successful
            # OTA returns a 200 above — it never lands here. A dropped connection with no response
            # means the upload aborted (ESP_FAIL) or the socket died mid-transfer: that is a failure,
            # so retry rather than silently reporting "rebooting".
            print("connection dropped (no 200 — upload did not complete)")
        if attempt < retries:
            print(f"  Retrying in 5s (attempt {attempt+1}/{retries})...")
            time.sleep(5)
        else:
            print("  All retries failed.")
            sys.exit(1)


def wait_for_host(base_url, auth, timeout=120, settle=3):
    print(f"  Waiting for master to reboot...", end="", flush=True)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = requests.get(f"{base_url}/status", auth=auth, timeout=3)
            if r.ok:
                print(f" online", end="", flush=True)
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
    parser.add_argument("--flash-fs", action="store_true", help="Also flash filesystem (config.json changes only)")
    parser.add_argument("--fs-only",  action="store_true", help="Flash filesystem only, skip firmware")
    args = parser.parse_args()

    do_fw = not args.fs_only
    do_fs = args.flash_fs or args.fs_only

    config = load_config()
    host = args.host or config.get("deploy_host", "ellafi.local")
    username = config.get("omada_username", "")
    password = config.get("omada_password", "")
    auth = HTTPBasicAuth(username, password) if username else None
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

    if do_fs:
        n += 1
        step(n, total, "Uploading filesystem (config.json)")
        print(  "  Master will: back up runtime files → flash new FS → restore → reboot")
        upload(f"{base_url}/update-fs", fs_path, "littlefs.bin", auth)

        if do_fw:
            n += 1
            step(n, total, "Waiting for master to come back up")
            wait_for_host(base_url, auth)

    if do_fw:
        n += 1
        step(n, total, "Uploading firmware")
        print(  "  Master will: flash new firmware → save fw_size to NVS → reboot")
        upload(f"{base_url}/update", fw_path, "firmware.bin", auth)

    n += 1
    step(n, total, "Done")
    if do_fw:
        print("  Master rebooting. Slaves will auto-update on next heartbeat (~15s).")
    if do_fs:
        print("  Filesystem updated (config.json changes applied).")


if __name__ == "__main__":
    main()
