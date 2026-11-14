#!/bin/bash
# A/B with the Reels-like fake phone (scroll3.py over QUIC, --slots 8): 3 Pi phones, MIN minutes per setup.
#   open     shaper off, AP limit off, gateway off
#   omada    shaper off, AP per-client limit 2048/1024 (Bakhaw today), gateway off
#   gateway  shaper off, AP limit off, ER605 Bandwidth Control "Lab 2/1" (Individual, IP group = lab phones)
#   shaper   shaper 2/1 per phone (fq_codel target 50 ms), AP limit 4096/2048 as backstop, gateway off
# Every column, every setup: Pi 4 bytes sent (eth0 counter), mini PC captures both ports, Omada per-client
# counters before/after (read until two readings 150 s apart agree), each phone's own counts.
# Ends with: gateway off, shaper on, AP limit 4/2. Results: lab/farm/results/ab3-<stamp>/<setup>.*
# Usage: ab3.sh [minutes] [setups...]
MIN=${1:-4}; shift; SETUPS=${@:-open omada gateway shaper}
PY=/Users/paulkilroy/.platformio/penv/bin/python3
SP=/private/tmp/claude-501/-Users-paulkilroy-dev-EllaFi/3f0cb27a-5c85-4227-80ca-0bf768b2dd08/scratchpad
TL=$SP/testlab_run.py; BWC=$SP/testlab_bwc.py; CL=$SP/lab_clients.py
PZ=paulkilroy@192.168.0.105; SH=paulkilroy@192.168.0.109; SRV=paulkilroy@192.168.1.174
FREE="8c:86:dd:e3:78:f0 4e:fc:79:4c:c9:74 04:2e:c1:93:5e:03"
OUT=$(cd "$(dirname "$0")" && pwd)/results/ab3-$(date +%Y%m%d-%H%M); mkdir -p $OUT
PHONES="host 192.168.0.105 or host 192.168.0.106 or host 192.168.0.107"
omada() { for i in 1 2 3 4 5; do $PY $TL --set-mahal $1 $2 2>/dev/null | grep -q "^NOW.*'downLimitEnable': $3" && { echo "  AP limit $1/$2 confirmed"; return 0; }; sleep 30; done; echo "  ABORT: AP limit not confirmed"; exit 1; }
gateway() { $PY $BWC --rule-$1 "Lab 2/1" 2>/dev/null | grep -E "RULE SET|GLOBAL SET" | tr '\n' ' '; echo; }
shaper_off() { ssh $SH "sudo systemctl stop ellafi-shaper shaper 2>/dev/null; sleep 1; sudo python3 /usr/local/bin/shaper.py down >/dev/null 2>&1"; }
shaper_on() { shaper_off; ssh $SH "sudo systemd-run --unit=shaper --collect /usr/bin/python3 /usr/local/bin/shaper.py up --wan eno1 --lan enp4s0 --br br0 --down 2 --up 1 --target-ms 50 --interval-ms 300 --free $FREE >/dev/null"; }
sent() { ssh $SRV "cat /sys/class/net/eth0/statistics/tx_bytes" 2>/dev/null; }
$PY $BWC --group-ips 192.168.0.105,192.168.0.106,192.168.0.107,192.168.0.108 2>/dev/null | grep "GROUP SET"
for m in $SETUPS; do
  echo "$(date +%H:%M:%S) $m"
  case $m in
    open)    shaper_off; gateway off; omada 0 0 False ;;
    omada)   shaper_off; gateway off; omada 2048 1024 True ;;
    gateway) shaper_off; omada 0 0 False; gateway on ;;
    shaper)  gateway off; shaper_on; omada 4096 2048 True ;;
  esac
  sleep 200                                            # AP applies the change; controller counters reset/settle
  $PY $CL | grep pi02w | sort > $OUT/$m.omada_before.txt
  ssh $SH "sudo rm -f /var/tmp/caps/ab3_*; for p in eno1 enp4s0; do sudo systemd-run --unit=ab3-\$p --collect timeout $((MIN*60+30)) tcpdump -i \$p -s 96 -w /var/tmp/caps/ab3_\$p.pcap '$PHONES' >/dev/null 2>&1; done"
  s0=$(sent)
  ssh $PZ "cd farm; ~/qenv/bin/python scroll3.py --slots 8 --name p1 --minutes $MIN > /tmp/a1.json & \
           sudo ip vrf exec p3 /home/paulkilroy/qenv/bin/python scroll3.py --slots 8 --name p3 --minutes $MIN > /tmp/a3.json & \
           sudo ip vrf exec p4 /home/paulkilroy/qenv/bin/python scroll3.py --slots 8 --name p4 --minutes $MIN > /tmp/a4.json & wait; \
           cat /tmp/a1.json /tmp/a3.json /tmp/a4.json" > $OUT/$m.phones.jsonl
  s1=$(sent); echo "server_sent_bytes $((s1-s0))" > $OUT/$m.server.txt
  ssh $SH "while systemctl is-active -q ab3-eno1 || systemctl is-active -q ab3-enp4s0; do sleep 2; done; journalctl -u ab3-eno1 -u ab3-enp4s0 --no-pager | grep 'dropped by kernel' | tail -2" > $OUT/$m.capture_drops.txt
  scp -q $SH:/var/tmp/caps/ab3_eno1.pcap $OUT/$m.internet.pcap; scp -q $SH:/var/tmp/caps/ab3_enp4s0.pcap $OUT/$m.toap.pcap
  prev=""
  for i in 1 2 3 4; do
    sleep 150; cur=$($PY $CL | grep pi02w | sort); echo "$cur" > $OUT/$m.omada_after.txt
    [ "$cur" = "$prev" ] && break; prev=$cur
  done
  echo "  done: $(cat $OUT/$m.server.txt)"
done
gateway off; shaper_on; omada 4096 2048 True
echo "$(date +%H:%M:%S) all done -> $OUT"
