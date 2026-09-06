#!/bin/bash
# Retarget BAR1 (64 KB, currently aimed at 0xD0000000 where nothing lives)
# onto the card's DDR, giving the host a scratch window inside the card.
#
# Register: pex_epiwtar1 at CCSR+0x9DE4, little-endian (PCIe block).
# Value:    DDR local address | 1, mirroring the two windows that work
#           (epiwtar0 = 0xE0000001 -> CCSR, epiwtar2 = 0xF0000001 -> flash).
# Target:   0x04000000, 64 MB into the 128 MB DDR. Well clear of u-boot,
#           which lives at the top of DDR, and of anything loaded at the bottom.
#
# Without "apply" this only prints the current registers.
set -eu
HERE=$(dirname "$(readlink -f "$0")")
D=/sys/bus/pci/devices/${DEV:-0000:07:00.0}
TARGET=${TARGET:-0x04000000}
echo "endpoint inbound translation registers (little-endian):"
for i in 0 1 2 3; do printf "  epiwtar%d (BAR%s) " $i $([ $i = 3 ] && echo 4 || echo $i); $HERE/poke rd $D/resource0 $((0x9de0 + i*4)) le; done
[ "${1:-}" = apply ] || { echo; echo "dry run. re-run with: $0 apply"; exit 0; }
echo
echo "retargeting BAR1 -> DDR $TARGET"
$HERE/poke wr $D/resource0 0x9de4 $(( TARGET | 1 )) le
echo
echo "reading BAR1 (first 64 bytes) - the read that hung in v1:"
$HERE/bar-peek dump $D/resource1 0 0x40
echo
echo "write a pattern into card DDR through BAR1 and read it back:"
$HERE/poke wr $D/resource1 0x0 0xdeadbeef le
$HERE/poke wr $D/resource1 0x4 0x0badf00d le
$HERE/poke wr $D/resource1 0xfffc 0xcafebabe le
$HERE/bar-peek dump $D/resource1 0 0x10
$HERE/bar-peek dump $D/resource1 0xfff0 0x10
echo
echo "if the pattern reads back, the host now has a 64 KB window into card DDR."
