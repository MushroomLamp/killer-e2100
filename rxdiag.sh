#!/bin/bash
# Why does a TCP download deliver nothing while pings and uploads work?
# Three independent views of the receive path, with the driver loaded:
#   A. self-test and bus state from dmesg
#   B. flood of 1400-byte pings: ping itself verifies every reply's payload,
#      so loss or "wrong data byte" here means RX corruption without any TCP
#   C. a 10-second download with the kernel's TCP/IP counters around it:
#      TcpInCsumErrors = frames arrived corrupted; IpInReceives/TcpInSegs vs the
#      MAC's own count = frames never reached the stack; OFO = reordering
# Args pass to insmod. Needs root.
set -u
HERE=$(dirname "$(readlink -f "$0")")
DEV=0000:07:00.0
URL='https://speed.cloudflare.com/__down?bytes=20000000'
trap 'nmcli radio wifi on >/dev/null 2>&1; echo "(wifi back on)"' EXIT
lsmod | grep -q '^killer_e2100' && rmmod killer_e2100 && sleep 1
insmod "$HERE/driver/killer_e2100.ko" "$@" || exit 1
sleep 1
IF=$(ls /sys/bus/pci/devices/$DEV/net/ 2>/dev/null | head -1)
nmcli radio wifi off
ip link set "$IF" up || { dmesg | grep -iE 'wdma' | tail -n 8; exit 1; }
for i in $(seq 25); do ip -4 addr show "$IF" | grep -q 'inet ' && break; sleep 1; done
IP=$(ip -4 -o addr show "$IF" | awk '{print $4}' | cut -d/ -f1)
[ -n "$IP" ] || { echo "no address"; dmesg | grep -iE 'wdma' | tail -n 8; exit 1; }
GW=$(ip route show default | awk '{print $3; exit}')
SRV=$(getent ahostsv4 speed.cloudflare.com | awk '{print $1; exit}')
echo "== $IF $IP  gw $GW  server $SRV  $(dmesg | grep -E "$IF: Link is Up" | tail -1 | grep -oE '[0-9]+[GM]bps/[A-Za-z]+') =="
echo "== A. self-test / bus =="
dmesg | grep -E 'wdma|CSB arbiter|bus: SPCR' | tail -n 8 | cut -c1-170 | sed 's/^/   /'
mrx() { ethtool -S "$IF" | awk -v k="$1" '$1==k":"{print $2}'; }
nrx() { ip -s link show "$IF" | awk 'f&&NR==f+1{print $2; exit} /RX:/{f=NR}'; }

echo "== B. flood ping, 1400-byte payload, 3000 packets (payload verified by ping) =="
m0=$(mrx mac_rx_packets); n0=$(nrx)
ping -f -s 1400 -c 3000 -W 2 -I "$IF" "$GW" 2>&1 | tail -n 2 | sed 's/^/   /'
echo "   MAC saw +$(( $(mrx mac_rx_packets) - m0 )) frames, driver delivered +$(( $(nrx) - n0 )) packets, bd errors now $(mrx bd_rx_overrun)"

echo "== C. 10 s TCP download with kernel counters =="
nstat -n >/dev/null 2>&1
m0=$(mrx mac_rx_packets); n0=$(nrx)
curl -s --interface "$IF" --max-time 10 -o /dev/null -w '%{size_download} %{time_total} %{http_code}' "$URL" > /tmp/kl_c &
sleep 5
echo "   socket at 5 s:"; ss -tin "src $IP and dst $SRV" | grep -A1 ESTAB | grep -v ESTAB | sed 's/^/     /' | fold -w 190 | head -n 6
wait
read sz el code < /tmp/kl_c
awk -v b="${sz:-0}" -v s="${el:-10}" -v c="${code:-?}" 'BEGIN{printf "   download: %.1f Mbit/s, %d bytes, http %s\n", b*8/s/1e6, b, c}'
echo "   MAC saw +$(( $(mrx mac_rx_packets) - m0 )) frames, driver delivered +$(( $(nrx) - n0 )) packets"
echo "   kernel counters:"
nstat 2>/dev/null | grep -E 'IpInReceives|IpInHdrErrors|IpInDiscards|IpInDelivers|TcpInSegs|TcpOutSegs|TcpInCsumErrors|TcpInErrs|TcpRetransSegs|TcpExtTCPOFOQueue|TcpExtTCPRcvQDrop|TcpExtTCPBacklogDrop|TcpExtPruneCalled|TcpExtTCPAbort|TcpExtTCPDSACKOfoSent|TcpExtTCPDSACKOldSent|TcpExtTCPSACKReorder|TcpExtTCPRcvCoalesce|UdpInCsumErrors|IpExtInCsumErrors|TcpExtTCPTimeouts' | awk '{printf "     %-28s %s\n",$1,$2}'
echo "== driver counters =="; ethtool -S "$IF" | awk '$2 != 0 && !/mac_(rx|tx)_bytes/' | sed 's/^/   /'
