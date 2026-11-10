#!/usr/bin/env python3
"""EllaFi shaper — every phone gets its own 2 Mbps down / 1 Mbps up lane that QUEUES instead of drops.

Why: every Omada limit drops whatever is over the cap (AP rate_limit.ko, see reports/Omada rate limiter
internals.md). Dropped data has already crossed Starlink and been counted, then the server sends it again:
~45% of capped-hour Starlink data never reaches a phone, and dropping makes TCP stall and video stutter.
A queue holds the extra for a few ms and sends it when the lane has room: nothing wasted, delay stays low.

What it is: a transparent Linux bridge with two Ethernet ports, cabled between the router's LAN side and
the switch (inside the NAT, where each phone is still visible). It changes no addresses, so the portal,
vouchers and Omada keep working. If the box is unplugged the network is cut, so it needs the router's
backup power (or a bypass relay later).

How each phone gets a lane:
  - phones are told apart by MAC address, so IPv4 and IPv6 are both shaped (Starlink gives phones IPv6;
    an IP-based shaper would let all v6 traffic through unshaped) and a randomized MAC is just a new phone.
  - a learner loop reads the bridge's own table of MACs seen on the LAN port every 2 s, gives each new one a
    number, and adds: an nftables map entry MAC -> number (marks its packets), and on each port an HTB
    class with that number, rate = ceil = the cap, holding a fair queue (fq_codel: every connection gets its
    own small line, and a line that starts building delay is trimmed early).
  - a MAC that leaves the bridge table (5 min idle) loses its lane; it gets a new one when it comes back.
  - anything unmarked (the router, Omada gear, MACs listed with --free) goes to an unshaped default class.
  - optional --total-down/--total-up: an overall ceiling above all lanes, set just under what the
    uplink really delivers so the queue lives here (fair) and not in Starlink's own buffer (unfair, laggy).

Run as root on the bridge box (Ubuntu): shaper.py up --wan enp1s0 --lan enp2s0 [--down 2 --up 1 ...]
                                         shaper.py stats   (JSON: per-lane bytes, drops, queue; port totals)
                                         shaper.py down    (removes everything; the bridge passes traffic
                                                            unshaped — the A/B switch for lab tests)
Needs: iproute2, nftables, (ethtool optional).
"""
import argparse, json, os, signal, subprocess, sys, time

STATE = "/run/ellafi-shaper.json"
FIRST, LAST = 10, 4000                 # lane numbers (HTB class minor ids, nft marks); 9999 = default class
DEFAULT = 9999


def sh(cmd, check=True):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f"{cmd}\n{r.stderr.strip()}")
    return r.stdout


def mbit(x):
    return f"{int(float(x) * 1000)}kbit"


def leaf(dev, cls, a):
    # fq_codel tuned for slow lanes: one 1500-byte packet takes 6 ms at 2 Mbps, so the default 5 ms target
    # would drop constantly. Lab 2026-11-09 (real Reels, 2 Mbps lane): target 10 ms still dropped ~40% of what
    # Meta's QUIC servers sent, so the target/interval are settings (--target-ms/--interval-ms) to sweep.
    # Small quantum so small packets (DNS, ACKs, game) aren't stuck behind big ones.
    # --leaf cake uses CAKE instead (what LibreQoS puts inside each subscriber class).
    if a.leaf == "cake":
        return f"tc qdisc replace dev {dev} parent 1:{cls:x} cake besteffort flows rtt 100ms"
    return f"tc qdisc replace dev {dev} parent 1:{cls:x} fq_codel target {a.target_ms}ms interval {a.interval_ms}ms quantum 300 limit 1000"


def port_tc(dev, rate, total, a):
    """HTB root on one port's outgoing side: 1:1 = overall ceiling, 1:<default> = unshaped traffic."""
    top = mbit(total) if total else "10gbit"
    sh(f"tc qdisc del dev {dev} root", check=False)
    sh(f"tc qdisc add dev {dev} root handle 1: htb default {DEFAULT:x} r2q 10")
    sh(f"tc class add dev {dev} parent 1: classid 1:1 htb rate {top} ceil {top}")
    sh(f"tc class add dev {dev} parent 1:1 classid 1:{DEFAULT:x} htb rate 1mbit ceil {top} prio 1")
    sh(leaf(dev, DEFAULT, a))


