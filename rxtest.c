// rxtest — receive frames on the Killer E2100 by driving eTSEC1 from the
// host, with the RX ring and buffers in CARD DDR (via BAR1 -> 0x04000000).
// Optionally transmits the same ARP request as txtest first, so the host's
// own wifi stack answers it and the reply comes back in on this port.
//
// Window layout (card 0x04000000 + off):
//   0x0000  TX BD (1)        0x0100  RX BD ring (8 x 8 bytes)
//   0x1000  TX frame         0x2000  RX buffers, 8 x 2 KB
//
//   rxtest [--seconds N] [--promisc] [--no-tx] [--target-ip IP]
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define E        0x24000u
#define DDR_WIN  0x04000000u
#define WIN_SIZE 0x10000
#define TXBD_OFF 0x0000
#define RXBD_OFF 0x0100
#define TXF_OFF  0x1000
#define RXB_OFF  0x2000
#define NRX      8
#define RXBUF    0x800

#define IEVENT 0x010
#define IMASK 0x014
#define ECNTRL 0x020
#define DMACTRL 0x02c
#define TSTAT 0x104
#define TQUEUE 0x114
#define TBASEH 0x200
#define TBASE0 0x204
#define RCTRL 0x300
#define RSTAT 0x304
#define RQUEUE 0x314
#define MRBLR 0x340
#define RBPTR0 0x384
#define RBASEH 0x400
#define RBASE0 0x404
#define MACCFG1 0x500
#define MACCFG2 0x504
#define MAXFRM 0x510
#define MIIMCFG 0x520
#define MIIMCOM 0x524
#define MIIMADD 0x528
#define MIIMSTAT 0x530
#define MIIMIND 0x534
#define MACSTNADDR1 0x540
#define MACSTNADDR2 0x544
#define RBYT 0x69c
#define RPKT 0x6a0
#define RFCS 0x6a4
#define RMCA 0x6a8
#define RBCA 0x6ac
#define RUND 0x6cc
#define RDRP 0x6dc
#define TPKT 0x6e4

#define MACCFG1_SOFT_RESET 0x80000000u
#define MACCFG1_RX_EN 0x4u
#define MACCFG1_TX_EN 0x1u
#define ECNTRL_STEN 0x1000u
#define ECNTRL_R100 0x8u
#define DMACTRL_INIT 0xc3u
#define DMACTRL_GRS 0x10u
#define DMACTRL_GTS 0x8u
#define TSTAT_THLT0 0x80000000u
#define RSTAT_RHLT0 0x00800000u
#define TQUEUE_EN0 0x8000u
#define RQUEUE_EN0 0x00800080u
#define RCTRL_PROM 0x8u
#define IEVENT_GRSC 0x100u
#define IEVENT_GTSC 0x02000000u

#define TXBD_READY 0x8000
#define TXBD_WRAP 0x2000
#define TXBD_LAST 0x0800
#define TXBD_CRC 0x0400
#define RXBD_EMPTY 0x8000
#define RXBD_WRAP 0x2000
#define RXBD_LAST 0x0800
#define RXBD_FIRST 0x0400
#define RXBD_BCAST 0x0080
#define RXBD_MCAST 0x0040
#define RXBD_ERRS  0x003f   /* LG NO SH CR OV TR */

static volatile uint8_t *ccsr, *win;
static uint32_t rd(uint32_t o) { return __builtin_bswap32(*(volatile uint32_t *)(ccsr + o)); }
static void wr(uint32_t o, uint32_t v) { *(volatile uint32_t *)(ccsr + o) = __builtin_bswap32(v); }
static void win_write(uint32_t off, const uint8_t *s, size_t n) { for (size_t i = 0; i < n; i += 4) { uint32_t v = 0; memcpy(&v, s + i, n - i < 4 ? n - i : 4); *(volatile uint32_t *)(win + off + i) = v; } }
static void win_read(uint32_t off, uint8_t *d, size_t n) { for (size_t i = 0; i < n; i += 4) { uint32_t v = *(volatile uint32_t *)(win + off + i); memcpy(d + i, &v, n - i < 4 ? n - i : 4); } }
static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }
static int miim_wait(uint32_t m) { long t0 = now_ms(); while (rd(E + MIIMIND) & m) if (now_ms() - t0 > 100) return -1; return 0; }
static void miim_init(void) { wr(E + MIIMCFG, 0x80000000u); wr(E + MIIMCFG, 7); miim_wait(1); }
static uint16_t phy_rd(int reg) { wr(E + MIIMADD, (1 << 8) | reg); wr(E + MIIMCOM, 0); wr(E + MIIMCOM, 1); if (miim_wait(5)) { fprintf(stderr, "MIIM timeout\n"); exit(1); } return rd(E + MIIMSTAT) & 0xffff; }
static void bd_write(uint32_t off, uint16_t st, uint16_t len, uint32_t buf) { uint8_t b[8] = { st >> 8, st, len >> 8, len, buf >> 24, buf >> 16, buf >> 8, buf }; win_write(off, b, 8); }
static void bd_read(uint32_t off, uint16_t *st, uint16_t *len, uint32_t *buf) { uint8_t b[8]; win_read(off, b, 8); *st = b[0] << 8 | b[1]; *len = b[2] << 8 | b[3]; *buf = (uint32_t)b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7]; }
static volatile int stop; static void on_int(int s) { (void)s; stop = 1; }
static const uint8_t mac[6] = { 0x02, 0x4b, 0x49, 0x4c, 0x4c, 0x52 };

