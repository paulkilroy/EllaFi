#!/usr/bin/env python3
"""Upload the portal bundle to Omada's Import Customized Page — replaces the manual controller-UI
upload. Endpoints from the 2026-08-15 HAR capture (memory/api_captures/portal-page-upload.md):

  1. POST  {cid}/api/v2/files/sites/{site}/portal/page   multipart data={md5,fileName}    (pre-check)
  2. POST  same, + file                                   -> {result:{id}}                (upload)
  3. PATCH {cid}/api/v2/sites/{site}/setting/portals/{portalId}  full config with
           importedPortalPage.id = uploaded id                                            (bind)

The PATCH body is the portal's own current config (fetched, then updated) so nothing else changes.

Usage:
  make_zip.sh && ./upload_portal.py --portal "EllaFi Test Portal"
  ./upload_portal.py --portal "EllaFi Test Portal" --zip ../omada-html/ellafi-poc.zip
"""
import argparse
import hashlib
import json
import os
import sys

import requests
import urllib3

urllib3.disable_warnings()

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG_PATH = os.path.join(REPO, "data", "config.json")
DEFAULT_ZIP = os.path.join(REPO, "omada-html", "ellafi-poc.zip")


def load_config():
    with open(CONFIG_PATH) as f:
        cfg = json.load(f)
    env = cfg.get("environments", {}).get(cfg.get("active_env", ""), {})
    merged = dict(cfg)
    merged.update(env)
    return merged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--portal", required=True, help='Portal name as shown in the controller UI, e.g. "EllaFi Test Portal"')
    ap.add_argument("--zip", default=DEFAULT_ZIP, help="Bundle to upload (default: omada-html/ellafi-poc.zip)")
    ap.add_argument("--site", default=None, help="Site name (default: the firmware's site from config)")
    args = ap.parse_args()

    cfg = load_config()
    base = cfg["omada_url"].rstrip("/")
    cid = cfg.get("omada_controller_id")

    s = requests.Session()
    # Controller id discovery, same as the firmware: /api/info carries it when not configured.
    if not cid:
        cid = s.get(f"{base}/api/info", verify=False, timeout=20).json()["result"]["omadacId"]

    r = s.post(f"{base}/{cid}/api/v2/login",
               json={"username": cfg["omada_username"], "password": cfg["omada_password"]},
               verify=False, timeout=20)
    j = r.json()
    if j.get("errorCode") != 0:
        sys.exit(f"login failed: {j.get('errorCode')} {j.get('msg')}")
    hdr = {"Csrf-Token": j["result"]["token"]}
    print(f"[1/5] logged in to {base}")

    # user/sites + currentPageSize — the firmware's exact working call (param names vary per endpoint)
    r = s.get(f"{base}/{cid}/api/v2/user/sites", headers=hdr,
              params={"currentPage": 1, "currentPageSize": 100}, verify=False, timeout=20)
    sites = r.json().get("result", {}).get("data", [])
    want = args.site or cfg.get("site_name")
    site = next((x for x in sites if not want or x.get("name") == want), None)
    if not site:
        sys.exit(f"site not found (have: {[x.get('name') for x in sites]})")
    site_id = site.get("siteId") or site.get("id")
    print(f"[2/5] site: {site.get('name')} ({site_id})")

    r = s.get(f"{base}/{cid}/api/v2/sites/{site_id}/setting/portals", headers=hdr, verify=False, timeout=20)
    portals = r.json().get("result", [])
    portal = next((p for p in portals if p.get("name") == args.portal), None)
    if not portal:
        sys.exit(f"portal '{args.portal}' not found (have: {[p.get('name') for p in portals]})")
    print(f"[3/5] portal: {portal['name']} ({portal['id']})")

    with open(args.zip, "rb") as f:
        blob = f.read()
    meta = json.dumps({"md5": hashlib.md5(blob).hexdigest(), "fileName": os.path.basename(args.zip)})
    upload_url = f"{base}/{cid}/api/v2/files/sites/{site_id}/portal/page"
    # Two-step, as the UI does it: md5 pre-check, then the actual bytes.
    r = s.post(upload_url, headers=hdr, files={"data": (None, meta)}, verify=False, timeout=30)
    if r.json().get("errorCode") != 0:
        sys.exit(f"pre-check failed: {r.text[:200]}")
    r = s.post(upload_url, headers=hdr,
               files={"data": (None, meta), "file": (os.path.basename(args.zip), blob, "application/zip")},
               verify=False, timeout=60)
    j = r.json()
    if j.get("errorCode") != 0:
        sys.exit(f"upload failed: {r.text[:200]}")
    file_id = j["result"]["id"]
    print(f"[4/5] uploaded {os.path.basename(args.zip)} ({len(blob):,} B) -> {file_id}")

    # Exactly the field set the controller UI PATCHes (from the HAR) — the full fetched object fails
    # validation on its half-populated portalCustomize, so send the trimmed shape instead.
    body = {k: portal[k] for k in ("id", "name", "enable", "httpsRedirectEnable", "landingPage",
                                   "landingUrlScheme", "portalUrlAuto", "portalUrl", "authType",
                                   "hotspot", "resource", "modifyTimeMs", "ssidList", "networkList")
            if k in portal}
    body["importedPortalPage"] = {"id": file_id}
    body["pageType"] = 2   # 2 = imported custom page
    r = s.patch(f"{base}/{cid}/api/v2/sites/{site_id}/setting/portals/{portal['id']}",
                headers=hdr, json=body, verify=False, timeout=30)
    j = r.json()
    if j.get("errorCode") != 0:
        sys.exit(f"bind failed: {r.text[:200]}")
    print(f"[5/5] portal '{portal['name']}' now serves the new page")


if __name__ == "__main__":
    main()
