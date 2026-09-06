#!/bin/bash
# Definitive large-frame test. Needs root.
#  1. reloads the driver from the current build
#  2. turns wifi OFF for the duration so every byte must go over the Killer port
#     (both interfaces sit on the same subnet; with wifi up, replies can wander)
#  3. clears the card's sticky PCIe error bits and the MAC's event register
#  4. large ping (TX+RX of 1428-byte frames), a 10 MB HTTP download (RX-heavy),
#     and a burst of small pings
#  5. shows MAC event/counters, kernel netdev stats, PCIe error status + AER log
#  6. wifi back ON
set -u
HERE=$(dirname "$(readlink -f "$0")")
DEV=0000:07:00.0; R0=/sys/bus/pci/devices/$DEV/resource0
E=0x24000
trap 'nmcli radio wifi on >/dev/null 2>&1; echo "(wifi back on)"' EXIT

echo "==== load driver ===="
lsmod | grep -q '^killer_e2100' && rmmod killer_e2100 && sleep 1
insmod "$HERE/driver/killer_e2100.ko" "$@" || exit 1
sleep 1
IF=$(ls /sys/bus/pci/devices/$DEV/net/ 2>/dev/null | head -1)
[ -n "$IF" ] || { echo "no netdev"; exit 1; }
echo "==== wifi off, waiting for $IF address ===="
nmcli radio wifi off
ip link set "$IF" up
for i in $(seq 25); do ip -4 addr show "$IF" | grep -q 'inet ' && break; sleep 1; done
ip -br addr show "$IF"
IP=$(ip -4 -o addr show "$IF" | awk '{print $4}' | cut -d/ -f1)
GW=$(ip route show default | awk '{print $3; exit}')
[ -n "$GW" ] || { echo "no default route via $IF"; exit 1; }
arping -c 2 -U -I "$IF" "$IP" >/dev/null 2>&1 || true     # tell the gateway which MAC owns our IP now

echo; echo "==== clear sticky error state ===="
setpci -s $DEV CAP_EXP+0x0a.w=0x000f
setpci -s $DEV ECAP_AER+0x04.l=0xffffffff 2>/dev/null || true
setpci -s $DEV ECAP_AER+0x10.l=0xffffffff 2>/dev/null || true
"$HERE/poke" wr "$R0" $((E + 0x010)) 0xffffffff be >/dev/null
ctr() {
	for x in "0x010 IEVENT" "0x08c FIFO_TX_THR" "0x6a0 RPKT" "0x6a4 RFCS" "0x6d0 ROVR" "0x6dc RDRP" \
	         "0x6e4 TPKT" "0x714 TDRP" "0x728 TUND" "0x104 TSTAT" "0x304 RSTAT"; do
		set -- $x; printf "  %-12s %s\n" "$2" "$("$HERE/bar-peek" word "$R0" $((E + $1)) be)"
	done
}
echo "==== before ===="; ctr

echo; echo "==== A: large ping 1400 B -> $GW ===="
ping -c 4 -s 1400 -W 1 -I "$IF" "$GW" | tail -n 3
echo; echo "==== B: 10 MB HTTP download over $IF ===="
curl --interface "$IF" -s -o /dev/null --max-time 30 -w 'download: %{speed_download} bytes/s, http %{http_code}, %{size_download} bytes\n' \
	http://speedtest.tele2.net/10MB.zip || echo "curl failed"
echo; echo "==== C: 300 small pings ===="
ping -c 300 -i 0.005 -q -W 1 -I "$IF" "$GW" | tail -n 2

echo; echo "==== after ===="; ctr
ip -s -s link show "$IF" | sed -n '3,$p'
echo; echo "==== PCIe: card ===="
lspci -vvv -s "$DEV" | grep -E 'DevSta:|UESta:|CESta:|HeaderLog'
echo "==== PCIe: root port ===="
RP=$(basename "$(dirname "$(readlink -f /sys/bus/pci/devices/$DEV)")")
lspci -vvv -s "$RP" | grep -E 'DevSta:|UESta:|CESta:'
echo; echo "==== ethtool -S (non-zero) ===="; ethtool -S "$IF" | awk '$2 != 0'
echo; echo "==== dmesg ===="; dmesg | grep -iE 'killer|enp7|DMA bus|aer' | tail -n 8
