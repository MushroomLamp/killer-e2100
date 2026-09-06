// txtest — send ONE ethernet frame from the Killer E2100 by driving its
// eTSEC1 MAC directly from the host.
//
// Everything the MAC's DMA engine touches lives in the CARD's DDR, reached
// through BAR1 (retargeted to card address 0x04000000 by ddr-window.sh).
// The DMA never sees host memory, so an IOMMU-less host is never at risk.
//
// Layout inside the BAR1 window (card address 0x04000000 + offset):
//   0x0000  one 8-byte TX buffer descriptor, WRAP set (a ring of one)
//   0x1000  the frame itself
//
// Sequence (mirrors what gianfar and u-boot's tsec.c do):
//   1. PHY: enable RGMII clock delays (88E1116R page 2 reg 21), soft reset,
//      wait for link, read negotiated speed.
//   2. MAC: soft reset, MACCFG2 for that speed, station address, no IRQs.
//   3. Write BD + frame into card DDR (big-endian BD fields).
//   4. TBASE0 -> ring, enable queue 0, clear THLT, set TX_EN.
//   5. Poll the BD's READY bit through BAR1; report IEVENT + TX counters.
//   6. Graceful TX stop, TX_EN off.
//
//   txtest [--target-ip A.B.C.D] [--delay] [--status]
//     --target-ip  ARP "who-has" target (default 192.168.1.14, the wifi IP)
//     --delay      ALSO program RGMII clock delays into the PHY. Off by
//                  default: the card's own device tree says plain "rgmii",
//                  so the board already has the delays right (PCB or straps)
//                  and adding more would break gigabit TX.
//     --status     just print MAC/BD/counter state, change nothing

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CCSR_SIZE   0x100000
#define E           0x24000u          /* eTSEC1 */
#define DDR_WIN     0x04000000u       /* card address BAR1 now points at */
#define WIN_SIZE    0x10000
#define BD_OFF      0x0000
#define FRAME_OFF   0x1000

/* eTSEC registers (offsets from block base) */
#define IEVENT   0x010
#define IMASK    0x014
#define ECNTRL   0x020
#define DMACTRL  0x02c
#define TSTAT    0x104
#define TQUEUE   0x114
#define TBPTR0   0x184
#define TBASEH   0x200
#define TBASE0   0x204
#define MACCFG1  0x500
#define MACCFG2  0x504
#define MAXFRM   0x510
#define MIIMCFG  0x520
#define MIIMCOM  0x524
#define MIIMADD  0x528
#define MIIMCON  0x52c
#define MIIMSTAT 0x530
#define MIIMIND  0x534
#define MACSTNADDR1 0x540
#define MACSTNADDR2 0x544
#define TBYT     0x6e0
#define TPKT     0x6e4
#define TBCA     0x6ec
#define TFCS     0x71c
#define TUND     0x728

#define MACCFG1_SOFT_RESET 0x80000000u
#define MACCFG1_TX_EN      0x00000001u
#define MACCFG2_GMII_FD    0x00007205u   /* preamble 7, byte mode, pad+crc, full duplex */
#define MACCFG2_MII_FD     0x00007105u   /* preamble 7, nibble mode, pad+crc, full duplex */
#define ECNTRL_STEN        0x00001000u
#define ECNTRL_R100        0x00000008u
#define DMACTRL_INIT       0x000000c3u
#define DMACTRL_GRS        0x00000010u
#define DMACTRL_GTS        0x00000008u
#define TSTAT_THLT0        0x80000000u
#define TQUEUE_EN0         0x00008000u
#define IEVENT_GTSC        0x02000000u
#define IEVENT_TXE         0x00400000u
#define IEVENT_TXB         0x00200000u
#define IEVENT_TXF         0x00100000u
#define IEVENT_XFUN        0x00010000u

#define TXBD_READY 0x8000
#define TXBD_WRAP  0x2000
#define TXBD_LAST  0x0800
#define TXBD_CRC   0x0400

#define PHY 1
#define MII_BMCR 0
#define MII_BMSR 1
#define MARVELL_CSTAT   17
#define MARVELL_PAGE    22
#define MARVELL_MSCR2   21   /* page 2: MAC specific control 2 */
#define MSCR2_RX_DELAY  0x0020
#define MSCR2_TX_DELAY  0x0010

