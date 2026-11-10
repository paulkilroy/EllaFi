#!/bin/bash
# Limiter A/B from the Mac: each limit x 2 passes (mirrored order) x N min, all 3 phones on pi02w.
#   omada  = lab Mahal client rate limit 2048/1024 Kbps (the AP's limiter), phones unlimited
#   open   = no limit anywhere
#   police = drop-over-rate at each phone's interface (limit.sh police), Omada off
#   shape  = queue (HTB + fq_codel) at each phone's interface (limit.sh shape), Omada off
# Also counts the download server's own TCP resends per run (pi4 /proc/net/snmp): that is the data a
# limiter threw away after it had already crossed the uplink. Lines go to $RES.
# Leaves Mahal's limit OFF at the end. Phone results land in pi02w:farm/results/.
# Usage: overnight.sh <testlab.py path> [minutes] [modes...]
TL=${1:?testlab.py}; MIN=${2:-5}; shift 2; MODES=${@:-omada open police shape}
PY=~/.platformio/penv/bin/python3; PI=paulkilroy@pi02w.local; SRV=paulkilroy@192.168.1.174
RES=$(dirname "$0")/server_resends.txt
omada() { for i in 1 2 3; do $PY "$TL" --set-mahal $1 $2 | grep -q "^SET 0" && return; sleep 30; done; echo "omada set FAILED"; }
tcp() { ssh $SRV "awk '/^Tcp:/ {l=\$0} END {split(l, f, \" \"); print f[12], f[13]}' /proc/net/snmp"; }   # OutSegs RetransSegs
ORDER="$MODES $(echo $MODES | tr ' ' '\n' | tail -r | tr '\n' ' ')"
seen=" "
for m in $ORDER; do
  pass=$(( $(echo "$seen" | tr " " "\n" | grep -c "^$m$") + 1 )); seen="$seen $m"
  echo "$(date +%H:%M) $m pass $pass"
  if [ $m = omada ]; then omada 2048 1024; else omada 0 0; fi
  ssh $PI "sudo bash farm/limit.sh $([ $m = omada ] && echo open || echo $m)" >/dev/null
  sleep 45                                  # let the AP apply the setting and Wi-Fi settle
  read o0 r0 <<< "$(tcp)"
  ssh $PI "sudo bash farm/run.sh $m-$pass $MIN >/dev/null; sudo bash farm/limit.sh stats | sudo tee farm/results/$m-$pass.tc >/dev/null"
  read o1 r1 <<< "$(tcp)"
  echo "$m-$pass server sent $((o1-o0)) packets, resent $((r1-r0)) = $(awk "BEGIN {printf \"%.2f\", 100*($r1-$r0)/($o1-$o0)}")%" | tee -a $RES
done
omada 0 0; ssh $PI "sudo bash farm/limit.sh open"
echo "$(date +%H:%M) done"
