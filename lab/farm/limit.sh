#!/bin/bash
# Per-phone 2 down / 1 up limit applied at each phone's own Wi-Fi interface on pi02w, two ways:
#   police = drop anything over the rate (what the Omada AP/gateway limiters do)
#   shape  = queue it instead, each connection in its own short line (HTB + fq_codel, what shaper.py does)
#   open   = no limit
# Down = ingress (redirected through an ifb so it can be queued); up = egress.
# Usage: sudo limit.sh open|police|shape [down_mbit up_mbit burst_kb]   |  sudo limit.sh stats
MODE=${1:?open|police|shape|stats}; DOWN=${2:-2}; UP=${3:-1}; BURST=${4:-32}
DEVS="wlan0 wlan1 wlan1b"
modprobe ifb numifbs=0
i=0
for d in $DEVS; do
  ifb=ifb$i; i=$((i+1))
  if [ "$MODE" = stats ]; then
    echo "== $d"; tc -s filter show dev $d ingress | grep -E "police|Sent|dropped|overlimits"
    tc -s qdisc show dev $ifb 2>/dev/null | grep -E "qdisc|Sent|backlog" | head -6
    continue
  fi
  tc qdisc del dev $d clsact 2>/dev/null; tc qdisc del dev $d root 2>/dev/null
  ip link del $ifb 2>/dev/null
  [ "$MODE" = open ] && continue
  tc qdisc add dev $d clsact
  if [ "$MODE" = police ]; then
    tc filter add dev $d ingress matchall action police rate ${DOWN}mbit burst ${BURST}k drop
    tc filter add dev $d egress matchall action police rate ${UP}mbit burst $((BURST/2))k drop
  else
    ip link add $ifb type ifb && ip link set $ifb up
    tc filter add dev $d ingress matchall action mirred egress redirect dev $ifb
    tc qdisc add dev $ifb root handle 1: htb default 1
    tc class add dev $ifb parent 1: classid 1:1 htb rate ${DOWN}mbit ceil ${DOWN}mbit burst ${BURST}k
    tc qdisc add dev $ifb parent 1:1 fq_codel target 10ms interval 100ms quantum 300
    tc qdisc add dev $d root handle 1: htb default 1
    tc class add dev $d parent 1: classid 1:1 htb rate ${UP}mbit ceil ${UP}mbit burst $((BURST/2))k
    tc qdisc add dev $d parent 1:1 fq_codel target 10ms interval 100ms quantum 300
  fi
done
[ "$MODE" != stats ] && echo "limit: $MODE ${DOWN}/${UP} Mbit burst ${BURST}k on $DEVS"