static volatile uint8_t *ccsr, *win;
static uint32_t rd(uint32_t o)             { return __builtin_bswap32(*(volatile uint32_t *)(ccsr + o)); }
static void     wr(uint32_t o, uint32_t v) { *(volatile uint32_t *)(ccsr + o) = __builtin_bswap32(v); }
/* DDR window: plain memory, keep byte order. Write bytes as 32-bit words in memory order. */
static void win_write(uint32_t off, const uint8_t *src, size_t n)
{
	for (size_t i = 0; i < n; i += 4) {
		uint32_t v = 0; memcpy(&v, src + i, n - i < 4 ? n - i : 4);
		*(volatile uint32_t *)(win + off + i) = v;
	}
}
static void win_read(uint32_t off, uint8_t *dst, size_t n)
{
	for (size_t i = 0; i < n; i += 4) {
		uint32_t v = *(volatile uint32_t *)(win + off + i);
		memcpy(dst + i, &v, n - i < 4 ? n - i : 4);
	}
}
static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }

/* ---- MDIO ---- */
static int miim_wait(uint32_t mask) { long t0 = now_ms(); while (rd(E + MIIMIND) & mask) if (now_ms() - t0 > 100) return -1; return 0; }
static void miim_init(void) { wr(E + MIIMCFG, 0x80000000u); wr(E + MIIMCFG, 7); miim_wait(1); }
static uint16_t phy_rd(int reg)
{
	wr(E + MIIMADD, (PHY << 8) | reg); wr(E + MIIMCOM, 0); wr(E + MIIMCOM, 1);
	if (miim_wait(1 | 4)) { fprintf(stderr, "MIIM timeout\n"); exit(1); }
	return rd(E + MIIMSTAT) & 0xffff;
}
static void phy_wr(int reg, uint16_t v)
{
	wr(E + MIIMADD, (PHY << 8) | reg); wr(E + MIIMCON, v);
	if (miim_wait(1)) { fprintf(stderr, "MIIM timeout\n"); exit(1); }
}

static void print_status(void)
{
	uint8_t bd[8]; win_read(BD_OFF, bd, 8);
	printf("  MACCFG1 %08x MACCFG2 %08x ECNTRL %08x DMACTRL %08x\n", rd(E+MACCFG1), rd(E+MACCFG2), rd(E+ECNTRL), rd(E+DMACTRL));
	printf("  IEVENT  %08x TSTAT   %08x TBASE0 %08x TBPTR0  %08x\n", rd(E+IEVENT), rd(E+TSTAT), rd(E+TBASE0), rd(E+TBPTR0));
	printf("  TPKT %u TBYT %u TBCA %u TFCS %u TUND %u\n", rd(E+TPKT), rd(E+TBYT), rd(E+TBCA), rd(E+TFCS), rd(E+TUND));
	printf("  BD @card 0x%08x: status %02x%02x len %02x%02x buf %02x%02x%02x%02x\n",
	       DDR_WIN + BD_OFF, bd[0], bd[1], bd[2], bd[3], bd[4], bd[5], bd[6], bd[7]);
}

