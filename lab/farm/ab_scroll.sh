#!/bin/bash
# Scroll A/B from the Mac: 3 emulated phones on pi02w (scroll.py) against the Pi 4 server, one run per mode:
#   open   = shaper off, Omada off
#   omada  = shaper off, Omada client limit 2048/1024 (today's Bakhaw setting)
#   shape  = shaper 2/1 per phone (fq_codel target 50 ms), Omada 4096/2048 as backstop
# Per run: capture the phones' traffic on both shaper ports (eno1 = from internet, enp4s0 = to AP), server TCP
# counters before/after. Results in lab/farm/results/<stamp>/. Leaves shaper ON (50 ms) and Omada 4/2 at the end.
# Usage: ab_scroll.sh <testlab.py> [minutes] [modes...]
TL=${1:?testlab.py}; MIN=${2:-4}; shift 2; MODES=${@:-open omada shape}
PY=~/.platformio/penv/bin/python3
PZ=paulkilroy@192.168.0.105; SRV=paulkilroy@192.168.1.174; SH=paulkilroy@192.168.0.109
FREE="8c:86:dd:e3:78:f0 4e:fc:79:4c:c9:74 04:2e:c1:93:5e:03"     # AP, Mac, Paul's iPhone: never shaped
OUT=$(cd "$(dirname "$0")" && pwd)/results/$(date +%Y%m%d-%H%M); mkdir -p $OUT
FILTER="host 192.168.0.105 or host 192.168.0.106 or host 192.168.0.107"
omada() { for i in 1 2 3; do $PY "$TL" --set-mahal $1 $2 | grep -q "^SET 0" && return; sleep 20; done; echo "omada set FAILED"; }
shaper_off() { ssh $SH "sudo systemctl stop shaper 2>/dev/null; sleep 1; sudo python3 /usr/local/bin/shaper.py down >/dev/null 2>&1"; }
shaper_on() { shaper_off; ssh $SH "sudo systemd-run --unit=shaper --collect /usr/bin/python3 /usr/local/bin/shaper.py up --wan eno1 --lan enp4s0 --br br0 --down 2 --up 1 --target-ms 50 --interval-ms 300 --free $FREE >/dev/null"; }
tcp() { ssh $SRV "awk '/^Tcp:/ {l=\$0} END {split(l, f, \" \"); print f[12], f[13]}' /proc/net/snmp" 2>/dev/null; }
for m in $MODES; do
  echo "$(date +%H:%M) $m"
  case $m in
    open)  shaper_off; omada 0 0 ;;
    omada) shaper_off; omada 2048 1024 ;;
    shape) shaper_on;  omada 4096 2048 ;;
  esac
  sleep 30                                             # AP applies the setting, shaper learns the phones
  ssh $SH "sudo rm -f /var/tmp/caps/ab_*; for p in eno1 enp4s0; do sudo systemd-run --unit=ab-\$p --collect timeout $((MIN*60+20)) tcpdump -i \$p -s 96 -w /var/tmp/caps/ab_\$p.pcap '$FILTER' >/dev/null 2>&1; done"
  read o0 r0 <<< "$(tcp)"
  ssh $PZ "cd farm; python3 scroll.py --name p1 --seed 1 --minutes $MIN > /tmp/s1.json & \
           sudo ip vrf exec p3 python3 scroll.py --name p3 --seed 3 --minutes $MIN > /tmp/s3.json & \
           sudo ip vrf exec p4 python3 scroll.py --name p4 --seed 4 --minutes $MIN > /tmp/s4.json & wait; cat /tmp/s1.json /tmp/s3.json /tmp/s4.json" > $OUT/$m.phones.jsonl
  read o1 r1 <<< "$(tcp)"
  ssh $SH "while systemctl is-active -q ab-eno1 || systemctl is-active -q ab-enp4s0; do sleep 2; done"
  scp -q $SH:/var/tmp/caps/ab_eno1.pcap $OUT/$m.internet.pcap; scp -q $SH:/var/tmp/caps/ab_enp4s0.pcap $OUT/$m.to_ap.pcap
  echo "$m server_sent_pkts $((o1-o0)) server_resent_pkts $((r1-r0))" >> $OUT/server.txt
  ssh $SH "tc -s qdisc show dev enp4s0 | grep -A1 fq_codel | grep -o 'dropped [0-9]*' | awk '{s+=\$2} END {print \"$m shaper_dropped_pkts\", s+0}'" >> $OUT/server.txt
done
shaper_on; omada 4096 2048
echo "$(date +%H:%M) done -> $OUT"
