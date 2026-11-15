# EllaFi — TODO

Everything still open, numbered (rewritten 2026-11-14). Finished items are deleted; detail lives in the files
named. ⛔ = needs Paul's OK.

## B. Shaper box — MeLE Quieter DL (arrives Nov 14)

5. **Set it up:** Ubuntu, bridge, shaper + boot service, power-on after power loss, temperature readings.
6. **Admin gateway:** Tailscale subnet router so Paul reaches the whole Bakhaw network (controller, APs,
   VendoAdmin, dish) from anywhere.
7. **Load test:** 100–300 Mbps through ~250 lanes overnight, watch CPU and temperature.
8. **Monitoring + Reliability timeline:** the box collects Starlink dish history, pings and per-phone bytes;
   SiteDashboard's Network tab shows it like the Aug "EllaFi Reliability" page (`tools/uptime_report.html`).

## D. Nov trip — in install order

16. **Install the two new switches** (Grandma's in the house first, already grounded); confirm the Starlink link
    holds 1000 with no lost pings, school APs on their own channels.
15. **Shaper "before" at Bakhaw:** iPhone Reels runs on Omada 2/1, rush hour and morning — before the shaper goes in.
21. **ER706W router + network move:** own address range (e.g. 10.20.x.x, so Tailscale doesn't clash with home
    networks); Voucher and PisoWifi SSIDs on their own VLANs, IPv4 only; router runs DHCP; Starlink #1 in bypass.
    Check nothing uses the Starlink router's own Wi-Fi first; DHCP reservation for the ESP's controller address.
21a. **DHCP option 114 (can go in early):** the router hands each phone the https address of the sign-in page,
    so the phone shows "Sign in to network" straight away instead of guessing. Needs a valid https certificate
    or phones ignore it.
14. **Second Starlink** on the ER706W's second internet port, bypass mode, 100% of traffic from Nov 28 (both dishes
    stay under 5 TB before Dec 10).
21b. **Put the MeLE shaper in** between the router and the switch; Tailscale with the new range.
13. **Install the OC300 last** (30,000 vouchers vs 10,000) — controller backup and restore on its own day.
12. **Release the firmware:** E3 voucher sheet + coin sales to SiteDashboard; then watch `vendo_sale` fill.

Any time on the trip:

17. **Maridel AP:** new cable / cable end.
18. **Hardwire the school with cable instead of mesh;** measure runs first (100 m limit).
19. **Negra 2.4 GHz antennas** are pointed sideways toward the sea — fix.
20. **WiGLE signal walk + house test** for a real coverage map and wall/roof losses.

Later:

11. **Replace all outdoor Ethernet cables with shielded ones** (shielded plugs, shield grounded at the switch).

## E. Firmware

22. **Find the controller automatically when its IP moves — important, test it.** DHCP option 138 is how the
    router tells Omada devices the controller's address; the ESP can read the same option (or scan the subnet).
    Stopgap is a DHCP reservation.
26. **Read-only file viewer on VendoAdmin** (passwords masked, capped reads).
29. **Masterless "pick-your-box" vendo design** — concurrent payers, per-box reservation (replaces the
    single `COIN_SESSION_SLOT`), naming-convention discovery (`ellafi1…9.local`), stay on the current
    `extPortal/auth`+`extend` path (no vouchers — By-Usage can't be topped up). Gated by a real-phone
    reachability POC (client isolation is make-or-break). Includes moving the CustomerPortal page onto the
    controller so the ESP only counts coins and lets phones on. Full design + POC checklist:
    `memory/distributed_vendo_design.md`.

## G. Ideas — not this trip

32. **Home booster firmware that only forwards packets,** so each phone behind it keeps its own voucher.
