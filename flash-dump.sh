#!/bin/bash
# Dump the card's 8 MB boot flash through BAR2. READ-ONLY.
# BAR2's endpoint translation register (pex_epiwtar2) = 0xF0000001: the flash
# at local 0xF0000000, enabled. The flash LAW and chip-select cover 8 MB, so
# reads within the first 8 MB always complete. Never read past 8 MB.
set -eu
HERE=$(dirname "$(readlink -f "$0")")
D=/sys/bus/pci/devices/${DEV:-0000:07:00.0}
OUT=${OUT:-$HERE/flash.bin}
echo "confirming BAR2 translation register:"
$HERE/poke rd $D/resource0 0x9de8 le
echo "reading 8 MB of flash -> $OUT  (takes a few seconds)"
$HERE/bar-peek raw $D/resource2 0 0x800000 > "$OUT"
sha256sum "$OUT"
echo
echo "first strings:"
strings -n 8 "$OUT" | head -n 40
echo "..."
echo "u-boot / linux markers:"
strings -n 8 "$OUT" | grep -iE 'u-boot|linux|bigfoot|killer|version|Copyright' | head -n 20 || true
