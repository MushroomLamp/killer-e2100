#!/bin/bash
# Killer E2100 / MPC8308 reconnaissance. READ-ONLY. Needs root for BAR mmap.
#
# Answers the questions that decide the whole strategy:
#   1. Is BAR0 the MPC8308 CCSR register window?  (1 MB is the giveaway size)
#   2. Is the on-card PowerPC alive and running firmware right now?
#      (BAR2 = card DDR; u-boot/Linux banners there mean "yes")
#   3. Is the eTSEC MAC reachable, and does it already hold a real MAC address?
#
# Register labels below follow the MPC83xx family CCSR layout. Treat them as
# candidates until confirmed against the MPC8308 Reference Manual.
set -u
DEV=${DEV:-0000:07:00.0}
D=/sys/bus/pci/devices/$DEV
BP=$(dirname "$(readlink -f "$0")")/bar-peek
OUT=${OUT:-$(dirname "$(readlink -f "$0")")/recon-$(date +%Y%m%d-%H%M%S)}
[ -x "$BP" ] || { echo "build first: make"; exit 1; }
[ -d "$D" ]  || { echo "no device at $DEV"; exit 1; }
mkdir -p "$OUT"
echo "device $DEV  ->  $OUT"
echo "vendor $(cat $D/vendor) device $(cat $D/device) class $(cat $D/class)"
echo

echo "==== BAR0 first 4K, shown big-endian (candidate CCSR system-config block) ===="
$BP dump $D/resource0 0 0x1000 be 2>&1 | tee $OUT/bar0-head.txt | tail -n 1
echo

echo "==== BAR0 candidate MPC83xx registers ===="
for x in "0x000 IMMRBAR" "0x0A0 SPRIDR" "0x0A4 SPCR" "0x0A8 SICRL" "0x0AC SICRH" \
         "0x900 RCWLR" "0x904 RCWHR" "0x910 RSR" "0x91C RCR" "0xA00 SPMR"; do
	set -- $x
	printf "  %-8s @ %-6s = %s\n" "$2" "$1" "$($BP word $D/resource0 $1 be)"
done | tee $OUT/bar0-regs.txt
echo

echo "==== BAR0 candidate eTSEC1 MAC block (0x24500..0x24560, big-endian) ===="
echo "  MACCFG1 @+500, MACCFG2 @+504, MACSTNADDR1/2 @+540/+544 (station MAC address)"
$BP dump $D/resource0 0x24500 0x60 be 2>&1 | tee $OUT/bar0-etsec1.txt
echo

echo "==== BAR1 first 4K (64K region, purpose unknown) ===="
$BP dump $D/resource1 0 0x1000 2>&1 | tee $OUT/bar1-head.txt | tail -n 1
echo

echo "==== BAR4 first 4K (16K region, likely message/doorbell unit) ===="
$BP dump $D/resource4 0 0x1000 2>&1 | tee $OUT/bar4-head.txt | tail -n 1
echo

echo "==== BAR2 first 1M strings (candidate card DDR - look for u-boot / Linux banners) ===="
$BP strings $D/resource2 0 0x100000 8 2>&1 | tee $OUT/bar2-strings.txt | head -n 60
echo "  ($(wc -l < $OUT/bar2-strings.txt) strings total, full list in $OUT/bar2-strings.txt)"
echo
echo "raw hex dumps and full outputs saved under $OUT"
