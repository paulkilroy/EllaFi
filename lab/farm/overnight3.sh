#!/bin/bash
# Second overnight block (Paul, 2026-11-09): bad phones (more delay), longer shaper waits, an overall ceiling, and
# 256 shaper lanes. Waits for overnight2.sh to finish, runs the plan, then again in reverse order, then summarizes.
HERE=/Users/paulkilroy/dev/EllaFi/lab/farm
while pgrep -f "lab/farm/overnight2.sh" >/dev/null || pgrep -f "lab/farm/ab3.sh" >/dev/null; do sleep 60; done
OUTD=$HERE/results/overnight3-$(date +%Y%m%d-%H%M); mkdir -p $OUTD
echo "omada 4096 2048" > $OUTD/.lab_state; echo off > $OUTD/.lab_state.gw      # how overnight2.sh leaves the lab
PLAN=(
  "weak-shaper50 shaper --profile weak"
  "weak-shaper100 shaper --profile weak --target-ms 100 --interval-ms 600"
  "weak-shaper200 shaper --profile weak --target-ms 200 --interval-ms 1000"
  "bad-shaper50 shaper --profile bad"
  "bad-shaper100 shaper --profile bad --target-ms 100 --interval-ms 600"
  "bad-shaper200 shaper --profile bad --target-ms 200 --interval-ms 1000"
  "ceiling4-good shaper --target-ms 100 --interval-ms 600 --total-down 4"
  "ceiling4-bad shaper --profile bad --target-ms 100 --interval-ms 600 --total-down 4"
  "lanes256 shaper --target-ms 100 --interval-ms 600 --fake-lanes 253"
  "weak-omada omada --profile weak"
  "bad-omada omada --profile bad"
)
for pass in 1 2; do
  if [ $pass = 1 ]; then idx=$(seq 0 $((${#PLAN[@]}-1))); else idx=$(seq $((${#PLAN[@]}-1)) -1 0); fi
  for i in $idx; do
    set -- ${PLAN[$i]}; label=$1-p$pass; setup=$2; shift 2
    bash $HERE/run2.sh $OUTD $label $setup 4 "$@" || echo "$(date +%H:%M:%S) [$label] FAILED" >> $OUTD/log.txt
  done
  /Users/paulkilroy/.platformio/penv/bin/python3 $HERE/overnight_summary.py $OUTD > $OUTD/summary.txt 2>&1
done
bash $HERE/run2.sh $OUTD restore-check shaper 1 >/dev/null 2>&1          # leaves: shaper 50 ms on, AP 4/2, good phones
echo "$(date +%H:%M:%S) ALL DONE -> $OUTD" | tee -a $OUTD/log.txt
