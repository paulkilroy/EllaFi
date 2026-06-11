# Hardware Notes

## PCBway assembly — ESP32 socket

Note to provide PCBway when ordering assembled boards:

> **Assembly notes — ESP32 socket**
> - **Solder the female header sockets** (through-hole) for the ESP32 footprint to the board.
> - **Do NOT populate the ESP32 module** — marked **DNP** in the BOM; installed by hand after delivery.
> - Populate all other components per the BOM.

- ESP32 module is **socketed, not soldered** — user inserts it themselves.
- In the BOM: ESP32 module = **DNP**; the female header sockets = **include/populate** (call them out
  separately so PCBway doesn't skip the sockets along with the DNP module).
- Board is the ESP32-S3 (DevKitC-1 class) per firmware target.

## Misc

- ESP32-S3 does **not** need BOOT held during serial upload (auto-reset).
