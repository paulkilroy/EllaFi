#pragma once

// ── Coin sales → the usage tracker (Nov 2026) ─────────────────────────────────────────────────
// Voucher sales and the income statement moved to the tracker's Admin page (ellafi-usage-tracker /admin);
// it needs the vendo's coin sales too. The master POSTs /vendo_history.json lines newer than the last
// batch the tracker confirmed (NVS "salesync"/"acked"), 200 lines at a time — the first run uploads the
// whole year the box keeps. Signed with HMAC-SHA256 over the body keyed by the Omada password (the tracker
// holds the same one), so no new secret and nothing secret on the wire. The tracker upserts by
// (ts, node, coins, minutes), so a resend never double-counts.

// Call from loop() on every node; does work only on the master, at most every 10 min (every minute while
// catching up), never when WiFi/NTP are down or internal DRAM can't fit a TLS handshake.
void salesSyncTick();
