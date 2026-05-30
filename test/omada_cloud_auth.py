#!/usr/bin/env python3
"""
Cloud SSO auth flow for accessing the Daram on-premises Omada controller remotely
via TP-Link's controller-connector cloud relay.

The flow:
  1. GET login page → extract session_code
  2. POST id.tplinkcloud.com/api/v1/login → UID_SSO_SID + SESSION cookies + redirectParams.state
  3. GET oauth/authorize (with cookies) → 302 redirect containing code=
  4. POST login-with-uid-code → TPEC_SID + csrfToken (+ possibly omadac_id in redirectUrl)
  5. Use Omada session against controller-connector endpoint with remote headers

URL pattern (confirmed from browser DevTools):
  https://use1-api-omada-controller-connector.tplinkcloud.com
    /omadac/{omadac_id}/{controller_id}/api/v2/...
  where omadac_id is a 40-char hex string (SHA1-sized cloud session token).

Usage:
  python3 omada_cloud_auth.py            # list hotspot clients
  python3 omada_cloud_auth.py --all      # list all WiFi clients (admin)
  python3 omada_cloud_auth.py --debug    # verbose HTTP logging
"""

import json
import re
import sys
import os
import time
import argparse
import requests
from urllib.parse import urlparse, parse_qs, urlencode, unquote
from urllib3.exceptions import InsecureRequestWarning

requests.packages.urllib3.disable_warnings(InsecureRequestWarning)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "../data/config.json")

ID_HOST       = "https://h2api-id.tplinkcloud.com"
ID_HOST_OAUTH = "https://use1-api-id.tplinkcloud.com"
CLOUD_MANAGER = "https://use1-api-omada-cloud-manager.tplinkcloud.com"
CONNECTOR     = "https://use1-api-omada-controller-connector.tplinkcloud.com"

CLOUD_ACCESS  = "https://use1-api-omada-cloud-access.tplinkcloud.com"

REMOTE_HEADERS = {
    "omada-remote-timeout": "60",
    "omada-request-source": "web-remote",
}

def load_config():
    with open(CONFIG_PATH) as f:
        return json.load(f)

# ---------- Step 1: session_code ----------

BROWSER_HEADERS = {
    "User-Agent": ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) "
                   "Chrome/124.0.0.0 Safari/537.36"),
    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
    "Accept-Language": "en-US,en;q=0.9",
    "Accept-Encoding": "gzip, deflate, br",
    "Connection": "keep-alive",
}

def get_session_code(session, debug):
    """Load the TP-Link ID login page and extract the embedded session_code."""
    url = "https://id.tplinkcloud.com/"
    if debug:
        print(f"GET {url}")
    r = session.get(url, headers=BROWSER_HEADERS)
    if debug:
        print(f"  → {r.status_code}  final URL: {r.url}")
        print(f"  Cookies set: {[(c.name, c.domain) for c in session.cookies]}")

    # session_code appears inline in JSON or as a query param on the page/redirect
    for pattern in [
        r'["\']sessionCode["\']\s*:\s*["\']([A-F0-9]{32})["\']',
        r'sessionCode[=:\s]+["\']?([A-F0-9]{32})',
        r'session_code[=:\s]+["\']?([A-F0-9]{32})',
    ]:
        m = re.search(pattern, r.text, re.IGNORECASE)
        if m:
            if debug:
                print(f"  Found session_code in page body")
            return m.group(1)

    # Also check redirect URL query params
    parsed = urlparse(r.url)
    qs = parse_qs(parsed.query)
    for key in ("sessionCode", "session_code"):
        if key in qs:
            return qs[key][0]

    if debug:
        # print a snippet of the page to help diagnose
        print(f"  Page snippet (first 800 chars):\n{r.text[:800]}")
    return None

# ---------- Step 2: TP-Link ID login ----------

