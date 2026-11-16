# EllaFi — TODO

Everything still open, numbered (renumbered 2026-11-14). Finished items are deleted; detail lives in the files
named. ⛔ = needs Paul's OK.

## A. Nov trip — in install order

1. **Install the two new switches** (Grandma's in the house first, already grounded); confirm the Starlink link
   holds 1000 with no lost pings, school APs on their own channels.
2. **ER706W router + network move:** VLAN 1 equipment 10.1.0.0/24, VLAN 98 Voucher 10.98.0.0/22, VLAN 99
   PisoWifi 10.99.0.0/24 (each can grow inside its own 10.x block; no clash with home networks on Tailscale);
   2-hour DHCP leases; IPv4 only; Starlink #1 in bypass. Check nothing uses the Starlink router's own Wi-Fi
   first; DHCP reservation for the ESP's controller address.
3. **Second Starlink** on the ER706W's second internet port, bypass mode, 100% of traffic from Nov 28 (both dishes
   stay under 5 TB before Dec 10).
4. **Install the OC300 last** (30,000 vouchers vs 10,000) — controller backup and restore on its own day.
5. **DHCP option 114:** the router hands each phone the https address of the sign-in page, so the phone shows
   "Sign in to network" straight away instead of guessing. Needs a valid https certificate or phones ignore it.
6. **Put the MeLE shaper in at Bakhaw** between the router and the switch (needs the VLANs from #2). Trunk the whole
   path: the ER706W LAN port carries Voucher_VLAN + PisoWifi_VLAN tagged (Default untagged), the House Swich port to
   the MeLE on the "All" profile, and check the PtP bridges pass the tags to Grandma's and Negra's. Shaper config
   `--vlans 98 99 --subnets 10.98.0.0/22 10.99.0.0/24`.

Any time on the trip:

7. **Maridel AP:** new cable / cable end.
8. **Hardwire the school with cable instead of mesh;** measure runs first (100 m limit).
9. **Negra 2.4 GHz antennas** are pointed sideways toward the sea — fix.
10. **WiGLE signal walk + house test** for a real coverage map and wall/roof losses.

Later:

11. **Replace all outdoor Ethernet cables with shielded ones** (shielded plugs, shield grounded at the switch).

## B. PisoWifi shaper — MeLE Quieter DL (arrives Mon Nov 16)

12. **Set it up:** Ubuntu, bridge, shaper + boot service, power-on after power loss, temperature readings.
13. **Admin gateway:** Tailscale subnet router (`lab/shaper/tailscale_setup.sh`, Bakhaw's range) so Paul reaches
    the whole Bakhaw network (controller, APs, VendoAdmin, dish) from anywhere.
14. **Load test:** 100–300 Mbps through ~250 lanes overnight, watch CPU and temperature.
15. **Monitoring + Reliability timeline:** the box collects Starlink dish history, pings and per-phone bytes;
    SiteDashboard's Network tab shows it like the Aug "EllaFi Reliability" page (`tools/uptime_report.html`).

## C. Firmware

16. **Release the firmware (on the trip):** E3 voucher sheet + coin sales to SiteDashboard; then watch `vendo_sale` fill.
17. **Find the controller automatically when its IP moves — important, test it.** DHCP option 138 is how the
    router tells Omada devices the controller's address; the ESP can read the same option (or scan the subnet).
    Stopgap is a DHCP reservation.
18. **Read-only file viewer on VendoAdmin** (passwords masked, capped reads).
19. **Masterless "pick-your-box" vendo design** — concurrent payers, per-box reservation (replaces the
    single `COIN_SESSION_SLOT`), naming-convention discovery (`ellafi1…9.local`), stay on the current
    `extPortal/auth`+`extend` path (no vouchers — By-Usage can't be topped up). Gated by a real-phone
    reachability POC (client isolation is make-or-break). Includes moving the CustomerPortal page onto the
    controller so the ESP only counts coins and lets phones on. Full design + POC checklist:
    `memory/distributed_vendo_design.md`.

## D. Ideas — not this trip

20. **Home Booster:** custom firmware for an OpenWrt router that forwards packets for our WiFi, so each phone behind it keeps its own voucher.
