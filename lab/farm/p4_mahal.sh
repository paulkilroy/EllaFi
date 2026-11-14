#!/bin/bash
# Put fake phone p4 (USB dongle, 2nd connection) back on Mahal: recreate its connection, retry up to 3 times.
PZ=paulkilroy@192.168.0.105
for i in 1 2 3; do
  ssh $PZ "sudo nmcli con delete p4-wifi >/dev/null 2>&1; sudo iw dev wlan1b del 2>/dev/null; sleep 2; sudo bash farm/phones_up.sh p4" 2>&1 | grep -q "p4 up" && break
  sleep 10
done
ssh $PZ "ip -br -4 addr show wlan1b; sudo ip vrf exec p4 ping -c 2 -W 3 192.168.0.1 | tail -1"