def lane_add(dev, n, rate, a):
    burst = f"{int(a.burst_kb)}k"
    sh(f"tc class replace dev {dev} parent 1:1 classid 1:{n:x} htb rate {mbit(rate)} ceil {mbit(rate)} "
       f"burst {burst} cburst {burst} prio 2")
    sh(leaf(dev, n, a))
    sh(f"tc filter replace dev {dev} parent 1: protocol all prio 1 handle {n} fw classid 1:{n:x}")


def lane_del(dev, n):
    sh(f"tc filter del dev {dev} parent 1: protocol all prio 1 handle {n} fw", check=False)
    sh(f"tc qdisc del dev {dev} parent 1:{n:x}", check=False)
    sh(f"tc class del dev {dev} classid 1:{n:x}", check=False)


def nft_setup(a):
    sh("nft delete table bridge ellafi", check=False)
    # down = packets coming in from the router side, to a phone (match destination MAC);
    # up = packets coming in from the phone side (match source MAC). Unknown MAC: no mark -> default class.
    rules = f"""
table bridge ellafi {{
  map lane {{ type ether_addr : mark; }}
  chain mark_lanes {{
    type filter hook forward priority 0; policy accept;
    iifname "{a.wan}" meta mark set ether daddr map @lane
    iifname "{a.lan}" meta mark set ether saddr map @lane
  }}
}}"""
    p = subprocess.run(["nft", "-f", "-"], input=rules, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(p.stderr)


def up(a):
    sh(f"ip link show {a.br}", check=False) or sh(f"ip link add {a.br} type bridge")
    sh(f"ip link set {a.br} type bridge stp_state 0 ageing_time {a.idle * 100}")
    for p in (a.wan, a.lan):
        sh(f"ip link set {p} master {a.br}")
        sh(f"ip link set {p} up")
        # no packet merging: GRO/TSO glue packets into 64 kB lumps, which a 2 Mbps lane would send as one
        # 260 ms block — fair queuing needs real packets
        sh(f"ethtool -K {p} gro off gso off tso off", check=False)
    sh(f"ip link set {a.br} up")
    # traffic on the bridge must not go through iptables (br_netfilter); nftables bridge table does the marking
    sh("sysctl -qw net.bridge.bridge-nf-call-iptables=0 net.bridge.bridge-nf-call-ip6tables=0", check=False)
    nft_setup(a)
    port_tc(a.lan, a.down, a.total_down, a)            # leaving toward the phones = download
    port_tc(a.wan, a.up, a.total_up, a)                # leaving toward the router = upload
    st = {"wan": a.wan, "lan": a.lan, "br": a.br, "lanes": {}, "args": vars(a)}
    save(st)
    print(f"shaper up: {a.down}/{a.up} Mbps per phone, overall {a.total_down or '-'}/{a.total_up or '-'}, "
          f"leaf {a.leaf}; learning MACs on {a.lan}", flush=True)
    learn(a, st)


def save(st):
    tmp = STATE + ".tmp"
    json.dump(st, open(tmp, "w")); os.replace(tmp, STATE)


def learn(a, st):
    free = {m.lower() for m in a.free}
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    lanes = st["lanes"]                                 # mac -> lane number
    while not stop:
        try:
            fdb = json.loads(sh(f"bridge -j fdb show br {a.br}") or "[]")
        except (RuntimeError, ValueError):
            fdb = []
        here = {e["mac"].lower() for e in fdb
                if e.get("ifname") == a.lan and "permanent" not in (e.get("state") or "")
                and not (int(e["mac"][:2], 16) & 1)}      # skip group addresses
        here -= free
        for mac in sorted(here - set(lanes)):
            used = set(lanes.values())
            n = next((i for i in range(FIRST, LAST) if i not in used), None)
            if n is None:
                break
            lane_add(a.lan, n, a.down, a); lane_add(a.wan, n, a.up, a)
            sh(f"nft add element bridge ellafi lane '{{ {mac} : {n} }}'")
            lanes[mac] = n
            print(f"+ lane {n} {mac}", flush=True)
        for mac in sorted(set(lanes) - here):
            n = lanes.pop(mac)
            sh(f"nft delete element bridge ellafi lane '{{ {mac} }}'", check=False)
            lane_del(a.lan, n); lane_del(a.wan, n)
            print(f"- lane {n} {mac}", flush=True)
        save(st)
        time.sleep(2)


def down(a):
    st = json.load(open(STATE)) if os.path.exists(STATE) else {}
    for dev in (st.get("lan", a.lan), st.get("wan", a.wan)):
        if dev:
            sh(f"tc qdisc del dev {dev} root", check=False)
    sh("nft delete table bridge ellafi", check=False)
    if os.path.exists(STATE):
        os.remove(STATE)
    print("shaper down: bridge passes traffic unshaped")


def stats(a):
    st = json.load(open(STATE)) if os.path.exists(STATE) else {}
    lan, wan = st.get("lan", a.lan), st.get("wan", a.wan)
    mac_of = {v: k for k, v in st.get("lanes", {}).items()}
    out = {"t": time.time(), "lanes": {}, "ports": {}}
    for dev, way in ((lan, "down"), (wan, "up")):
        if not dev:
            continue
        for c in json.loads(sh(f"tc -s -j class show dev {dev}", check=False) or "[]"):
            n = int(c["handle"].split(":")[1], 16)
            key = mac_of.get(n, "default" if n == DEFAULT else None)
            if key is None:
                continue
            e = out["lanes"].setdefault(key, {})
            e[way] = {"bytes": c["stats"]["bytes"], "packets": c["stats"]["packets"],
                      "drops": c["stats"]["drops"], "backlog": c["stats"].get("backlog", 0)}
        for q in json.loads(sh(f"tc -s -j qdisc show dev {dev}", check=False) or "[]"):
            if q.get("kind") in ("fq_codel", "cake") and q.get("parent", "").startswith("1:"):
                n = int(q["parent"].split(":")[1], 16)
                key = mac_of.get(n, "default" if n == DEFAULT else None)
                if key in out["lanes"] and way in out["lanes"][key]:
                    out["lanes"][key][way]["qdrops"] = q.get("drops", 0)
    for dev in (lan, wan):
        if dev:
            s = json.loads(sh(f"ip -s -j link show {dev}"))[0]["stats64"]
            out["ports"][dev] = {"rx": s["rx"]["bytes"], "tx": s["tx"]["bytes"],
                                 "rx_drop": s["rx"]["dropped"], "tx_drop": s["tx"]["dropped"]}
    print(json.dumps(out))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cmd", choices=["up", "down", "stats"])
    p.add_argument("--wan", help="port cabled to the router (its LAN side)")
    p.add_argument("--lan", help="port cabled to the switch / APs")
    p.add_argument("--br", default="br-shape")
    p.add_argument("--down", type=float, default=2.0, help="Mbps per phone, download")
    p.add_argument("--up", type=float, default=1.0, help="Mbps per phone, upload")
    p.add_argument("--total-down", type=float, help="overall Mbps ceiling, download (just under the uplink)")
    p.add_argument("--total-up", type=float, help="overall Mbps ceiling, upload")
    p.add_argument("--burst-kb", type=float, default=32, help="data a lane may send at full speed before the "
                   "cap applies (small = smooth; bigger = snappier first moments of a page)")
    p.add_argument("--leaf", choices=["fq_codel", "cake"], default="fq_codel")
    p.add_argument("--target-ms", type=int, default=10, help="fq_codel: queue delay a lane tolerates before dropping")
    p.add_argument("--interval-ms", type=int, default=100, help="fq_codel: how long the delay must persist first")
    p.add_argument("--idle", type=int, default=300, help="seconds before an idle MAC loses its lane")
    p.add_argument("--free", nargs="*", default=[], help="MACs never shaped (router, controller, Mahal devices)")
    a = p.parse_args()
    if a.cmd == "up" and not (a.wan and a.lan):
        p.error("up needs --wan and --lan")
    if os.geteuid() != 0 and a.cmd != "stats":
        sys.exit("run as root")
    {"up": up, "down": down, "stats": stats}[a.cmd](a)


if __name__ == "__main__":
    main()
