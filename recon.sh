#!/bin/bash
# Killer E2100 / MPC8308 reconnaissance, v2. READ-ONLY. Needs root.
#
# v1 run (2026-09-06) established:
#   * BAR0 IS the MPC8308 CCSR register window. SPRIDR = 0x81010110.
#   * Reading BAR1 hung the PCIe bus (completion timeout, hard reset needed).
# Therefore: BAR0 is known-good, BAR1 is NEVER touched here, and BAR2/BAR4 are
# opt-in (BAR2=1 / BAR4=1) with BAR2 gated on the DDR controller being enabled.
#
# Offsets follow the MPC8308 CCSR map. (v1's labels at 0xA0-0xAC were wrong -
# those are the DDR local access windows; system config lives at 0x100.)
set -u
DEV=${DEV:-0000:07:00.0}
D=/sys/bus/pci/devices/$DEV
HERE=$(dirname "$(readlink -f "$0")")
BP=$HERE/bar-peek
OUT=${OUT:-$HERE/recon-$(date +%Y%m%d-%H%M%S)}
[ -x "$BP" ] || { echo "build first: make"; exit 1; }
[ -d "$D" ]  || { echo "no device at $DEV"; exit 1; }
mkdir -p "$OUT"
R0=$D/resource0

w()      { $BP word $R0 "$1" be; }                       # one BE word from CCSR
lawsize() { local ar=$((0x$1)); echo "$(( 1 << ((ar & 0x3f) + 1) ))"; }  # LAW size in bytes
sect()   { # sect <name> <off> <len> <file>  -> dump to file, print 1-line summary
	echo "---- $1  (CCSR+$2, len $3) -> $4.txt"
	$BP dump $R0 "$2" "$3" be 2>&1 | tee "$OUT/$4.txt" | tail -n 1
}

echo "device $DEV  ->  $OUT"
echo

echo "==== identity ===="
printf "  SPRIDR   @0x108 = %s   (0x8101xxxx = MPC8308; low byte = silicon rev)\n" "$(w 0x108)"
printf "  IMMRBAR  @0x000 = %s   (reset value is ff400000; e0000000 = firmware relocated it)\n" "$(w 0x000)"
echo

echo "==== local access windows (who lives where in the card's address space) ===="
for x in "0x020 0x024 LBLAW0(flash?)" "0x028 0x02C LBLAW1" "0x030 0x034 LBLAW2" "0x038 0x03C LBLAW3" \
         "0x060 0x064 PCILAW0" "0x068 0x06C PCILAW1" "0x080 0x084 LAW@0x80(PCIe?)" "0x088 0x08C LAW@0x88" \
         "0x0A0 0x0A4 DDRLAW0" "0x0A8 0x0AC DDRLAW1"; do
	set -- $x
	bar=$(w $1); ar=$(w $2)
	en=$(( (0x$ar >> 31) & 1 ))
	printf "  %-16s base=%s ar=%s en=%d size=%d MiB\n" "$3" "$bar" "$ar" "$en" "$(( $(lawsize $ar) / 1048576 ))"
done | tee "$OUT/laws.txt"
echo

echo "==== system config / pinmux / reset / clocks ===="
for x in "0x110 SPCR" "0x114 SICRL" "0x118 SICRH" "0x204 SWCRR(watchdog)" \
         "0x900 RCWLR" "0x904 RCWHR" "0x910 RSR" "0x91C RCR" \
         "0xA00 SPMR" "0xA04 OCCR" "0xA08 SCCR(clock-gating)" \
         "0xC00 GP1DIR" "0xC08 GP1DAT"; do
	set -- $x
	printf "  %-18s @%-6s = %s\n" "$2" "$1" "$(w $1)"
done | tee "$OUT/sysconf.txt"
echo

echo "==== DDR controller (must be enabled before BAR2 is safe to read) ===="
for x in "0x2000 CS0_BNDS" "0x2080 CS0_CONFIG" "0x2110 DDR_SDRAM_CFG" "0x2114 DDR_SDRAM_CFG_2" "0x2118 DDR_SDRAM_MODE"; do
	set -- $x
	printf "  %-18s @%-6s = %s\n" "$2" "$1" "$(w $1)"
done | tee "$OUT/ddr.txt"
MEMEN=$(( (0x$(w 0x2110) >> 31) & 1 ))
echo "  DDR MEM_EN = $MEMEN"
echo

echo "==== eTSEC1 / eTSEC2: which MAC did firmware touch? ===="
for e in "0x24000 eTSEC1" "0x25000 eTSEC2"; do
	set -- $e
	b=$(( $1 ))
	printf "  %-7s MACCFG1=%s MACCFG2=%s MIIMCFG=%s MACSTNADDR1=%s MACSTNADDR2=%s\n" "$2" \
		"$(w $((b+0x500)))" "$(w $((b+0x504)))" "$(w $((b+0x520)))" "$(w $((b+0x540)))" "$(w $((b+0x544)))"
done | tee "$OUT/etsec-summary.txt"
echo "  (MACSTNADDR = 0 and MACCFG2 = 00007000 means reset defaults: never configured)"
echo

echo "==== full block dumps (saved, one summary line each) ===="
sect "sysconf+LAWs"  0x0000  0x200  block-0000-sysconf
sect "IPIC irq ctrl" 0x0700  0x100  block-0700-ipic
sect "reset+clock"   0x0900  0x200  block-0900-reset-clock
sect "GPIO"          0x0C00  0x020  block-0c00-gpio
sect "DDR ctrl"      0x2000  0x200  block-2000-ddr
sect "eLBC (flash cs)" 0x5000 0x100 block-5000-elbc
sect "PCIe ctrl"     0x9000  0x1000 block-9000-pcie
sect "eTSEC1"        0x24000 0x600  block-24000-etsec1
sect "eTSEC2"        0x25000 0x600  block-25000-etsec2
echo

if [ "${BAR2:-0}" = 1 ]; then
	if [ "$MEMEN" = 1 ]; then
		echo "==== BAR2 (card DDR) first 64K strings - opt-in, DDR enabled ===="
		$BP strings $D/resource2 0 0x10000 8 2>&1 | tee "$OUT/bar2-strings.txt" | head -n 40
	else
		echo "==== BAR2 skipped: DDR controller MEM_EN=0, reading it would likely hang ===="
	fi
	echo
fi
if [ "${BAR4:-0}" = 1 ]; then
	echo "==== BAR4 first 4K - opt-in ===="
	$BP dump $D/resource4 0 0x1000 be 2>&1 | tee "$OUT/bar4-head.txt" | tail -n 1
	echo
fi
echo "BAR1 is deliberately never read (hung the bus in v1)."
echo "all output under $OUT"