def tplink_login(session, config, session_code, debug):
    url = f"{ID_HOST}/api/v1/login"
    body = {
        "email":             config["omada_username"],
        "password":          config["omada_password"],
        "terminalUUID":      "d3084c5a-60b8-4fa3-b166-f9b7c3e13f58",
        "privatePolicyChecked": False,
    }
    headers = {
        "session_code": session_code,
        "Content-Type": "application/json",
        "Origin":  "https://id.tplinkcloud.com",
        "Referer": "https://id.tplinkcloud.com/",
    }
    print(f"POST {url}")
    r = session.post(url, json=body, headers=headers)
    data = r.json()
    if debug:
        print(f"  Login response: {json.dumps(data, indent=2)}")

    if not data.get("success") and data.get("errorCode", 0) != 0:
        print(f"  ERROR: login failed — {data}")
        sys.exit(1)

    result = data.get("result", {})
    # redirectParams is a URL-encoded string like "responseType=code&clientId=...&state=4ad31c..."
    rp_raw = result.get("redirectParams", "")
    rp = parse_qs(rp_raw)
    state          = rp.get("state",       [""])[0]
    client_id      = rp.get("clientId",    ["omada-cloud-portal"])[0]
    redirect_uri   = unquote(rp.get("redirectUri", ["https://omada.tplinkcloud.com/#/loginRedirect"])[0])
    scope          = rp.get("scope",       ["openid"])[0]
    service_url    = result.get("serviceUrl", "https://use1-api-id.tplinkcloud.com")

    print(f"  state={state}  client_id={client_id}")
    if debug:
        print(f"  redirectParams raw: {rp_raw}")
        print(f"  redirect_uri={redirect_uri} service_url={service_url}")
    print("  Cookie jar after login:")
    for cookie in session.cookies:
        print(f"    {cookie.name}={cookie.value[:20]}...  domain={cookie.domain}  path={cookie.path}")

    return state, client_id, redirect_uri, scope, service_url, rp_raw

# ---------- Step 3: OAuth2 authorize → code ----------

def get_oauth_code(session, state, client_id, redirect_uri, scope, service_url, rp_raw, debug):
    """
    With UID_SSO_SID + SESSION cookies set, the authorize endpoint redirects to
    redirect_uri with code= in the URL (may be in query params or before the # fragment).
    We capture the 302 Location header without following it to extract the code.
    """
    # JS uses /oauth/authorize (NOT /oauth2/authorize) with camelCase params appended
    # directly from the raw redirectParams string: location.href = serviceUrl + "/oauth/authorize?" + redirectParams
    # We reconstruct the same query string using the raw value from the login response.
    authorize_url = f"{service_url}/oauth/authorize"
    params = {
        "responseType": "code",
        "clientId":     client_id,
        "redirectUri":  redirect_uri,
        "scope":        scope,
        "state":        state,
    }
    # requests only sends cookies for the exact domain that set them.
    cookie_dict = {c.name: c.value for c in session.cookies}
    headers = {
        **BROWSER_HEADERS,
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Origin":  "https://id.tplinkcloud.com",
        "Referer": "https://id.tplinkcloud.com/",
        "Sec-Fetch-Mode": "navigate",
        "Sec-Fetch-Site": "same-site",
        "Sec-Fetch-Dest": "document",
    }

    # Use raw redirectParams appended verbatim — matches what the browser does
    full_url = f"{authorize_url}?{rp_raw}"
    print(f"GET {full_url}  (no-redirect, browser headers)")
    if debug:
        print(f"  cookies forwarded: {list(cookie_dict.keys())}")

    r = session.get(full_url, cookies=cookie_dict, headers=headers, allow_redirects=False)
    print(f"  → {r.status_code}")
    if debug:
        for k, v in r.headers.items():
            print(f"  {k}: {v}")

    if r.status_code in (301, 302, 303, 307, 308):
        location = r.headers.get("Location", "")
        print(f"  Location: {location}")

        # Code may be in query params or in the hash fragment:
        #   ?code=xxx#/loginRedirect          → query string before hash
        #   #/loginRedirect?code=xxx&state=y  → query string inside hash path
        parts = location.split("#", 1)
        pre_hash  = parts[0]
        post_hash = parts[1] if len(parts) > 1 else ""

        # Check pre-hash query string
        qs = parse_qs(urlparse(pre_hash).query)
        if "code" in qs:
            code = qs["code"][0]
            print(f"  code={code}  (from pre-hash query string)")
            return code

        # Check query string inside the hash fragment  (#/path?code=xxx)
        if "?" in post_hash:
            frag_qs = parse_qs(post_hash.split("?", 1)[1])
            if "code" in frag_qs:
                code = frag_qs["code"][0]
                print(f"  code={code}  (from hash fragment query string)")
                return code

        print(f"  (no code= found in Location — full location shown above)")
    elif r.status_code == 403:
        # Try following all redirects to diagnose — the hash fragment may be in the final URL
        print(f"  Retrying with allow_redirects=True to see where we land...")
        r2 = session.get(full_url, cookies=cookie_dict, headers=headers, allow_redirects=True)
        print(f"  Final URL: {r2.url}")
        print(f"  Status: {r2.status_code}")
        # The code won't be in r2.url (fragment is stripped by HTTP), but note any clues
        if debug:
            print(f"  Body (first 400): {r2.text[:400]}")
    else:
        print(f"  (expected 302, got {r.status_code})")
        if debug:
            print(f"  Body: {r.text[:600]}")

    return None

