#!/bin/bash
# Whole-board validation suite: static checkers + ngspice scenario benches.
# Limit lines: "meas_name min max" (-inf/inf for one-sided, "info" to just report).
cd "$(dirname "$0")"
fail=0

echo "== check_polarity.py (pad-1 nets vs datasheet intent) =="
python3 check_polarity.py | sed 's/^/  /' | grep -v '^  PASS' || true
python3 check_polarity.py > /dev/null || fail=1

echo "== check_sim_sync.py (board.inc vs fresh schematic netlist) =="
python3 check_sim_sync.py | sed 's/^/  /' | grep -v '^  PASS' || true
python3 check_sim_sync.py > /dev/null || fail=1

check() {  # $1=file $2=limits
  local out
  out=$(ngspice -b "$1" 2>/dev/null)
  echo "== $1 =="
  while read -r name lo hi; do
    [ -z "$name" ] && continue
    local val
    val=$(echo "$out" | awk -v n="$name" '$1==n && $2=="=" {print $3; exit}')
    if [ "$lo" = "info" ]; then printf "  INFO %-16s = %s\n" "$name" "${val:-n/a}"; continue; fi
    if [ -z "$val" ]; then echo "  FAIL $name: no result (sim error?)"; fail=1; continue; fi
    local ok
    ok=$(echo "$val $lo $hi" | awk '{print ($1>=$2 && $1<=$3) ? 1 : 0}')
    if [ "$ok" = "1" ]; then printf "  PASS %-16s = %-12s [%s .. %s]\n" "$name" "$val" "$lo" "$hi"
    else printf "  FAIL %-16s = %-12s [%s .. %s]\n" "$name" "$val" "$lo" "$hi"; fail=1; fi
  done <<< "$2"
}

check tb_powerup.cir "
v5_final 4.85 5.15
v33_final 3.25 3.35
v5_min 4.75 inf
v5_max -inf 5.5
rail12_min 10.5 inf
i_steady_max -inf 1.5
i_inrush_pk info"

check tb_coin_pulse.cir "
g4_low -inf 0.4
g4_high 3.0 inf
iled_on 0.006 0.015
igrn_on 0.0008 0.005
width 0.045 0.055"

check tb_coin_fault.cir "
g4_min_noise 0.825 inf
coin_pk_fault -inf 9
g4_max_fault -inf 3.6
g4_min_fault -0.3 inf
opto_vrev_pk -inf 6
d1_i_pk info"

check tb_enable_channel.cir "
vds_on_max -inf 0.1
i_lamp_on 0.07 0.10
i_d4_on 0.004 0.010
g4_low_coin -inf 0.4
blink_off 11 inf
d4_vrev_off -inf 5"

check tb_inductive.cir "
vds_pk -inf 45
vds_pk_noacc -inf 45
i_aval_pk info
d4_vrev_pk -inf 5"

check tb_hotplug.cir "
rail12_pk -inf 24
rail12_min 6 inf
tvs_i_pk info"

check tb_reverse.cir "
rail12_min -1.2 inf
i_fault info"

check tb_fuse_trip.cir "
t_trip 0.001 1.0
d1_i_posttrip -inf 0.01
d1_i_during info"

check tb_opto_aging.cir "
g4_low_ctr100 -inf 0.4
g4_low_ctr50 -inf 0.4
g4_low_ctr30 -inf 0.825"

check tb_panel3wire.cir "
g14_boot_pk -inf 3.6
g14_after 3.0 3.6
vb_panel info
vpanel_col info
blink_boot info"

echo "== tb_montecarlo.cir (60 trials, R-tolerances + opto CTR bin) =="
ngspice -b tb_montecarlo.cir 2>/dev/null | awk '
  $1=="MC" { n++
    if ($2<4.85 || $2>5.15) v5bad++
    if ($3>0.8) g4bad++
    if ($2<v5lo || !v5lo) v5lo=$2;  if ($2>v5hi) v5hi=$2
    if ($3>g4worst) g4worst=$3 }
  END {
    printf "  INFO trials=%d  v5 range [%.3f .. %.3f]  g4 worst %.3f\n", n, v5lo, v5hi, g4worst
    if (n<60) { print "  FAIL montecarlo: only " n " trials ran"; exit 1 }
    if (v5bad) { print "  FAIL " v5bad " trials outside 5V limits"; exit 1 }
    if (g4bad) { print "  FAIL " g4bad " trials with weak coin logic-low (>0.8V) — low-CTR optos; fixed by R1/R5 change"; exit 1 }
    print "  PASS all trials in limits" }' || fail=1

echo
[ $fail = 0 ] && echo "ALL PASS" || echo "FAILURES PRESENT (see README.md — open items are documented)"
exit $fail
