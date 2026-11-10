#!/bin/bash
# Run all Wi-Fi phones on pi02w at once against the Pi 4 download server; one JSON line per phone.
# p1 runs in the main table (wlan0); p3/p4 in their VRFs (see phones_up.sh). Same seed per phone = same traffic each run.
# Note: TCP resend counters are per network namespace, so every phone's "tcp" block is the whole box's total.
# Usage: sudo run.sh <label> [minutes]   -> results/<label>.jsonl
LABEL=${1:?label}; MIN=${2:-5}
URL="http://192.168.1.174:8080/__down?bytes={n}"
cd "$(dirname "$0")"; mkdir -p results; OUT=results/$LABEL.jsonl; : > $OUT
python3 phone.py --name p1 --seed 1 --minutes $MIN --ping 192.168.0.1 --url "$URL" >> $OUT &
for v in p3 p4; do
  ip vrf exec $v python3 phone.py --name $v --seed ${v#p} --minutes $MIN --ping 192.168.0.1 --url "$URL" >> $OUT &
done
wait
cat $OUT
