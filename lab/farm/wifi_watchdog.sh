#!/bin/bash
# pi02w: the built-in Wi-Fi (wlan0 — p1 and the only SSH path) can stay off after the AP restarts its radios
# (Nov 14: an SSID was added, wlan0 never rejoined, needed a power cycle). Every minute (systemd timer):
# if the gateway hasn't answered for 2 checks in a row, restart wlan0's connection; after 5, reload the driver;
# then bring the fake phones on the USB dongle back (phones_up.sh p3 p4).
# Install: wifi_watchdog.sh install
GW=192.168.0.1; STATE=/run/wifi_watchdog.fails; HERE=/home/paulkilroy/farm

if [ "$1" = install ]; then
  sudo cp "$0" /usr/local/bin/wifi_watchdog.sh
  sudo tee /etc/systemd/system/wifi-watchdog.service >/dev/null <<'EOF'
[Unit]
Description=Restart Wi-Fi when the gateway stops answering
[Service]
Type=oneshot
ExecStart=/usr/local/bin/wifi_watchdog.sh
EOF
  sudo tee /etc/systemd/system/wifi-watchdog.timer >/dev/null <<'EOF'
[Unit]
Description=Wi-Fi watchdog every minute
[Timer]
OnBootSec=3min
OnUnitActiveSec=1min
[Install]
WantedBy=timers.target
EOF
  sudo systemctl daemon-reload && sudo systemctl enable --now wifi-watchdog.timer
  exit
fi

if ping -c 2 -W 3 -I wlan0 $GW >/dev/null 2>&1; then
  echo 0 > $STATE
  # the dongle phones: bring back any that are down (cheap check, only when wlan0 is fine)
  for p in p3 p4; do
    ip vrf exec $p ping -c 1 -W 3 $GW >/dev/null 2>&1 || bash $HERE/phones_up.sh $p >/dev/null 2>&1
  done
  exit 0
fi
n=$(( $(cat $STATE 2>/dev/null || echo 0) + 1 )); echo $n > $STATE
logger -t wifi-watchdog "gateway not answering on wlan0 ($n)"
if [ $n -ge 5 ]; then
  logger -t wifi-watchdog "reloading brcmfmac"
  modprobe -r brcmfmac && sleep 2 && modprobe brcmfmac; sleep 15
  echo 2 > $STATE
elif [ $n -ge 2 ]; then
  nmcli con up "$(nmcli -t -f NAME,DEVICE con show | awk -F: '$2=="wlan0"{print $1; exit}')" >/dev/null 2>&1 || nmcli dev connect wlan0
fi
