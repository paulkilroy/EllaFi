#!/usr/bin/env python3
"""Stress repro: hammer the controller-refresh path concurrently, capture serial, auto-stop on corruption.

Requires a TEST_MODE build (omada.h: `#define TEST_MODE true`) — it hits GET /diag/hammer, which only
exists under TEST_MODE. Use it to re-verify the CREDS_MUTEX create-once fix (omada.cpp setupOmada): kill
the controller so logins fail, then hammer. A clean run for several minutes = the double-owner mutex
corruption is not reproducing.

Owns the device serial port directly (close any other serial monitor first — the port is single-owner),
hammers GET /diag/hammer over WiFi, logs EVERYTHING to a timestamped file, and stops automatically the
moment it sees heap corruption or a crash on the serial stream — capturing ~3s past the trigger so the
backtrace finishes printing, then prints a banner.

SETUP: kill the mock controller first (controller logins must fail — that's the crash condition), and
       close the VSCode serial monitor so this script can own the port.

USAGE (use the PlatformIO python — it bundles pyserial):
  ~/.platformio/penv/bin/python3 test/crash_hammer.py [host] [workers] [serial_port] [baud]
    host         default http://ellafi.local      (or http://10.0.0.234)
    workers      default 6
    serial_port  default /dev/cu.wchusbserial110
    baud         default 115200
"""
import sys, time, threading, json, re, glob
import urllib.request
try:
    import serial   # pyserial
except ImportError:
    sys.exit("pyserial missing — run with ~/.platformio/penv/bin/python3 (it bundles pyserial)")

def find_port():
    for pat in ("/dev/cu.wchusbserial*", "/dev/cu.usbserial*", "/dev/cu.SLAB*", "/dev/cu.usbmodem*"):
        m = sorted(glob.glob(pat))
        if m: return m[0]
    return "/dev/cu.wchusbserial110"

HOST    = sys.argv[1] if len(sys.argv) > 1 else "http://ellafi.local"
WORKERS = int(sys.argv[2]) if len(sys.argv) > 2 else 6
PORT    = sys.argv[3] if len(sys.argv) > 3 else find_port()
BAUD    = int(sys.argv[4]) if len(sys.argv) > 4 else 115200
URL     = HOST.rstrip("/") + "/diag/hammer"
LOGFILE = time.strftime("crash_hammer_%Y%m%d_%H%M%S.log")

# Serial lines that mean "stop and capture": heap integrity failed, or any crash/panic.
CORRUPT = re.compile(r"OWNER CORRUPT|DOUBLE-ENTRY|assert failed|Backtrace:|Guru Meditation|abort\(\)")

stop     = threading.Event()
detected = {"t": None, "line": None}
lock     = threading.Lock()
hits = errs = 0
last = {}
log  = open(LOGFILE, "w")

def logwrite(s):
    log.write(s + "\n"); log.flush()

def serial_reader():
    try:
        ser = serial.Serial(PORT, BAUD, timeout=1)
    except Exception as e:
        print(f"could not open {PORT}: {e}  (is the VSCode monitor still open?)")
        stop.set(); return
    while not stop.is_set():
        try:
            raw = ser.readline()
        except Exception:
            break
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").rstrip()
        logwrite(f"{time.strftime('%H:%M:%S')} | {line}")
        if CORRUPT.search(line) and detected["t"] is None:
            with lock:
                detected["t"] = time.time(); detected["line"] = line
            print(f"\n*** TRIGGER on serial: {line!r}  — capturing 3s then stopping ***\n")
    try: ser.close()
    except Exception: pass

def hammer():
    global hits, errs, last
    while not stop.is_set():
        try:
            with urllib.request.urlopen(URL, timeout=20) as r:
                d = json.loads(r.read().decode())
            with lock:
                hits += 1; last = d
        except Exception:
            with lock:
                errs += 1

threading.Thread(target=serial_reader, daemon=True).start()
time.sleep(0.5)
if stop.is_set():
    sys.exit(1)
for _ in range(WORKERS):
    threading.Thread(target=hammer, daemon=True).start()

print(f"serial+trend → {LOGFILE}")
print(f"hammering {URL} with {WORKERS} workers — Ctrl-C to stop\n")
logwrite(f"# host={HOST} workers={WORKERS} port={PORT}")
mins = {}
prev_free = None
try:
    while not stop.is_set():
        time.sleep(2)
        if detected["t"] and time.time() - detected["t"] > 3:   # let the backtrace finish, then stop
            break
        with lock:
            h, e, d = hits, errs, dict(last)
        for k in ("free", "internal", "httpd_stack"):
            v = d.get(k)
            if v is not None:
                mins[k] = min(mins.get(k, v), v)
        free = d.get("free")
        flag = "   <<< heap jumped — REBOOT?" if (free and prev_free and free > prev_free + 30000) else ""
        if free: prev_free = free
        msg = (f"hits={h} errs={e} | free={free} internal={d.get('internal')} "
               f"httpd_stack={d.get('httpd_stack')} | min free={mins.get('free')} "
               f"internal={mins.get('internal')} stack={mins.get('httpd_stack')}{flag}")
        print(msg); logwrite("# " + msg)
except KeyboardInterrupt:
    pass
finally:
    stop.set()
    time.sleep(0.4)
    print(f"\nstopped. full log: {LOGFILE}")
    if detected["line"]:
        print(f"TRIGGER: {detected['line']!r}")
    log.close()
