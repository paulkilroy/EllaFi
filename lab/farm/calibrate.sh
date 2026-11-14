#!/bin/bash
# Calibration: does the lab QUIC server push into Omada's 2/1 limit the way Meta's servers did for Paul's iPhone
# (bursts start 14-22 Mbps, then settle ~6.5 Mbps arriving at the AP; 10 ms peaks median ~49 Mbps)?
# One Pi phone (p3, VRF) runs scroll2.py over QUIC for MIN minutes; shaper OFF, Omada at 2/1; capture on eno1.
# Usage: calibrate.sh <testlab.py> [minutes] [extra scroll3.py args, e.g. --slots 8]
TL=${1:?testlab.py}; MIN=${2:-3}; shift 2; EXTRA="$*"
PY=~/.platformio/penv/bin/python3; PZ=paulkilroy@192.168.0.105; SH=paulkilroy@192.168.0.109
OUT=$(cd "$(dirname "$0")" && pwd)/results/calibrate-$(date +%Y%m%d-%H%M%S)$(echo $EXTRA | tr -d " -"); mkdir -p $OUT
ssh $SH "sudo systemctl stop ellafi-shaper shaper 2>/dev/null; sleep 1; sudo python3 /usr/local/bin/shaper.py down >/dev/null 2>&1"
# the cloud controller sometimes times out: retry, and refuse to run on the wrong limit
for i in 1 2 3 4 5; do
  $PY "$TL" --set-mahal 2048 1024 2>/dev/null | grep -q "^NOW.*'downLimit': 2048" && { echo "Omada 2/1 confirmed"; break; }
  echo "Omada set failed (try $i)"; sleep 30
  [ $i = 5 ] && { echo "ABORT: Omada limit not confirmed"; exit 1; }
done
sleep 60
ssh $SH "sudo rm -f /var/tmp/caps/cal_*; sudo systemd-run --unit=cal-cap --collect timeout $((MIN*60+20)) tcpdump -i eno1 -s 96 -w /var/tmp/caps/cal_eno1.pcap 'host 192.168.0.106' >/dev/null 2>&1"
echo "$(date +%H:%M:%S) scrolling (QUIC, p3)"
ssh $PZ "cd farm; sudo ip vrf exec p3 /home/paulkilroy/qenv/bin/python scroll3.py --name p3 --minutes $MIN $EXTRA" > $OUT/phone.json
ssh $SH "while systemctl is-active -q cal-cap; do sleep 2; done; journalctl -u cal-cap --no-pager | grep 'dropped by kernel'"
scp -q $SH:/var/tmp/caps/cal_eno1.pcap $OUT/internet.pcap
cat $OUT/phone.json
echo "$(date +%H:%M:%S) done -> $OUT"
