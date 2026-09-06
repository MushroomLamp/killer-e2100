#!/bin/bash
# Why is a single TCP download slow when the link, the MAC and the DMA are clean?
# Runs one download and reads the kernel's own TCP/IP counters around it:
# checksum failures, out-of-order segments, receive-queue drops, window state.
# Then repeats with receive-buffer autotuning disabled and a large fixed buffer,
# which tells us whether the receiver's advertised window is the limiter.
# Args pass through to insmod. Needs root.
set -u
HERE=$(dirname "$(readlink -f "$0")")
DEV=0000:07:00.0
URL=http://speedtest.tele2.net/10MB.zip
RMEM=$(sysctl -n net.ipv4.tcp_rmem); MOD=$(sysctl -n net.ipv4.tcp_moderate_rcvbuf)
trap 'sysctl -qw net.ipv4.tcp_rmem="$RMEM" net.ipv4.tcp_moderate_rcvbuf="$MOD"; nmcli radio wifi on >/dev/null 2>&1; echo "(sysctls restored, wifi back on)"' EXIT

lsmod | grep -q '^killer_e2100' && rmmod killer_e2100 && sleep 1
insmod "$HERE/driver/killer_e2100.ko" "$@" || exit 1
sleep 1
IF=$(ls /sys/bus/pci/devices/$DEV/net/ 2>/dev/null | head -1)
nmcli radio wifi off
ip link set "$IF" up || { dmesg | grep -iE 'wdma' | tail -n 6; exit 1; }
for i in $(seq 25); do ip -4 addr show "$IF" | grep -q 'inet ' && break; sleep 1; done
IP=$(ip -4 -o addr show "$IF" | awk '{print $4}' | cut -d/ -f1)
[ -n "$IP" ] || { echo "no address"; dmesg | grep -iE 'wdma' | tail -n 6; exit 1; }
echo "== $IF $IP  $(dmesg | grep -E "$IF: Link is Up" | tail -1 | grep -oE '[0-9]+[GM]bps/[A-Za-z]+')  poll_us=$(cat /sys/module/killer_e2100/parameters/poll_us) =="
mbps() { awk -v b="$1" -v s="$2" 'BEGIN{printf "%.1f Mbit/s", b*8/s/1e6}'; }

run() {   # $1 = label
	nstat -n >/dev/null 2>&1
	curl -s --interface "$IF" --max-time 12 -o /dev/null -w '%{size_download} %{time_total}' "$URL" > /tmp/kl_curl &
	sleep 5
	echo "-- $1: socket state mid-transfer (our side is the receiver) --"
	ss -tin "src $IP" | grep -A1 -E '^ESTAB' | grep -vE '^--' | sed 's/^/   /' | cut -c1-230 | head -n 6
	wait
	read sz el < /tmp/kl_curl
	echo "   download: $(mbps ${sz:-0} ${el:-12})   ($sz bytes)"
	echo "   kernel counters during the transfer:"
	nstat 2>/dev/null | grep -E 'TcpInSegs|TcpOutSegs|TcpInCsumErrors|TcpInErrs|IpInHdrErrors|IpInDiscards|IpInReceives|TcpExtTCPOFOQueue|TcpExtTCPOFOMerge|TcpExtTCPDSACKOfoSent|TcpExtTCPDSACKRecv|TcpExtTCPSACKReorder|TcpExtTCPRcvQDrop|TcpExtPruneCalled|TcpExtTCPRcvCollapsed|TcpExtTCPBacklogDrop|TcpExtTCPRcvCoalesce|TcpExtTCPTimeouts|TcpRetransSegs|TcpExtDelayedACKs|TcpExtTCPHPHits|TcpExtTCPPureAcks|TcpExtTCPDSACKOldSent|TcpExtTCPAbort' | awk '{printf "   %-28s %s\n",$1,$2}'
}

run "A: defaults"
echo
sysctl -qw net.ipv4.tcp_moderate_rcvbuf=0 net.ipv4.tcp_rmem="4096 6291456 6291456"
run "B: autotuning off, 6 MB receive buffer"
echo
echo "-- driver counters --"
ethtool -S "$IF" | grep -E 'wdma|overrun|rx_dropped' | sed 's/^/   /'
ip -s link show "$IF" | sed -n '4,5p' | sed 's/^/   /'
