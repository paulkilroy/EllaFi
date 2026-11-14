#!/bin/bash
# Measure kernel memory for N shaper lanes (HTB class + fq_codel each) on a throwaway dummy interface.
# Run on the box: sudo bash lane_mem_test.sh 500
N=${1:-500}
avail() { awk '/MemAvailable/ {print $2}' /proc/meminfo; }
sync; echo 3 > /proc/sys/vm/drop_caches; sleep 1
ip link add lmt0 type dummy; ip link set lmt0 up
B=$(avail)
{
  echo "qdisc add dev lmt0 root handle 1: htb default 2"
  echo "class add dev lmt0 parent 1: classid 1:1 htb rate 1gbit"
  for i in $(seq 10 $((N + 9))); do
    echo "class add dev lmt0 parent 1:1 classid 1:$i htb rate 2mbit ceil 2mbit"
    echo "qdisc add dev lmt0 parent 1:$i fq_codel target 200ms interval 1000ms"
  done
} > /tmp/lmt.batch
tc -batch /tmp/lmt.batch
sleep 1; A=$(avail)
echo "$N lanes: $(( (B - A) / 1024 )) MB ($(( (B - A) / N )) KB per lane)"
ip link del lmt0; rm -f /tmp/lmt.batch
