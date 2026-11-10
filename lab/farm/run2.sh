#!/bin/bash
# run1.sh plus: --profile good|weak|bad (phone_netem.sh on the Pi phones), --total-down MBPS (shaper overall ceiling),
# --fake-lanes N (N static fake phones added to the bridge so the shaper builds N extra lanes; records the shaper
# box's memory and CPU time during the run). Same counts as run1.sh, all with packet headers.
# Usage: run2.sh <outdir> <label> <setup> <minutes> [run1 options] [--profile P] [--total-down M] [--fake-lanes N]
OUTD=$1; LABEL=$2; SETUP=$3; MIN=$4; shift 4
TARGET=50; INTERVAL=300; BURST=32; SLOTS=8; WATCH=4; CC=2; DELAY=40; PROFILE=good; TOTAL=""; FAKE=0
while [ $# -gt 0 ]; do
  case $1 in
    --target-ms) TARGET=$2;; --interval-ms) INTERVAL=$2;; --burst-kb) BURST=$2;; --slots) SLOTS=$2;;
    --watch-s) WATCH=$2;; --cc) CC=$2;; --delay) DELAY=$2;; --profile) PROFILE=$2;;
    --total-down) TOTAL=$2;; --fake-lanes) FAKE=$2;;
  esac; shift 2
done
PY=/Users/paulkilroy/.platformio/penv/bin/python3
SP=/private/tmp/claude-501/-Users-paulkilroy-dev-EllaFi/3f0cb27a-5c85-4227-80ca-0bf768b2dd08/scratchpad
TL=$SP/testlab_run.py; BWC=$SP/testlab_bwc.py
PZ=paulkilroy@192.168.0.105; SH=paulkilroy@192.168.0.109; SRV=paulkilroy@192.168.1.174
FREE="8c:86:dd:e3:78:f0 4e:fc:79:4c:c9:74 04:2e:c1:93:5e:03"
STATE=$OUTD/.lab_state; mkdir -p $OUTD; touch $STATE
D=$OUTD/$LABEL; mkdir -p $D
log() { echo "$(date +%H:%M:%S) [$LABEL] $*" | tee -a $OUTD/log.txt; }
want_omada() {
  [ "$(cat $STATE)" = "omada $1 $2" ] && return 0
  for i in 1 2 3 4 5; do
    if [ $1 = 0 ]; then $PY $TL --set-mahal 0 0 2>/dev/null | grep -q "^NOW.*'downLimitEnable': False" && break
    else $PY $TL --set-mahal $1 $2 2>/dev/null | grep -q "^NOW.*'downLimit': $1" && break; fi
    sleep 30; [ $i = 5 ] && { log "ABORT: AP limit not confirmed"; exit 1; }
  done
  echo "omada $1 $2" > $STATE; log "AP limit set $1/$2; waiting 200 s for it to apply"; sleep 200
}
gw() { [ "$(cat $STATE.gw 2>/dev/null)" = "$1" ] && return 0; $PY $BWC --rule-$1 "Lab 2/1" >/dev/null 2>&1; echo $1 > $STATE.gw; log "gateway rule $1"; sleep 20; }
fake_lanes() {   # $1 = add|del ; static fake MACs 02:fa:ce:00:XX:YY on the AP-side port
  ssh $SH "for i in \$(seq 1 $FAKE); do sudo bridge fdb $1 02:fa:ce:00:\$(printf %02x \$((i/256))):\$(printf %02x \$((i%256))) dev enp4s0 master static 2>/dev/null; done; true"
}
shaper_off() { ssh $SH "sudo systemctl stop shaper 2>/dev/null; sleep 1; sudo python3 /usr/local/bin/shaper.py down >/dev/null 2>&1"; }
shaper_on() {
  shaper_off
  ssh $SH "sudo systemd-run --unit=shaper --collect /usr/bin/python3 /usr/local/bin/shaper.py up --wan eno1 --lan enp4s0 --br br0 --down 2 --up 1 --target-ms $TARGET --interval-ms $INTERVAL --burst-kb $BURST ${TOTAL:+--total-down $TOTAL} --free $FREE >/dev/null"
  sleep $((5 + FAKE / 10))
}
ssh $SRV "grep -q ' -A $CC ' /etc/systemd/system/quic-server.service || { sudo sed -i 's/ -A [0-9] / -A $CC /' /etc/systemd/system/quic-server.service; sudo systemctl daemon-reload; sudo systemctl restart quic-server; }; sudo tc qdisc replace dev eth0 root netem delay ${DELAY}ms limit 10000" 2>/dev/null
ssh $PZ "sudo bash farm/phone_netem.sh $PROFILE" >/dev/null 2>&1
[ $FAKE -gt 0 ] && fake_lanes add
case $SETUP in
  open)    shaper_off; gw off; want_omada 0 0 ;;
  omada)   shaper_off; gw off; want_omada 2048 1024 ;;
  gateway) shaper_off; want_omada 0 0; gw on ;;
  shaper)  gw off; want_omada 4096 2048; shaper_on ;;
