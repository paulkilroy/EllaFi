#!/bin/bash
# Turn the shaper on for a real-iPhone test: 2/1 lanes, fq_codel target/interval as given (default 200/1000 ms).
# The iPhone is NOT in the free list here (run1/run2 free it so it can't disturb the Pi runs).
# Usage: iphone_setup.sh [target_ms] [interval_ms]
SH=paulkilroy@192.168.0.109; IPHONE=04:2e:c1:93:5e:03
TARGET=${1:-200}; INTERVAL=${2:-1000}
ssh $SH "sudo systemctl stop ellafi-shaper shaper 2>/dev/null; sleep 1; sudo python3 /usr/local/bin/shaper.py down >/dev/null 2>&1; \
         sudo systemd-run --unit=shaper --collect /usr/bin/python3 /usr/local/bin/shaper.py up --wan eno1 --lan enp4s0 --br br0 \
           --down 2 --up 1 --target-ms $TARGET --interval-ms $INTERVAL --free 8c:86:dd:e3:78:f0 4e:fc:79:4c:c9:74 >/dev/null"
sleep 6
ssh $SH "systemctl is-active shaper; sudo nft list map bridge ellafi lane | grep -i $IPHONE || echo 'iPhone has no lane yet (it gets one when it sends)'; \
         tc qdisc show dev enp4s0 | grep -m1 fq_codel"
