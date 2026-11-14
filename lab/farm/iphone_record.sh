#!/bin/bash
# Record a real-iPhone run on both shaper ports (headers only) and count the iPhone's download at each hop.
#   from internet = eno1 (router side), to AP = enp4s0 (after the shaper); shaper dropped = fq_codel drops
# Clock-timed so Paul and the recording line up: SECS of scrolling from START with a ping every second, then a
# 60 s idle ping baseline (phone awake on the home screen, Facebook closed).
# Usage: iphone_record.sh <label> <start HH:MM:SS> [seconds]
SH=paulkilroy@192.168.0.109; IPHONE=04:2e:c1:93:5e:03
# the iPhone's address by its MAC (it moves: a fake phone took .108 on Nov 13)
CL=/private/tmp/claude-501/-Users-paulkilroy-dev-EllaFi/3f0cb27a-5c85-4227-80ca-0bf768b2dd08/scratchpad/lab_clients.py
IP=$(~/.platformio/penv/bin/python3 $CL 2>/dev/null | awk -v m=$(echo $IPHONE | tr a-f: A-F-) '$2==m && / ssid Mahal / {print $1; exit}')
[ -n "$IP" ] || { echo "iPhone $IPHONE not on Mahal (controller client list) — join Mahal and retry"; exit 1; }
LABEL=$1; START=$(date -j -f %H:%M:%S "$2" +%s); SECS=${3:-300}
OUT=/Users/paulkilroy/dev/EllaFi/lab/farm/results/iphone-$(date +%Y%m%d-%H%M)-$LABEL; mkdir -p $OUT
ssh $SH "tc -s qdisc show dev enp4s0" > $OUT/qdisc_before.txt
sleep $(( START - $(date +%s) ))
ssh $SH "sudo rm -f /var/tmp/caps/ip_*; for p in eno1 enp4s0; do sudo systemd-run --unit=ip-\$p --collect timeout $SECS tcpdump -i \$p -s 96 -w /var/tmp/caps/ip_\$p.pcap ether host $IPHONE >/dev/null 2>&1; done"
echo "$(date +%H:%M:%S) RECORDING $SECS s -> $OUT" | tee $OUT/log.txt
ssh $SH "ping -c $((SECS - 5)) -i 1 $IP | tail -2" > $OUT/ping_scrolling.txt
sleep $(( START + SECS - $(date +%s) ))
ssh $SH "ping -c 55 -i 1 $IP | tail -2" > $OUT/ping_idle.txt      # after STOP: phone awake on the home screen
ssh $SH "while systemctl is-active -q ip-eno1 || systemctl is-active -q ip-enp4s0; do sleep 2; done; \
         echo from_internet \$(sudo python3 /usr/local/bin/pcap_bytes.py /var/tmp/caps/ip_eno1.pcap $IP); \
         echo to_ap \$(sudo python3 /usr/local/bin/pcap_bytes.py /var/tmp/caps/ip_enp4s0.pcap $IP); \
         journalctl -u ip-eno1 -u ip-enp4s0 --no-pager | grep 'dropped by kernel'" > $OUT/counts.txt
ssh $SH "tc -s qdisc show dev enp4s0" > $OUT/qdisc_after.txt
scp -q $SH:/var/tmp/caps/ip_eno1.pcap $SH:/var/tmp/caps/ip_enp4s0.pcap $OUT/
echo "$(date +%H:%M:%S) DONE" | tee -a $OUT/log.txt
cat $OUT/counts.txt $OUT/ping_idle.txt $OUT/ping_scrolling.txt
