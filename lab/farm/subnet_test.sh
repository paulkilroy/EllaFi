#!/bin/bash
# Per-address lanes test: shaper with --subnets 192.168.98.0/24 --vlans 98 (all 254 lanes made at start), fake phone
# p4 on Lab98 (VLAN 98) and p3 on Mahal scroll together for 3 min. Expect p4 held to 2 Mbps from its first packet,
# p3 uncapped. Then p4 back to Mahal and the boot-service shaper back on.
SH=paulkilroy@192.168.0.109; PZ=paulkilroy@192.168.0.105
HERE=$(cd "$(dirname "$0")" && pwd); OUT=$HERE/results/subnet-$(date +%Y%m%d-%H%M); mkdir -p $OUT
KEY=/private/tmp/claude-501/-Users-paulkilroy-dev-EllaFi/3f0cb27a-5c85-4227-80ca-0bf768b2dd08/scratchpad/lab98.key
scp -q $HERE/../shaper/shaper.py $SH:/tmp/shaper.py
ssh $SH "sudo cp /tmp/shaper.py /usr/local/bin/shaper.py; sudo systemctl stop ellafi-shaper shaper 2>/dev/null; sleep 1; \
  sudo python3 /usr/local/bin/shaper.py down >/dev/null; \
  sudo systemd-run --unit=shaper --collect /usr/bin/python3 /usr/local/bin/shaper.py up --wan eno1 --lan enp4s0 --br br0 \
    --down 2 --up 1 --target-ms 200 --interval-ms 1000 --vlans 98 --subnets 192.168.98.0/24 >/dev/null; sleep 15; \
  systemctl is-active shaper; journalctl -u shaper --no-pager -n 3 | grep -E 'shaper up|Error|error'; \
  echo lanes: \$(tc class show dev enp4s0 | grep -c 'prio 2'); sudo nft list chain bridge ellafi mark_lanes | grep mark" | tee $OUT/setup.txt
ssh $PZ 'read -r K; sudo nmcli con mod p4-wifi 802-11-wireless.ssid "EllaFi Lab98" wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$K" && sudo nmcli con up p4-wifi >/dev/null; ip -br -4 addr show wlan1b' < $KEY | tee -a $OUT/setup.txt
ssh $PZ "cd farm; sudo ip vrf exec p3 /home/paulkilroy/qenv/bin/python scroll3.py --slots 8 --name p3-mahal --minutes 3 > /tmp/v3.json & \
  sudo ip vrf exec p4 /home/paulkilroy/qenv/bin/python scroll3.py --slots 8 --name p4-vlan98 --minutes 3 > /tmp/v4.json & wait; cat /tmp/v3.json /tmp/v4.json" > $OUT/phones.jsonl
ssh $SH "sudo python3 /usr/local/bin/shaper.py stats" > $OUT/stats.json
/Users/paulkilroy/.platformio/penv/bin/python3 - $OUT <<'EOF'
import json, sys
o = sys.argv[1]
for l in open(o + "/phones.jsonl"):
    if l.startswith("{"):
        r = json.loads(l)
        print(r["name"], "MB", r["mb_received"], "=", round(r["mb_received"] * 8 / 180, 2), "Mbps avg · ready",
              r["ready_at_swipe_pct"], "% · freezes", r["videos_with_freeze"])
s = json.load(open(o + "/stats.json"))
busy = {k: v for k, v in s["lanes"].items() if v.get("down", {}).get("bytes")}
print("lanes with traffic:", {k: (round(v["down"]["bytes"] / 1e6, 1), "MB") for k, v in busy.items()})
EOF
ssh $PZ "sudo nmcli con delete p4-wifi >/dev/null; sudo bash farm/phones_up.sh p4"
ssh $SH "sudo systemctl stop shaper; sudo python3 /usr/local/bin/shaper.py down >/dev/null; sudo systemctl start ellafi-shaper; systemctl is-active ellafi-shaper"
