#!/bin/bash
# Sweep the lab QUIC server's congestion control (lsquic -A: 1 Cubic, 2 BBRv1, 4 BBRv1-Copilot, 5 BBRv3) through the
# same calibration (Omada 2/1, shaper off, one Pi phone) to find which pushes into the limit most like Meta's
# servers did (steady 9-11 Mbps arriving at the AP). Leaves the server on the last algorithm given.
# Usage: calibrate_cc.sh <testlab.py> [minutes] [algos...]
TL=${1:?testlab.py}; MIN=${2:-3}; shift 2; ALGOS=${@:-1 4 5}
SRV=paulkilroy@192.168.1.174; HERE=$(cd "$(dirname "$0")" && pwd)
for A in $ALGOS; do
  ssh $SRV "sudo sed -i 's/ -A [0-9] / -A $A /' /etc/systemd/system/quic-server.service && sudo systemctl daemon-reload && sudo systemctl restart quic-server && systemctl is-active quic-server && grep -o ' -A [0-9] ' /etc/systemd/system/quic-server.service" 2>&1 | grep -v setlocale
  echo "=== algo $A"
  bash $HERE/calibrate.sh "$TL" $MIN | tail -2
done