# ---------- Step 4: exchange code for Omada session ----------

def login_with_uid_code(session, code, state, debug):
    url = f"{CLOUD_MANAGER}/api/v1/central/account/login-with-uid-code"
    body = {
        "code":          code,
        "state":         state,
        "uidServiceUrl": "https://use1-api-id.tplinkcloud.com",
        "canary":        True,
    }
    print(f"POST {url}")
    r = session.post(url, json=body)
    data = r.json()
    if debug:
        print(f"  Response: {json.dumps(data, indent=2)}")

    if data.get("errorCode", -1) != 0:
        print(f"  ERROR: {data}")
        sys.exit(1)

    result = data.get("result", {})
    csrf_token       = result.get("csrfToken", "")
    controller_token = result.get("token", "")
    redirect_url     = result.get("redirectUrl", "")

    # Always show result keys — helps diagnose what fields the cloud manager returns
    print(f"  result keys: {list(result.keys())}")
    if redirect_url:
        print(f"  redirectUrl: {redirect_url}")
        # The SPA is loaded as e.g. .../index.html?token=xxx&... — extract if present
        parsed_redir = urlparse(redirect_url)
        redir_qs = parse_qs(parsed_redir.query)
        if "token" in redir_qs and not controller_token:
            controller_token = redir_qs["token"][0]

    # Persist csrfToken as a cookie so it's sent with subsequent requests
    if csrf_token:
        session.cookies.set("csrfToken", csrf_token, domain=".tplinkcloud.com", path="/")

    print(f"  csrfToken: {csrf_token}")
    if controller_token:
        print(f"  controller_token: {controller_token}")
    print(f"  cookies: {list(session.cookies.keys())}")
    return csrf_token, controller_token

# ---------- Step 5: query controller-connector ----------
#
# Confirmed browser URL pattern (from DevTools):
#   CONNECTOR/omadac/{omadac_id}/{controller_id}/api/v2/...
# where omadac_id is a 40-char hex cloud session token (not the controller_id).

def _remote_url(connector_url, omadac_id, controller_id, path):
    return f"{connector_url}/omadac/{omadac_id}/{controller_id}/{path}"

def _remote_headers(csrf_token=""):
    h = {**REMOTE_HEADERS,
         "Origin":  "https://use1-omada-cloud.tplinkcloud.com",
         "Referer": "https://use1-omada-cloud.tplinkcloud.com/",
         "X-Requested-With": "XMLHttpRequest"}
    if csrf_token:
        h["csrf-token"] = csrf_token
    return h

