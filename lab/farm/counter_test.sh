#!/bin/bash
# Does Omada's per-client "down" counter count before or after the AP's rate limit drops data?
# 3 emulated phones (pi02w) scroll for MIN minutes with ONLY Omada's limit on (shaper off). Three numbers per phone:
#   (1) mini PC -> AP bytes (capture on enp4s0, exact)  (2) bytes the phone app received (scroll.py)
#   (3) Omada's client counter (TestLab controller), read until two readings 2.5 min apart agree.
#   Also every run: mini PC router-side capture (eno1 = from internet) and the Pi 4 server's TCP counters
#   (packets sent / resent) before and after, so every column of the results table is measured.
# Omada resets client counters when the SSID limit changes (clients reassociate), so the counter covers this run.
# Usage: counter_test.sh <testlab.py> <down_kbps> <up_kbps> [minutes]
TL=${1:?testlab.py}; DK=${2:?}; UK=${3:?}; MIN=${4:-4}
PY=~/.platformio/penv/bin/python3; PZ=paulkilroy@192.168.0.105; SH=paulkilroy@192.168.0.109; SRV=paulkilroy@192.168.1.174
HERE=$(cd "$(dirname "$0")" && pwd); OUT=$HERE/results/counter-$(date +%Y%m%d-%H%M)-$DK; mkdir -p $OUT
CL=/private/tmp/claude-501/-Users-paulkilroy-dev-EllaFi/3f0cb27a-5c85-4227-80ca-0bf768b2dd08/scratchpad/lab_clients.py
ssh $SH "sudo systemctl stop ellafi-shaper shaper 2>/dev/null; sleep 1; sudo python3 /usr/local/bin/shaper.py down >/dev/null 2>&1"
$PY "$TL" --set-mahal $DK $UK | grep -E "^SET|^NOW"
echo "$(date +%H:%M:%S) limit set; waiting for clients to reassociate and the controller to refresh"
sleep 200
$PY $CL | grep pi02w | tee $OUT/omada_before.txt
tcp() { ssh $SRV "awk '/^Tcp:/ {l=\$0} END {split(l, f, \" \"); print f[12], f[13]}' /proc/net/snmp" 2>/dev/null; }   # OutSegs RetransSegs
ssh $SH "sudo rm -f /var/tmp/caps/ct_*; for p in eno1 enp4s0; do sudo systemd-run --unit=ct-\$p --collect timeout $((MIN*60+30)) tcpdump -i \$p -s 96 -w /var/tmp/caps/ct_\$p.pcap 'host 192.168.0.105 or host 192.168.0.106 or host 192.168.0.107' >/dev/null 2>&1; done"
read o0 r0 <<< "$(tcp)"
echo "$(date +%H:%M:%S) scrolling"
ssh $PZ "cd farm; python3 scroll.py --name p1 --seed 1 --minutes $MIN > /tmp/c1.json & \
         sudo ip vrf exec p3 python3 scroll.py --name p3 --seed 3 --minutes $MIN > /tmp/c3.json & \
         sudo ip vrf exec p4 python3 scroll.py --name p4 --seed 4 --minutes $MIN > /tmp/c4.json & wait; cat /tmp/c1.json /tmp/c3.json /tmp/c4.json" > $OUT/phones.jsonl
read o1 r1 <<< "$(tcp)"; echo "server_sent_pkts $((o1-o0)) server_resent_pkts $((r1-r0))" | tee $OUT/server.txt
ssh $SH "while systemctl is-active -q ct-eno1 || systemctl is-active -q ct-enp4s0; do sleep 2; done; journalctl -u ct-eno1 -u ct-enp4s0 --no-pager | grep 'dropped by kernel'" | tee $OUT/capture_drops.txt
scp -q $SH:/var/tmp/caps/ct_eno1.pcap $OUT/internet.pcap; scp -q $SH:/var/tmp/caps/ct_enp4s0.pcap $OUT/toap.pcap
echo "$(date +%H:%M:%S) scroll done; reading Omada until stable"
prev=""
for i in $(seq 1 6); do
  sleep 150
  cur=$($PY $CL | grep pi02w | sort); echo "$cur" > $OUT/omada_after_$i.txt
  [ "$cur" = "$prev" ] && break; prev=$cur
done
cp $OUT/omada_after_$i.txt $OUT/omada_after.txt
echo "$(date +%H:%M:%S) done -> $OUT"