static void describe(const uint8_t *f, int n)
{
	uint16_t et = f[12] << 8 | f[13];
	printf("    %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x  type %04x  ",
	       f[6], f[7], f[8], f[9], f[10], f[11], f[0], f[1], f[2], f[3], f[4], f[5], et);
	if (et == 0x0806 && n >= 42) {
		int op = f[20] << 8 | f[21];
		printf("ARP %s %u.%u.%u.%u -> %u.%u.%u.%u", op == 1 ? "who-has" : op == 2 ? "REPLY" : "op?",
		       f[28], f[29], f[30], f[31], f[38], f[39], f[40], f[41]);
		if (op == 2 && !memcmp(f, mac, 6)) printf("   <== answered to OUR mac");
	} else if (et == 0x0800 && n >= 34) printf("IPv4 proto %u  %u.%u.%u.%u -> %u.%u.%u.%u", f[23], f[26], f[27], f[28], f[29], f[30], f[31], f[32], f[33]);
	else if (et == 0x86dd) printf("IPv6");
	putchar('\n');
}

int main(int argc, char **argv)
{
	int seconds = 10, promisc = 0, do_tx = 1; const char *target_ip = "192.168.1.14";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--promisc")) promisc = 1;
		else if (!strcmp(argv[i], "--no-tx")) do_tx = 0;
		else if (!strcmp(argv[i], "--target-ip") && i + 1 < argc) target_ip = argv[++i];
		else { fprintf(stderr, "usage: rxtest [--seconds N] [--promisc] [--no-tx] [--target-ip IP]\n"); return 2; }
	}
	const char *dev = getenv("DEV") ? getenv("DEV") : "0000:07:00.0";
	char p0[128], p1[128];
	snprintf(p0, sizeof p0, "/sys/bus/pci/devices/%s/resource0", dev);
	snprintf(p1, sizeof p1, "/sys/bus/pci/devices/%s/resource1", dev);
	int f0 = open(p0, O_RDWR | O_SYNC), f1 = open(p1, O_RDWR | O_SYNC);
	if (f0 < 0 || f1 < 0) { perror("open resource"); return 1; }
	ccsr = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE, MAP_SHARED, f0, 0);
	win = mmap(NULL, WIN_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, f1, 0);
	if (ccsr == MAP_FAILED || win == MAP_FAILED) { perror("mmap"); return 1; }
	if (rd(0x108) >> 16 != 0x8101) { fprintf(stderr, "not MPC8308\n"); return 1; }
	if (*(volatile uint32_t *)(ccsr + 0x9de4) != (DDR_WIN | 1)) { fprintf(stderr, "BAR1 not at 0x04000001; run ddr-window.sh apply\n"); return 1; }
	signal(SIGINT, on_int);

	/* PHY speed */
	miim_init();
	uint16_t cs = phy_rd(17);
	int speed = (cs >> 14) == 2 ? 1000 : (cs >> 14) == 1 ? 100 : 10;
	printf("PHY: link %s, %d Mb/s, %s duplex\n", (cs & 0x400) ? "UP" : "DOWN", speed, (cs & 0x2000) ? "full" : "HALF");
	if (!(cs & 0x400)) { fprintf(stderr, "no link\n"); return 1; }

	/* MAC */
	wr(E + MACCFG1, MACCFG1_SOFT_RESET); usleep(1000); wr(E + MACCFG1, 0); miim_init();
	wr(E + MACCFG2, speed == 1000 ? 0x7205 : 0x7105);
	uint32_t ec = (rd(E + ECNTRL) & ~ECNTRL_R100) | ECNTRL_STEN; if (speed == 100) ec |= ECNTRL_R100; wr(E + ECNTRL, ec);
	wr(E + MAXFRM, 1536);
	wr(E + MACSTNADDR1, mac[5] << 24 | mac[4] << 16 | mac[3] << 8 | mac[2]);
	wr(E + MACSTNADDR2, mac[1] << 24 | mac[0] << 16);
	wr(E + IMASK, 0); wr(E + IEVENT, 0xffffffffu);

	/* RX ring */
	for (int i = 0; i < NRX; i++)
		bd_write(RXBD_OFF + i * 8, RXBD_EMPTY | (i == NRX - 1 ? RXBD_WRAP : 0), 0, DDR_WIN + RXB_OFF + i * RXBUF);
	wr(E + MRBLR, RXBUF);
	wr(E + RCTRL, promisc ? RCTRL_PROM : 0);
	wr(E + RBASEH, 0); wr(E + RBASE0, DDR_WIN + RXBD_OFF);
	wr(E + RQUEUE, RQUEUE_EN0);

	/* TX ring (one BD), optional ARP */
	uint8_t fr[60] = {0};
	if (do_tx) {
		struct in_addr tip, sip; inet_aton(target_ip, &tip); inet_aton("192.168.1.250", &sip);
		memset(fr, 0xff, 6); memcpy(fr + 6, mac, 6); fr[12] = 0x08; fr[13] = 0x06;
		uint8_t *a = fr + 14; a[1] = 1; a[2] = 8; a[4] = 6; a[5] = 4; a[7] = 1;   /* htype 1, ptype 0800, hlen 6, plen 4, op 1 */
		memcpy(a + 8, mac, 6); memcpy(a + 14, &sip, 4); memcpy(a + 24, &tip, 4);
		win_write(TXF_OFF, fr, 60);
		bd_write(TXBD_OFF, TXBD_READY | TXBD_WRAP | TXBD_LAST | TXBD_CRC, 60, DDR_WIN + TXF_OFF);
		wr(E + TBASEH, 0); wr(E + TBASE0, DDR_WIN + TXBD_OFF); wr(E + TQUEUE, TQUEUE_EN0);
	}

	uint32_t c0[8] = { rd(E+RPKT), rd(E+RBYT), rd(E+RBCA), rd(E+RMCA), rd(E+RFCS), rd(E+RUND), rd(E+RDRP), rd(E+TPKT) };
	/* go */
	wr(E + DMACTRL, (rd(E + DMACTRL) | DMACTRL_INIT) & ~(DMACTRL_GRS | DMACTRL_GTS));
	wr(E + RSTAT, RSTAT_RHLT0);
	if (do_tx) wr(E + TSTAT, TSTAT_THLT0);
	wr(E + MACCFG1, rd(E + MACCFG1) | MACCFG1_RX_EN | (do_tx ? MACCFG1_TX_EN : 0));
	printf("RX enabled%s, listening %d s%s (^C to stop)\n", promisc ? " PROMISCUOUS" : " (our MAC + broadcast)", seconds,
	       do_tx ? ", sent ARP who-has to provoke a reply" : "");

	long t0 = now_ms(); int next = 0, got = 0;
	while (!stop && now_ms() - t0 < seconds * 1000L) {
		uint16_t st, len; uint32_t buf; bd_read(RXBD_OFF + next * 8, &st, &len, &buf);
		if (st & RXBD_EMPTY) { usleep(500); continue; }
		uint8_t f[128]; int n = len < sizeof f ? len : sizeof f; win_read(RXB_OFF + next * RXBUF, f, (n + 3) & ~3);
		got++;
		printf("[%ld ms] BD%d status %04x len %u%s%s%s%s\n", now_ms() - t0, next, st, len,
		       (st & RXBD_BCAST) ? " BCAST" : "", (st & RXBD_MCAST) ? " MCAST" : "",
		       (st & RXBD_ERRS) ? " ERRORS" : "", (st & (RXBD_FIRST | RXBD_LAST)) == (RXBD_FIRST | RXBD_LAST) ? "" : " (fragment?)");
		describe(f, n);
		if (got <= 3) { printf("    "); for (int i = 0; i < (n < 48 ? n : 48); i++) printf("%02x%s", f[i], i % 16 == 15 ? "\n    " : " "); putchar('\n'); }
		bd_write(RXBD_OFF + next * 8, RXBD_EMPTY | (next == NRX - 1 ? RXBD_WRAP : 0), 0, buf);   /* re-arm */
		wr(E + RSTAT, RSTAT_RHLT0);
		next = (next + 1) % NRX;
	}

	printf("\nreceived %d frame(s) into the ring.\nMAC counters this run: RPKT %u RBYT %u  bcast %u mcast %u  crc-err %u undersize %u dropped %u  TPKT %u\n", got,
	       rd(E+RPKT)-c0[0], rd(E+RBYT)-c0[1], rd(E+RBCA)-c0[2], rd(E+RMCA)-c0[3], rd(E+RFCS)-c0[4], rd(E+RUND)-c0[5], rd(E+RDRP)-c0[6], rd(E+TPKT)-c0[7]);
	printf("(RPKT counts everything the MAC saw on the wire, before address filtering)\n");
	/* stop */
	wr(E + DMACTRL, rd(E + DMACTRL) | DMACTRL_GRS | DMACTRL_GTS);
	t0 = now_ms(); while ((rd(E + IEVENT) & (IEVENT_GRSC | IEVENT_GTSC)) != (IEVENT_GRSC | IEVENT_GTSC) && now_ms() - t0 < 200) usleep(1000);
	wr(E + MACCFG1, rd(E + MACCFG1) & ~(MACCFG1_RX_EN | MACCFG1_TX_EN));
	wr(E + IEVENT, 0xffffffffu);
	return got ? 0 : 1;
}
