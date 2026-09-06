#!/bin/bash
# Throughput characterisation for the Killer port. Needs root.
# Args are passed straight to insmod (e.g. gigabit=1 poll_us=100).
# Turns wifi off for the run so nothing leaks onto the wireless path.
set -u
HERE=$(dirname "$(readlink -f "$0")")
DEV=0000:07:00.0
URL=http://speedtest.tele2.net/10MB.zip
UPURL=http://speedtest.tele2.net/upload.php
trap 'nmcli radio wifi on >/dev/null 2>&1; echo "(wifi back on)"' EXIT

lsmod | grep -q '^killer_e2100' && rmmod killer_e2100 && sleep 1
insmod "$HERE/driver/killer_e2100.ko" "$@" || exit 1
sleep 1
IF=$(ls /sys/bus/pci/devices/$DEV/net/ 2>/dev/null | head -1)
[ -n "$IF" ] || { echo "no netdev (self-test may have failed; dmesg):"; dmesg|grep -i wdma|tail -3; exit 1; }
nmcli radio wifi off
ip link set "$IF" up
for i in $(seq 25); do ip -4 addr show "$IF" | grep -q 'inet ' && break; sleep 1; done
IP=$(ip -4 -o addr show "$IF" | awk '{print $4}' | cut -d/ -f1)
[ -n "$IP" ] || { echo "no address on $IF; dmesg:"; dmesg | grep -iE 'wdma|enp7s0' | tail -n 8; exit 1; }
GW=$(ip route show default | awk '{print $3; exit}')
POLL=$(cat /sys/module/killer_e2100/parameters/poll_us)
GBIT=$(cat /sys/module/killer_e2100/parameters/gigabit)
LINK=$(dmesg | grep -E "$IF: Link is Up" | tail -1 | grep -oE '[0-9]+[GM]bps/[A-Za-z]+')
echo "== $IF  ip $IP  gw $GW  link $LINK  poll_us=$POLL gigabit=$GBIT =="
c0=$(ethtool -S "$IF" | awk '/wdma_chains/{print $2}')
rdrp0=$(ethtool -S "$IF" | awk '/mac_rx_dropped/{print $2}')

mbps() { awk -v b="$1" -v s="$2" 'BEGIN{printf "%.1f Mbit/s (%.2f MB/s)", b*8/s/1e6, b/s/1048576}'; }

echo "-- 1. single-stream download, 15 s cap --"
t=$( curl -s --interface "$IF" --max-time 15 -o /dev/null -w '%{size_download} %{time_total}' "$URL" 2>/dev/null )
sz=$(echo "$t"|awk '{print $1}'); el=$(echo "$t"|awk '{print $2}'); echo "   $(mbps ${sz:-0} ${el:-15})"

echo "-- 2. six parallel downloads, 15 s cap --"
start=$(date +%s.%N)
tmp=$(mktemp -d)
for n in $(seq 6); do
  ( curl -s --interface "$IF" --max-time 15 -o /dev/null -w '%{size_download}\n' "$URL" > "$tmp/$n" ) &
done
wait
end=$(date +%s.%N)
tot=$(cat "$tmp"/* | awk '{s+=$1} END{print s}')
el=$(awk -v a="$start" -v b="$end" 'BEGIN{print b-a}')
rm -rf "$tmp"
echo "   aggregate $(mbps ${tot:-0} ${el:-15})"

echo "-- 3. single-stream upload, 15 s cap --"
t=$( curl -s --interface "$IF" --max-time 15 -o /dev/null -w '%{size_upload} %{time_total}' -T <(head -c 50000000 /dev/zero) "$UPURL" 2>&1 )
sz=$(echo "$t"|awk '{print $1}'); el=$(echo "$t"|awk '{print $2}'); echo "   $(mbps ${sz:-0} ${el:-15})"

c1=$(ethtool -S "$IF" | awk '/wdma_chains/{print $2}')
to=$(ethtool -S "$IF" | awk '/wdma_timeouts/{print $2}')
we=$(ethtool -S "$IF" | awk '/wdma_errors/{print $2}')
rdrp1=$(ethtool -S "$IF" | awk '/mac_rx_dropped/{print $2}')
echo "== wdma chains +$((c1-c0)) timeouts $to errors $we | mac_rx_dropped +$((rdrp1-rdrp0)) =="
ov=$(ethtool -S "$IF" | awk '/bd_rx_overrun/{print $2}'); echo "== bd_rx_overrun total ${ov:-0} =="
