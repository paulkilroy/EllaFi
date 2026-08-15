#!/bin/bash
# Build the upload bundle for Omada's "Import Customized Page"
# (controller UI: Settings → Authentication → Portal → ... → Portal Customization → Import).
# The zip needs index.html at its ROOT (no wrapping folder), max 2 MB.
#
# Assets are staged fresh from web/ and data/ at build time — no duplicate copies in the repo.
# index.html here is the controller-hosted portal (ESP_HOST baked in — per-site!); diag.html is
# the POC diagnostics page, kept in the bundle and reachable at the portal URL's /diag.html.
set -e
cd "$(dirname "$0")"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

cp index.html diag.html getPortalPageSetting.json "$STAGE/"
cp ../web/EllaFi.webp ../web/favicon.ico "$STAGE/"
mkdir "$STAGE/assets"
cp ../web/assets/insertcoinbg.mp3 ../web/assets/coin-received.mp3 "$STAGE/assets/"
cp ../data/map.svg "$STAGE/" 2>/dev/null || echo "note: no data/map.svg — map card will stay hidden"
touch "$STAGE/theme.css"   # page links it; ESP serves 404→empty, controller needs the file to exist

rm -f ellafi-poc.zip
(cd "$STAGE" && zip -q -r ellafi-poc.zip .)
mv "$STAGE/ellafi-poc.zip" .
SIZE=$(du -h ellafi-poc.zip | cut -f1)
echo "ellafi-poc.zip: $SIZE ($(unzip -l ellafi-poc.zip | grep -c '^ ') entries, 2 MB cap)"
unzip -l ellafi-poc.zip
