#!/bin/bash
# Build the upload bundle for Omada's "Import Customized Page"
# (controller UI: Settings → Authentication → Portal → ... → Portal Customization → Import).
# The zip must contain index.html at its ROOT (no wrapping folder), max 2 MB.
cd "$(dirname "$0")"
rm -f ellafi-poc.zip
zip -q ellafi-poc.zip index.html getPortalPageSetting.json
echo "ellafi-poc.zip: $(du -h ellafi-poc.zip | cut -f1) ($(unzip -l ellafi-poc.zip | tail -1 | awk '{print $2}') files)"
