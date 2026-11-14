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

## D. Nov trip

11. **Ground the cables and switches — #1 priority.**
12. **Release the firmware:** E3 voucher sheet + coin sales to SiteDashboard; then watch `vendo_sale` fill.
13. **Install the OC300** (30,000 vouchers vs 10,000).
14. **Install the second Starlink** before the ~Dec 5 throttle.
15. **Shaper "before" at Bakhaw:** iPhone Reels runs on Omada 2/1, rush hour and morning.
16. **Install the two new switches;** confirm the Starlink link holds 1000 with no lost pings, school APs on
    their own channels.
17. **Maridel AP:** new cable / cable end.
18. **Hardwire the school with cable instead of mesh;** measure runs first (100 m limit).
19. **Negra 2.4 GHz antennas** are pointed sideways toward the sea — fix.
20. **WiGLE signal walk + house test** for a real coverage map and wall/roof losses.
21. **Put the Voucher and PisoWifi SSIDs on their own VLANs, IPv4 only** — needed before the shaper goes in.
21a. **Network move:** ER706W, CustomerPortal hosted by the controller, DHCP option 114 (memory network_design.md).

## E. Firmware

22. **Find the controller automatically when its IP moves — important, test it.** DHCP option 138 or a subnet
    scan; stopgap is a DHCP reservation.
23. **Remove the voucher tab from VendoAdmin** — vouchers live in VoucherAdmin now, and the tab froze the page
    whenever the controller was down.
24. **Change WiFi in the field** without a cable (`memory/field_reprovisioning_design.md`).
26. **Read-only file viewer on VendoAdmin** (passwords masked, capped reads).
27. **Session-cache refactor** — rethink in the new design (↓ #29).
28. **Coin relay has no single owner** — rethink in the new design (↓ #29).
29. **Masterless "pick-your-box" vendo design** — concurrent payers, per-box reservation (replaces the
    single `COIN_SESSION_SLOT`), naming-convention discovery (`ellafi1…9.local`), stay on the current
    `extPortal/auth`+`extend` path (no vouchers — By-Usage can't be topped up). Gated by a real-phone
    reachability POC (client isolation is make-or-break). Full design + POC checklist:
    `memory/distributed_vendo_design.md`.

## F. SiteDashboard / docs

31. **README rewrite** so a newcomer can install, flash, configure and test EllaFi.

## G. Ideas — not this trip

32. **Home booster firmware that only forwards packets,** so each phone behind it keeps its own voucher.