esac
echo "setup=$SETUP min=$MIN target_ms=$TARGET interval_ms=$INTERVAL burst_kb=$BURST slots=$SLOTS watch_s=$WATCH cc=$CC delay_ms=$DELAY profile=$PROFILE total_down=${TOTAL:-none} fake_lanes=$FAKE" > $D/settings.txt
[ $FAKE -gt 0 ] && ssh $SH "echo lanes \$(sudo nft list map bridge ellafi lane | grep -c ' : '); free -m | sed -n 2p; head -1 /proc/stat" > $D/shaper_load_before.txt
PH="host 192.168.0.105 or host 192.168.0.106 or host 192.168.0.107"
ssh $SH "sudo rm -f /var/tmp/caps/r1_*; for p in eno1 enp4s0; do sudo systemd-run --unit=r1-\$p --collect timeout $((MIN*60+40)) tcpdump -i \$p -s 96 -w /var/tmp/caps/r1_\$p.pcap '$PH' >/dev/null 2>&1; done"
rx() { ssh $PZ "cat /sys/class/net/wlan0/statistics/rx_bytes /sys/class/net/wlan1/statistics/rx_bytes /sys/class/net/wlan1b/statistics/rx_bytes" | tr '\n' ' '; }
tx() { ssh $SRV "cat /sys/class/net/eth0/statistics/tx_bytes" 2>/dev/null; }
r0=$(rx); s0=$(tx)
log "scrolling: $(cat $D/settings.txt)"
ssh $PZ "cd farm; ~/qenv/bin/python scroll3.py --slots $SLOTS --watch-s $WATCH --name p1 --minutes $MIN > /tmp/r1.json & \
         sudo ip vrf exec p3 /home/paulkilroy/qenv/bin/python scroll3.py --slots $SLOTS --watch-s $WATCH --name p3 --minutes $MIN > /tmp/r3.json & \
         sudo ip vrf exec p4 /home/paulkilroy/qenv/bin/python scroll3.py --slots $SLOTS --watch-s $WATCH --name p4 --minutes $MIN > /tmp/r4.json & wait; \
         cat /tmp/r1.json /tmp/r3.json /tmp/r4.json" > $D/phones.jsonl
s1=$(tx); r1=$(rx)
[ $FAKE -gt 0 ] && ssh $SH "echo lanes \$(sudo nft list map bridge ellafi lane | grep -c ' : '); free -m | sed -n 2p; head -1 /proc/stat; ps -o rss=,pcpu= -C python3" > $D/shaper_load_after.txt
echo "server_sent $((s1-s0))" > $D/counts.txt
set -- $r0; a=$1; b=$2; c=$3; set -- $r1
echo "phones_got $(( ($1-a) + ($2-b) + ($3-c) )) p1 $(($1-a)) p3 $(($2-b)) p4 $(($3-c))" >> $D/counts.txt
ssh $SH "while systemctl is-active -q r1-eno1 || systemctl is-active -q r1-enp4s0; do sleep 2; done; \
         echo from_internet \$(sudo python3 /usr/local/bin/pcap_bytes.py /var/tmp/caps/r1_eno1.pcap 192.168.0.105 192.168.0.106 192.168.0.107); \
         echo to_ap \$(sudo python3 /usr/local/bin/pcap_bytes.py /var/tmp/caps/r1_enp4s0.pcap 192.168.0.105 192.168.0.106 192.168.0.107); \
         echo shaper_dropped \$(tc -s qdisc show dev enp4s0 | grep -A1 fq_codel | grep -o 'dropped [0-9]*' | awk '{s+=\$2} END {print s+0}'); \
         journalctl -u r1-eno1 -u r1-enp4s0 --no-pager | grep -c ' 0 packets dropped by kernel' | sed 's/^/captures_clean_of_2 /'" >> $D/counts.txt
[ $FAKE -gt 0 ] && fake_lanes del
ssh $PZ "sudo bash farm/phone_netem.sh good" >/dev/null 2>&1
log "done: $(tr '\n' ' ' < $D/counts.txt)"
