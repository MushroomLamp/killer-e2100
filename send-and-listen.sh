#!/bin/bash
# Milestone 5: send one ARP broadcast from the Killer port, and catch it on
# the wifi interface (same LAN) with tcpdump. Needs root.
set -u
HERE=$(dirname "$(readlink -f "$0")")
WIFI=${WIFI:-wlx803f5d22642c}
SRC=02:4b:49:4c:4c:52
echo "listening on $WIFI for frames from $SRC..."
tcpdump -i "$WIFI" -e -n -l "ether src $SRC" 2>/dev/null &
TD=$!
sleep 2
echo; echo "=== txtest ==="
"$HERE/txtest" "$@"
RC=$?
sleep 4
kill $TD 2>/dev/null; wait $TD 2>/dev/null
echo; echo "=== tcpdump stopped ==="
echo "(txtest exit $RC)"
