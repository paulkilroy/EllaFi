#pragma once

// Entered from setup() when WiFi can't be joined with the stored credentials. NEVER RETURNS.
// Shows solid red, retries the stored creds in the background (reboots on success), and on a
// sustained BOOT-button hold brings up the SoftAP captive portal to re-enter config.
void runWifiRecovery();
