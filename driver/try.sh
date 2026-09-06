#!/bin/bash
# Load the driver, wait for an interface, ping the gateway through it.
# Unload afterwards with: sudo rmmod killer_e2100
set -u
HERE=$(dirname "$(readlink -f "$0")")
DEV=0000:07:00.0
if lsmod | grep -q '^killer_e2100'; then echo "unloading the old copy first"; rmmod killer_e2100; sleep 1; fi
insmod "$HERE/killer_e2100.ko" || exit 1
sleep 1
echo "==== dmesg ===="; dmesg | grep -iE 'killer|e2100|enp7|eth[0-9]|marvell|88E1116' | tail -n 10
IF=$(ls /sys/bus/pci/devices/$DEV/net/ 2>/dev/null | head -1)
[ -n "$IF" ] || { echo "no netdev bound to $DEV"; exit 1; }
echo; echo "==== interface: $IF ===="
ip link set "$IF" up
echo "waiting up to 20 s for an address (NetworkManager/DHCP)..."
for i in $(seq 20); do ip -4 addr show "$IF" | grep -q 'inet ' && break; sleep 1; done
ip -br addr show "$IF"
GW=$(ip route show default | awk '{print $3; exit}')
if ip -4 addr show "$IF" | grep -q 'inet '; then
	echo; echo "==== ping gateway $GW via $IF ===="
	ping -c 4 -I "$IF" "$GW"
else
	echo; echo "no address yet. give it one by hand, e.g.:"
	echo "  sudo ip addr add 192.168.1.250/24 dev $IF && ping -c 4 -I $IF $GW"
fi
echo; echo "==== ethtool ===="; ethtool "$IF" 2>/dev/null | grep -E "Speed|Duplex|Auto-negotiation|Link detected"; ethtool -i "$IF" 2>/dev/null | head -3
echo; echo "==== stats ===="; ip -s link show "$IF"
echo; dmesg | grep -iE 'killer|$IF' | tail -n 4
