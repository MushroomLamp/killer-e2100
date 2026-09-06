#!/bin/bash
# Where does the time go on a single TCP download? Capture the wire on the
# Killer port during a download and measure: data inter-arrival gaps, how long
# we take to ACK, dup-ACKs, retransmissions, windows. Plus ICMP RTT to the
# server and the full socket state. Runs once with GRO on and once with it off.
# Args pass to insmod. Needs root.
set -u
HERE=$(dirname "$(readlink -f "$0")")
DEV=0000:07:00.0
HOST=speed.cloudflare.com
URL='https://speed.cloudflare.com/__down?bytes=50000000'
trap 'nmcli radio wifi on >/dev/null 2>&1; echo "(wifi back on)"' EXIT

lsmod | grep -q '^killer_e2100' && rmmod killer_e2100 && sleep 1
insmod "$HERE/driver/killer_e2100.ko" "$@" || exit 1
sleep 1
IF=$(ls /sys/bus/pci/devices/$DEV/net/ 2>/dev/null | head -1)
nmcli radio wifi off
ip link set "$IF" up || { dmesg | grep -i wdma | tail -n 6; exit 1; }
for i in $(seq 25); do ip -4 addr show "$IF" | grep -q 'inet ' && break; sleep 1; done
IP=$(ip -4 -o addr show "$IF" | awk '{print $4}' | cut -d/ -f1)
[ -n "$IP" ] || { echo "no address"; dmesg | grep -i wdma | tail -n 6; exit 1; }
SRV=$(getent ahostsv4 $HOST | awk '{print $1; exit}')
echo "== $IF $IP -> $HOST ($SRV)  $(dmesg | grep -E "$IF: Link is Up" | tail -1 | grep -oE '[0-9]+[GM]bps/[A-Za-z]+') =="
echo "-- ICMP round trip to the server over $IF --"
ping -c 5 -i 0.3 -I "$IF" "$SRV" | tail -n 1 | sed 's/^/   /'

capture() {   # $1 label
	local pcap=/tmp/kl_$$.pcap
	tcpdump -i "$IF" -s 96 -w "$pcap" "tcp and host $SRV" 2>/dev/null &
	local td=$!
	sleep 1
	curl -s --interface "$IF" --max-time 8 -o /dev/null -w '%{size_download} %{time_total}' "$URL" > /tmp/kl_curl &
	local cp=$!
	sleep 4
	echo "-- $1: full socket state at 4 s --"
	ss -tin "src $IP and dst $SRV" | grep -A1 ESTAB | grep -v ESTAB | sed 's/^/   /' | fold -w 200 | head -n 8
	wait $cp 2>/dev/null
	sleep 1; kill $td 2>/dev/null; sleep 0.5; kill -9 $td 2>/dev/null; wait $td 2>/dev/null
	read sz el < /tmp/kl_curl
	awk -v b="${sz:-0}" -v s="${el:-8}" 'BEGIN{printf "   download %.2f Mbit/s (%d bytes)\n", b*8/s/1e6, b}'
	tcpdump -r "$pcap" -n -tt 2>/dev/null | python3 -c '
import sys, re, statistics as st
me = sys.argv[1]; srv = sys.argv[2]
rx = re.compile(r"^(\d+\.\d+) IP (\S+)\.(\d+) > (\S+)\.(\d+): Flags \[([^\]]*)\](.*)$")
data_in=[]; acks_out=[]; retrans=0; maxseq=0; dupack=0; lastack=None; win_me=[]; win_srv=[]; n_in=n_out=0
for line in sys.stdin:
    m = rx.match(line.strip())
    if not m: continue
    t=float(m.group(1)); src=m.group(2); dst=m.group(4); rest=m.group(7)
    ln = re.search(r"length (\d+)", rest); ln = int(ln.group(1)) if ln else 0
    sq = re.search(r"seq (\d+):(\d+)", rest); ak = re.search(r"ack (\d+)", rest); wn = re.search(r"win (\d+)", rest)
    if src == srv:
        n_in += 1
        if wn: win_srv.append(int(wn.group(1)))
        if ln > 0 and sq:
            s0=int(sq.group(1))
            if s0 < maxseq: retrans += 1
            maxseq = max(maxseq, int(sq.group(2)))
            data_in.append(t)
    elif src == me:
        n_out += 1
        if wn: win_me.append(int(wn.group(1)))
        if ln == 0 and ak:
            a=int(ak.group(1))
            if a == lastack: dupack += 1
            lastack = a
            acks_out.append(t)
gaps = [b-a for a,b in zip(data_in, data_in[1:])]
# ack latency: for each data segment, time until our next ACK
acklat=[]; j=0
for t in data_in:
    while j < len(acks_out) and acks_out[j] < t: j += 1
    if j < len(acks_out): acklat.append(acks_out[j]-t)
def ms(x): return "%.1f ms" % (x*1000)
print(f"   packets: {n_in} in, {n_out} out; data segments {len(data_in)}, our ACKs {len(acks_out)}, dup-ACKs {dupack}, server retransmissions {retrans}")
if gaps:
    gaps_s = sorted(gaps)
    print(f"   data inter-arrival: median {ms(st.median(gaps))}, 95th {ms(gaps_s[int(len(gaps_s)*0.95)])}, max {ms(gaps_s[-1])}; gaps >50 ms: {sum(1 for g in gaps if g>0.05)}")
    big = sorted(range(len(gaps)), key=lambda i: -gaps[i])[:3]
    print("   largest gaps at t=" + ", ".join(f"{data_in[i]-data_in[0]:.2f}s ({ms(gaps[i])})" for i in big))
if acklat:
    a = sorted(acklat)
    print(f"   our ACK latency after a data segment: median {ms(st.median(a))}, 95th {ms(a[int(len(a)*0.95)])}, max {ms(a[-1])}")
if win_me: print(f"   window we advertise (unscaled units): min {min(win_me)} max {max(win_me)}")
if win_srv: print(f"   window server advertises (unscaled): min {min(win_srv)} max {max(win_srv)}")
' "$IP" "$SRV"
	rm -f "$pcap"
}

capture "A: GRO on"
echo
ethtool -K "$IF" gro off >/dev/null 2>&1
capture "B: GRO off"
ethtool -K "$IF" gro on >/dev/null 2>&1
echo
echo "-- driver --"; ethtool -S "$IF" | grep -E 'wdma|overrun' | sed 's/^/   /'
