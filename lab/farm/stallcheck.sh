#!/bin/bash
# Check the fake-phone hang fix (scroll3.py --stall-s) on the real lab without touching the Mahal AP
# (an AP limit change makes every client reconnect). Shaper 2/1 per phone, Paul's devices exempt.
# Usage: stallcheck.sh [minutes]
MIN=${1:-20}
PZ=paulkilroy@192.168.0.105; SH=paulkilroy@192.168.0.109
FREE="8c:86:dd:e3:78:f0 4e:fc:79:4c:c9:74 04:2e:c1:93:5e:03"
HERE=$(cd "$(dirname "$0")" && pwd); OUT=$HERE/results/stallcheck-$(date +%Y%m%d-%H%M); mkdir -p $OUT
scp -q $HERE/scroll3.py $PZ:farm/scroll3.py
ssh $SH "sudo systemctl stop ellafi-shaper shaper 2>/dev/null; sleep 1; sudo python3 /usr/local/bin/shaper.py down >/dev/null 2>&1; \
         sudo systemd-run --unit=shaper --collect /usr/bin/python3 /usr/local/bin/shaper.py up --wan eno1 --lan enp4s0 \
         --br br0 --down 2 --up 1 --target-ms 50 --interval-ms 300 --free $FREE >/dev/null"
sleep 10
echo "$(date +%H:%M:%S) phones start, $MIN min -> $OUT" | tee $OUT/log.txt
ssh $PZ "cd farm; ~/qenv/bin/python scroll3.py --slots 8 --name p1 --minutes $MIN > /tmp/s1.json & \
         sudo ip vrf exec p3 /home/paulkilroy/qenv/bin/python scroll3.py --slots 8 --name p3 --minutes $MIN > /tmp/s3.json & \
         sudo ip vrf exec p4 /home/paulkilroy/qenv/bin/python scroll3.py --slots 8 --name p4 --minutes $MIN > /tmp/s4.json & wait; \
         cat /tmp/s1.json /tmp/s3.json /tmp/s4.json" > $OUT/phones.jsonl
ssh $SH "tc -s qdisc show dev enp4s0 | head -40" > $OUT/qdisc_after.txt
echo "$(date +%H:%M:%S) done" | tee -a $OUT/log.txt
