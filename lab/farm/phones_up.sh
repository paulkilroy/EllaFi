#!/bin/bash
# Bring up the 4 Wi-Fi phones on pi02w, each a separate Wi-Fi connection with its own MAC.
#   p1 = wlan0  (built-in, main table — also the SSH path, left untouched)
#   p2 = wlan0b (built-in, 2nd connection)  in vrf p2
#   p3 = wlan1  (USB MT7921AU)              in vrf p3
#   p4 = wlan1b (USB MT7921AU, 2nd conn)    in vrf p4
# The Mahal password is copied from NetworkManager's existing wlan0 connection, never printed.
# Run phones with:  sudo ip vrf exec pN python3 phone.py ...   (p1: plain python3 phone.py)
# Usage: phones_up.sh [p2 p3 p4]   (default all three)
set -e
SRC=$(nmcli -t -f NAME,DEVICE con show --active | awk -F: '$2=="wlan0"{print $1}')
declare -A DEV=([p2]=wlan0b [p3]=wlan1 [p4]=wlan1b) PARENT=([p2]=wlan0 [p4]=wlan1) TABLE=([p2]=1002 [p3]=1003 [p4]=1004)
declare -A MAC=([p2]=02:e1:1a:00:00:02 [p3]=02:e1:1a:00:00:03 [p4]=02:e1:1a:00:00:04)
modprobe vrf
for p in ${@:-p2 p3 p4}; do
  d=${DEV[$p]}
  [ -n "${PARENT[$p]}" ] && ! ip link show $d >/dev/null 2>&1 && /usr/sbin/iw dev ${PARENT[$p]} interface add $d type managed
  nmcli con show $p-vrf >/dev/null 2>&1 || nmcli con add type vrf con-name $p-vrf ifname $p table ${TABLE[$p]} ipv4.method disabled ipv6.method disabled >/dev/null
  nmcli con up $p-vrf >/dev/null
  if ! nmcli con show $p-wifi >/dev/null 2>&1; then
    nmcli con clone "$SRC" $p-wifi >/dev/null
    nmcli con mod $p-wifi connection.interface-name $d connection.controller $p connection.port-type vrf \
      802-11-wireless.cloned-mac-address ${MAC[$p]} connection.autoconnect yes
  fi
  nmcli con up $p-wifi >/dev/null && echo "$p up on $d: $(ip -br -4 addr show $d | awk '{print $3}') mac ${MAC[$p]}"
done
