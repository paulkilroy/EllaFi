#!/bin/bash
# Make the Pi phones behave like phones on a weak / bad Wi-Fi link (runs on pi02w as root).
# Delay and bunching are added on each phone's Wi-Fi interface on the way out (its requests and acknowledgements),
# which lengthens every round trip the same way a slow, retrying radio does. "slot" releases packets in bursts at
# irregular gaps (how Wi-Fi sends after contention/retries) without reordering them.
#   good: nothing added
#   weak: +60 ms, released in bursts every 5-40 ms
#   bad:  +150 ms, released in bursts every 20-150 ms, 1% of packets lost
# Usage: phone_netem.sh good|weak|bad
for d in wlan0 wlan1 wlan1b; do
  tc qdisc del dev $d root 2>/dev/null
  case $1 in
    weak) tc qdisc add dev $d root netem delay 60ms slot 5ms 40ms limit 10000 ;;
    bad)  tc qdisc add dev $d root netem delay 150ms slot 20ms 150ms loss 1% limit 10000 ;;
  esac
done
tc qdisc show dev wlan1 | head -1