def get_organization_device_id(session, controller_id, csrf_token, debug):
    """
    Fetch the cloud-access organizations list and find the entry matching
    controller_id (== omadacId field). Returns (device_id, connector_url).

    The deviceId is the 40-char hex hardware registration ID used in the
    connector URL path: {connectorUrl}/omadac/{deviceId}/{omadacId}/api/v2/...
    """
    url = (f"{CLOUD_ACCESS}/api/v1/cloudaccess/organizations"
           f"?currentPage=1&currentPageSize=10&searchKey=&filters.deviceType=0")
    headers = {
        "csrf-token": csrf_token,
        "Origin":  "https://use1-omada-cloud.tplinkcloud.com",
        "Referer": "https://use1-omada-cloud.tplinkcloud.com/",
        "X-Requested-With": "XMLHttpRequest",
        "request-hash": "1",
    }
    print(f"GET {url}")
    r = session.get(url, headers=headers)
    data = r.json()
    if debug:
        print(f"  Response: {json.dumps(data, indent=2)}")
    if data.get("errorCode") != 0:
        print(f"  ERROR: {data.get('msg')} (errorCode={data.get('errorCode')})")
        return None, None
    for entry in data.get("result", {}).get("data", []):
        if entry.get("omadacId") == controller_id:
            device_id     = entry["deviceId"]
            connector_url = entry.get("connectorUrl", CONNECTOR)
            print(f"  Found: deviceId={device_id}  connectorUrl={connector_url}")
            return device_id, connector_url
    print(f"  ERROR: no organization entry with omadacId={controller_id}")
    return None, None

def get_user_sites_remote(session, connector_url, omadac_id, controller_id, csrf_token, debug):
    """Fetch sites list and return the first site_id. Returns None on failure."""
    url = _remote_url(connector_url, omadac_id, controller_id,
                      "api/v2/user/sites?currentPage=1&currentPageSize=100")
    print(f"GET {url}")
    r = session.get(url, headers=_remote_headers(csrf_token))
    data = r.json()
    if debug:
        print(f"  Response: {json.dumps(data, indent=2)}")
    if data.get("errorCode") != 0:
        print(f"  ERROR: {data.get('msg')} (errorCode={data.get('errorCode')})")
        return None
    sites = data.get("result", {}).get("data", [])
    if not sites:
        print(f"  ERROR: no sites returned")
        return None
    site_id = sites[0]["id"]
    print(f"  site_id={site_id}  name={sites[0].get('name', '?')}")
    return site_id

def get_hotspot_clients_remote(session, connector_url, omadac_id, controller_id, site_id, csrf_token, debug,
                               active_only=True):
    """
    List hotspot sessions, paginating until exhausted (or, when active_only=True,
    until a page contains no sessions with end > now_ms).
    Results are sorted end desc so stopping early on expired sessions is safe.
    """
    now_ms   = int(time.time() * 1000)
    all_data = []
    page     = 1
    base     = f"api/v2/hotspot/sites/{site_id}/clients?sorts.end=desc&currentPageSize=100"
    while True:
        url = _remote_url(connector_url, omadac_id, controller_id, f"{base}&currentPage={page}")
        print(f"GET {url}")
        r    = session.get(url, headers=_remote_headers(csrf_token))
        data = r.json()
        if debug:
            print(f"  Response: {json.dumps(data, indent=2)}")
        if data.get("errorCode") != 0:
            print(f"  ERROR: {data.get('msg')} (errorCode={data.get('errorCode')})")
            break
        result     = data.get("result", {})
        page_data  = result.get("data", [])
        total_rows = result.get("totalRows", 0)
        if not page_data:
            break
        all_data.extend(page_data)
        print(f"  page {page}: got {len(page_data)} (total so far: {len(all_data)} / {total_rows})")
        # If active_only, stop once every record on this page is expired
        if active_only and all(c.get("end", 0) <= now_ms for c in page_data):
            break
        if len(all_data) >= total_rows:
            break
        page += 1
    if active_only:
        all_data = [c for c in all_data if c.get("end", 0) > now_ms]
    return all_data