int main(int argc, char **argv)
{
	const char *target_ip = "192.168.1.14";
	int do_delay = 0, status_only = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--target-ip") && i + 1 < argc) target_ip = argv[++i];
		else if (!strcmp(argv[i], "--delay")) do_delay = 1;
		else if (!strcmp(argv[i], "--status")) status_only = 1;
		else { fprintf(stderr, "usage: txtest [--target-ip IP] [--delay] [--status]\n"); return 2; }
	}
	const char *dev = getenv("DEV") ? getenv("DEV") : "0000:07:00.0";
	char p0[128], p1[128];
	snprintf(p0, sizeof p0, "/sys/bus/pci/devices/%s/resource0", dev);
	snprintf(p1, sizeof p1, "/sys/bus/pci/devices/%s/resource1", dev);
	int f0 = open(p0, O_RDWR | O_SYNC), f1 = open(p1, O_RDWR | O_SYNC);
	if (f0 < 0 || f1 < 0) { perror("open resource"); return 1; }
	ccsr = mmap(NULL, CCSR_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, f0, 0);
	win  = mmap(NULL, WIN_SIZE,  PROT_READ | PROT_WRITE, MAP_SHARED, f1, 0);
	if (ccsr == MAP_FAILED || win == MAP_FAILED) { perror("mmap"); return 1; }

	if (rd(0x108) >> 16 != 0x8101) { fprintf(stderr, "not an MPC8308 CCSR, refusing\n"); return 1; }
	uint32_t tar1 = *(volatile uint32_t *)(ccsr + 0x9de4);           /* PEX block is little-endian */
	if (tar1 != (DDR_WIN | 1)) {
		fprintf(stderr, "BAR1 translation is %08x, expected %08x. Run ddr-window.sh apply first.\n", tar1, DDR_WIN | 1);
		return 1;
	}

	if (status_only) { miim_init(); puts("current state:"); print_status(); return 0; }

	/* ---------- 1. PHY ---------- */
	miim_init();
	uint16_t bmsr = phy_rd(MII_BMSR); bmsr = phy_rd(MII_BMSR);
	printf("PHY %d: BMSR %04x link %s\n", PHY, bmsr, (bmsr & 4) ? "UP" : "DOWN");
	phy_wr(MARVELL_PAGE, 2);
	printf("PHY RGMII delay reg (page2/21): %04x  rx-delay=%d tx-delay=%d  (left alone unless --delay)\n",
	       phy_rd(MARVELL_MSCR2), !!(phy_rd(MARVELL_MSCR2) & MSCR2_RX_DELAY), !!(phy_rd(MARVELL_MSCR2) & MSCR2_TX_DELAY));
	phy_wr(MARVELL_PAGE, 0);
	if (do_delay) {
		phy_wr(MARVELL_PAGE, 2);
		uint16_t m = phy_rd(MARVELL_MSCR2);
		printf("PHY RGMII delay reg (page2/21): %04x -> ", m);
		m |= MSCR2_RX_DELAY | MSCR2_TX_DELAY;
		phy_wr(MARVELL_MSCR2, m);
		printf("%04x\n", phy_rd(MARVELL_MSCR2));
		phy_wr(MARVELL_PAGE, 0);
		phy_wr(MII_BMCR, phy_rd(MII_BMCR) | 0x8000);   /* soft reset so delay takes effect */
		printf("PHY reset, waiting for link"); fflush(stdout);
		long t0 = now_ms(); uint16_t cs;
		do { usleep(200000); cs = phy_rd(MARVELL_CSTAT); putchar('.'); fflush(stdout); }
		while (!((cs & 0x0400) && (cs & 0x0800)) && now_ms() - t0 < 10000);
		putchar('\n');
	}
	uint16_t cs = phy_rd(MARVELL_CSTAT);
	int speed = (cs >> 14) == 2 ? 1000 : (cs >> 14) == 1 ? 100 : 10;
	int fd = !!(cs & 0x2000);
	printf("PHY status %04x: link %s, %d Mb/s, %s duplex\n", cs, (cs & 0x0400) ? "UP" : "DOWN", speed, fd ? "full" : "HALF");
	if (!(cs & 0x0400)) { fprintf(stderr, "no link, not sending\n"); return 1; }

	/* ---------- 2. MAC ---------- */
	wr(E + MACCFG1, MACCFG1_SOFT_RESET); usleep(1000);
	wr(E + MACCFG1, 0);
	miim_init();                                   /* MAC reset clears MIIM config too */
	wr(E + MACCFG2, speed == 1000 ? MACCFG2_GMII_FD : MACCFG2_MII_FD);
	uint32_t ec = (rd(E + ECNTRL) & ~ECNTRL_R100) | ECNTRL_STEN;
	if (speed == 100) ec |= ECNTRL_R100;
	wr(E + ECNTRL, ec);
	wr(E + MAXFRM, 1536);
	const uint8_t mac[6] = { 0x02, 0x4b, 0x49, 0x4c, 0x4c, 0x52 };   /* locally administered, "KILLR" */
	wr(E + MACSTNADDR1, (mac[5] << 24) | (mac[4] << 16) | (mac[3] << 8) | mac[2]);
	wr(E + MACSTNADDR2, (mac[1] << 24) | (mac[0] << 16));
	wr(E + IMASK, 0);
	wr(E + IEVENT, 0xffffffffu);

	/* ---------- 3. frame + BD into card DDR ---------- */
	uint8_t fr[60] = {0};
	struct in_addr tip, sip; inet_aton(target_ip, &tip); inet_aton("192.168.1.250", &sip);
	memset(fr, 0xff, 6); memcpy(fr + 6, mac, 6); fr[12] = 0x08; fr[13] = 0x06;        /* ARP */
	uint8_t *a = fr + 14;
	a[0]=0; a[1]=1; a[2]=8; a[3]=0; a[4]=6; a[5]=4; a[6]=0; a[7]=1;                     /* eth/ip, request */
	memcpy(a + 8, mac, 6); memcpy(a + 14, &sip, 4); memset(a + 18, 0, 6); memcpy(a + 24, &tip, 4);
	win_write(FRAME_OFF, fr, sizeof fr);

	uint16_t st = TXBD_READY | TXBD_WRAP | TXBD_LAST | TXBD_CRC;
	uint32_t bufp = DDR_WIN + FRAME_OFF;
	uint8_t bd[8] = { st >> 8, st & 0xff, 0, sizeof fr, bufp >> 24, bufp >> 16, bufp >> 8, bufp };
	win_write(BD_OFF, bd, 8);
	uint8_t chk[8]; win_read(BD_OFF, chk, 8);
	if (memcmp(bd, chk, 8)) { fprintf(stderr, "BD readback mismatch, aborting\n"); return 1; }
	printf("frame: ARP who-has %s tell 192.168.1.250, src %02x:%02x:%02x:%02x:%02x:%02x, %zu bytes at card 0x%08x\n",
	       target_ip, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], sizeof fr, bufp);

	/* ---------- 4. go ---------- */
	wr(E + TBASEH, 0);
	wr(E + TBASE0, DDR_WIN + BD_OFF);
	wr(E + TQUEUE, TQUEUE_EN0);
	wr(E + DMACTRL, (rd(E + DMACTRL) | DMACTRL_INIT) & ~(DMACTRL_GRS | DMACTRL_GTS));
	wr(E + TSTAT, TSTAT_THLT0);
	wr(E + MACCFG1, rd(E + MACCFG1) | MACCFG1_TX_EN);

	/* ---------- 5. wait for the MAC to consume the BD ---------- */
	long t0 = now_ms(); int sent = 0;
	while (now_ms() - t0 < 2000) {
		win_read(BD_OFF, chk, 8);
		if (!(chk[0] & (TXBD_READY >> 8))) { sent = 1; break; }
		usleep(1000);
	}
	uint32_t iev = rd(E + IEVENT);
	printf("\n%s after %ld ms\n", sent ? "BD READY bit cleared: the MAC consumed the frame" : "TIMEOUT: BD still READY, MAC never took it", now_ms() - t0);
	printf("  IEVENT %08x:%s%s%s%s\n", iev, (iev & IEVENT_TXF) ? " TXF(frame sent)" : "", (iev & IEVENT_TXB) ? " TXB" : "",
	       (iev & IEVENT_TXE) ? " TXE(ERROR)" : "", (iev & IEVENT_XFUN) ? " XFUN(underrun)" : "");
	print_status();

	/* ---------- 6. stop ---------- */
	wr(E + DMACTRL, rd(E + DMACTRL) | DMACTRL_GTS);
	t0 = now_ms(); while (!(rd(E + IEVENT) & IEVENT_GTSC) && now_ms() - t0 < 200) usleep(1000);
	wr(E + MACCFG1, rd(E + MACCFG1) & ~MACCFG1_TX_EN);
	wr(E + IEVENT, 0xffffffffu);
	puts(sent ? "\nverdict: frame left the MAC. check tcpdump on the wifi side." : "\nverdict: nothing sent.");
	return sent ? 0 : 1;
}
