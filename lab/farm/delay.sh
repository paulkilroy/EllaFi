#!/bin/bash
# Starlink-like delay on the download server's cable: every packet it sends waits a fixed time,
# so phones see the server about that much further away (Bakhaw's Starlink adds ~30-60 ms).
# Fixed delay, no jitter: netem jitter reorders packets, which TCP would read as loss.
# Usage: delay.sh [ms]   (default 40; 0 = remove)
DEV=eth0; MS=${1:-40}
if [ "$MS" = 0 ]; then tc qdisc del dev $DEV root 2>/dev/null; else tc qdisc replace dev $DEV root netem delay ${MS}ms limit 10000; fi
tc qdisc show dev $DEV
