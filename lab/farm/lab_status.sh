#!/bin/bash
# One-call lab check (no arguments, so it can be approved once): Pis, fake phones, shaper box, latest overnight log.
PZ=paulkilroy@192.168.0.105; SH=paulkilroy@192.168.0.109; SRV=paulkilroy@192.168.1.174
HERE=$(cd "$(dirname "$0")" && pwd)
echo "== Pi 4 (download server)"
ssh -o ConnectTimeout=5 $SRV "uptime -p; systemctl is-active quic-server farm-server | tr '\n' ' '; echo" 2>&1 | tail -2
echo "== Pi Zero (fake phones)"
ssh -o ConnectTimeout=5 $PZ "for p in p3 p4; do printf '%s ' \$p; sudo ip vrf exec \$p ping -c 1 -W 3 192.168.0.1 >/dev/null 2>&1 && echo ok || echo DOWN; done; \
  printf 'p1 '; ping -c 1 -W 3 192.168.0.1 >/dev/null && echo ok || echo DOWN" 2>&1 | tail -3
echo "== Shaper box"
ssh -o ConnectTimeout=5 $SH "uptime -p; for u in ellafi-shaper shaper; do printf '%s: ' \$u; systemctl is-active \$u; done; \
  echo lanes: \$(tc class show dev enp4s0 | grep -c 'prio 2')" 2>&1 | tail -4
D=$(ls -d $HERE/results/overnight3-* 2>/dev/null | tail -1)
if [ -n "$D" ]; then
  echo "== Overnight: $(basename $D)"
  tail -6 $D/log.txt 2>/dev/null
fi