def get_all_clients_remote(session, connector_url, omadac_id, controller_id, site_id, csrf_token, debug):
    """List all connected WiFi clients."""
    url = _remote_url(connector_url, omadac_id, controller_id,
                      f"api/v2/sites/{site_id}/clients?page=1&pageSize=1000&filters.active=true")
    print(f"GET {url}")
    r = session.get(url, headers=_remote_headers(csrf_token))
    data = r.json()
    if debug:
        print(f"  Response: {json.dumps(data, indent=2)}")
    if data.get("errorCode") != 0:
        print(f"  ERROR: {data.get('msg')} (errorCode={data.get('errorCode')})")
        return []
    return data.get("result", {}).get("data", [])

# ---------- main ----------

def main():
    parser = argparse.ArgumentParser(description="Remote access to Daram controller via TP-Link cloud")
    parser.add_argument("--all",   action="store_true", help="also list all WiFi clients (admin login)")
    parser.add_argument("--debug", action="store_true", help="verbose HTTP responses")
    args = parser.parse_args()

    config = load_config()
    controller_id = config["omada_controller_id"]
    print(f"Controller ID : {controller_id}")
    print()

    s = requests.Session()

    # 1. session_code
    print("--- Step 1: session_code ---")
    session_code = get_session_code(s, args.debug)
    if session_code:
        print(f"  session_code={session_code}")
    else:
        session_code = "EA7C2AE2D0A045ACAC76A33F929ED1EA"
        print(f"  (not found in page, using fallback) session_code={session_code}")
    print()

    # 2. TP-Link ID login
    print("--- Step 2: TP-Link ID login ---")
    state, client_id, redirect_uri, scope, service_url, rp_raw = tplink_login(s, config, session_code, args.debug)
    print()

    # 3. OAuth2 code
    print("--- Step 3: OAuth2 authorize ---")
    code = get_oauth_code(s, state, client_id, redirect_uri, scope, service_url, rp_raw, args.debug)
    if not code:
        print("  BLOCKED: could not retrieve OAuth2 code.")
        sys.exit(1)
    print()

    # 4. Exchange code for cloud session (TPEC_SID + csrfToken)
    print("--- Step 4: login-with-uid-code ---")
    csrf_token, _ = login_with_uid_code(s, code, state, args.debug)
    print()

    # 5. Fetch controller deviceId + connectorUrl from cloud-access organizations
    print("--- Step 5: get organizations (deviceId + connectorUrl) ---")
    device_id, connector_url = get_organization_device_id(s, controller_id, csrf_token, args.debug)
    if not device_id:
        print("  Failed to get deviceId — cannot call connector.")
        sys.exit(1)
    print()

    # 6. Fetch site_id from controller
    print("--- Step 6: get site ID ---")
    site_id = get_user_sites_remote(s, connector_url, device_id, controller_id, csrf_token, args.debug)
    if not site_id:
        print("  Failed to get site_id.")
        sys.exit(1)
    print()

    # 7. Hotspot clients — TPEC_SID + deviceId is the implicit auth, no Omada login needed
    print("--- Step 7: hotspot clients ---")
    clients = get_hotspot_clients_remote(
        s, connector_url, device_id, controller_id, site_id, csrf_token, args.debug)

    now_ms = int(time.time() * 1000)
    if not clients:
        print("  No active hotspot clients.")
    else:
        print(f"  {len(clients)} active hotspot client(s):")
        for c in clients:
            remaining = max(0, round((c.get("end", 0) - now_ms) / 1000))
            print(f"  - {c.get('mac')}  id={c.get('id','?')}  remaining={remaining}s")

    if args.all:
        print()
        print("--- All WiFi clients ---")
        all_clients = get_all_clients_remote(
            s, connector_url, device_id, controller_id, site_id, csrf_token, args.debug)
        if not all_clients:
            print("  No clients.")
        else:
            print(f"  {len(all_clients)} client(s):")
            for c in all_clients:
                print(f"  - {c.get('mac')}  ip={c.get('ip','?')}  ssid={c.get('ssid','?')}")


if __name__ == "__main__":
    main()
