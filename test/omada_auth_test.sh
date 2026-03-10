#!/bin/bash
# Test Omada extPortal/auth with a specific client
# Usage: ./omada_auth_test.sh [local|cloud] [time_ms]
#   local = prod local controller (self-signed, default)
#   cloud = prod cloud controller
#   time_ms = duration in milliseconds (default: 1 for deauth)

TARGET=${1:-local}
TIME_MS=${2:-1}

CONFIG=$(python3 -c "import json; print(json.dumps(json.load(open('$(dirname "$0")/../data/config.json'))))")

if [ "$TARGET" = "cloud" ]; then
  BASE_URL=$(echo "$CONFIG" | python3 -c "import sys,json; print(json.load(sys.stdin)['omada_url'])")
  CONTROLLER_ID=$(echo "$CONFIG" | python3 -c "import sys,json; print(json.load(sys.stdin)['omada_controller_id'])")
else
  BASE_URL=$(echo "$CONFIG" | python3 -c "import sys,json; print(json.load(sys.stdin)['omada_url_prod_local'])")
  CONTROLLER_ID=$(echo "$CONFIG" | python3 -c "import sys,json; print(json.load(sys.stdin)['omada_controller_id_prod_local'])")
fi

USERNAME=$(echo "$CONFIG" | python3 -c "import sys,json; print(json.load(sys.stdin)['omada_operator_username'])")
PASSWORD=$(echo "$CONFIG" | python3 -c "import sys,json; print(json.load(sys.stdin)['omada_operator_password'])")
COOKIES=/tmp/omada-cookies.txt
CLIENT_MAC="36-18-EA-86-B6-43"
SITE="ffff169597fae350d5d7eab4d80f7"
AP_MAC="8C-86-DD-E3-78-F0"
SSID="EllaFi"
RADIO_ID="1"

echo "=== Target: $TARGET ($BASE_URL) ==="
echo "=== Time: ${TIME_MS}ms ==="
echo ""

# Step 1: Login
echo "--- Step 1: Login ---"
LOGIN_RESPONSE=$(curl -k -s -c "$COOKIES" -X POST \
  "${BASE_URL}/${CONTROLLER_ID}/api/v2/hotspot/login" \
  -H "Content-Type: application/json" \
  -d "{\"name\":\"${USERNAME}\",\"password\":\"${PASSWORD}\"}")

echo "$LOGIN_RESPONSE" | python3 -m json.tool
echo ""

TOKEN=$(echo "$LOGIN_RESPONSE" | python3 -c "import sys,json; print(json.load(sys.stdin)['result']['token'])" 2>/dev/null)

if [ -z "$TOKEN" ]; then
  echo "ERROR: Failed to extract token from login response"
  exit 1
fi

echo "Token: $TOKEN"
echo ""

# Step 2: Auth
AUTH_URL="${BASE_URL}/${CONTROLLER_ID}/api/v2/hotspot/extPortal/auth"
AUTH_BODY="{
  \"clientMac\": \"${CLIENT_MAC}\",
  \"site\": \"${SITE}\",
  \"time\": \"${TIME_MS}\",
  \"authType\": 4,
  \"apMac\": \"${AP_MAC}\",
  \"ssidName\": \"${SSID}\",
  \"radioId\": \"${RADIO_ID}\"
}"

echo "--- Step 2: extPortal/auth ---"
echo "URL:  $AUTH_URL"
echo "Body: $AUTH_BODY"
echo ""

curl -k -s -b "$COOKIES" -X POST \
  "$AUTH_URL" \
  -H "Content-Type: application/json" \
  -H "Csrf-Token: ${TOKEN}" \
  -d "$AUTH_BODY" | python3 -m json.tool
