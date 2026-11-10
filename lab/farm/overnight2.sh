#!/bin/bash
# Overnight sim-lab sweep (Paul, 2026-11-09: "run simulations all night ... only the sim lab, not Bakhaw").
# Waits for any running ab3.sh to finish, then runs the plan below twice (second pass in reverse order, to check
# runs repeat and to spread any slow drift), then writes the summary. Runs grouped by AP limit so Omada changes
# (slow to apply) are few. Every run: lab/farm/run1.sh (3 Pi phones, 4 min, all bytes counted with headers).
HERE=/Users/paulkilroy/dev/EllaFi/lab/farm
OUTD=$HERE/results/overnight-$(date +%Y%m%d-%H%M); mkdir -p $OUTD
while pgrep -f "lab/farm/ab3.sh" >/dev/null; do sleep 30; done
echo "omada 4096 2048" > $OUTD/.lab_state; echo off > $OUTD/.lab_state.gw     # how ab3.sh leaves the lab
PLAN=(
  "shaper50-a shaper"  "shaper50-b shaper"  "shaper50-c shaper"
  "shaper10 shaper --target-ms 10"  "shaper20 shaper --target-ms 20"
  "shaper100 shaper --target-ms 100"  "shaper200 shaper --target-ms 200 --interval-ms 600"
  "shaper50-burst512 shaper --burst-kb 512"
  "shaper50-slots4 shaper --slots 4"
  "shaper50-watch2 shaper --watch-s 2"  "shaper50-watch10 shaper --watch-s 10"
  "shaper50-cubic shaper --cc 1"  "shaper50-bbr3 shaper --cc 5"
  "shaper50-delay80 shaper --delay 80"
  "omada-a omada"  "omada-b omada"  "omada-c omada"
  "omada-slots4 omada --slots 4"
  "omada-watch2 omada --watch-s 2"  "omada-watch10 omada --watch-s 10"
  "omada-cubic omada --cc 1"  "omada-bbr3 omada --cc 5"
  "omada-delay80 omada --delay 80"
  "open-a open"  "open-b open"  "open-watch10 open --watch-s 10"
  "gateway-a gateway"  "gateway-b gateway"
)
for pass in 1 2; do
  if [ $pass = 1 ]; then idx=$(seq 0 $((${#PLAN[@]}-1))); else idx=$(seq $((${#PLAN[@]}-1)) -1 0); fi
  for i in $idx; do
    set -- ${PLAN[$i]}; label=$1-p$pass; setup=$2; shift 2
    bash $HERE/run1.sh $OUTD $label $setup 4 "$@" || echo "$(date +%H:%M:%S) [$label] FAILED" >> $OUTD/log.txt
  done
  /Users/paulkilroy/.platformio/penv/bin/python3 $HERE/overnight_summary.py $OUTD > $OUTD/summary.txt 2>&1
done
# leave the lab as found: gateway off, shaper on (50 ms), AP 4/2, server BBRv1 at 40 ms
bash $HERE/run1.sh $OUTD restore-check shaper 1 >/dev/null 2>&1
echo "$(date +%H:%M:%S) ALL DONE -> $OUTD" | tee -a $OUTD/log.txt
