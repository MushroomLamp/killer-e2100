// looptest — can the eTSEC receive full-size frames at gigabit line rate into
// the card's DDR at all? MAC-internal loopback: everything the MAC transmits
// comes straight back into its receiver at wire speed, nothing touches the
// PHY. TX and RX rings and buffers all live in card DDR; no PCIe DMA engine,
// no host memory. The host only re-arms descriptors through BAR1.
// The MAC never sends a truncated frame, so any RX descriptor with the OV bit
// means the receive FIFO could not drain into DDR fast enough.
//
//   looptest [seconds] [frame_len]      (driver must be unloaded; needs root)
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define E 0x24000u
#define DDR_WIN 0x04000000u
#define NTX 32
#define NRX 512
#define TXBD_OFF 0x0000            /* 32 x 8 */
#define RXBD_OFF 0x1000            /* 512 x 8 = 4 KB */
#define TXBUF_OFF 0x2000           /* 32 x 1536 = 48 KB, inside the window so we can fill them */
#define RXBUF_CARD (DDR_WIN + 0x100000)   /* 512 x 2 KB, nobody reads them */
#define BUFSZ 2048

#define IEVENT 0x010
#define ECNTRL 0x020
#define DMACTRL 0x02c
#define FIFO_TX_THR 0x08c
#define TSTAT 0x104
#define TQUEUE 0x114
#define TBASEH 0x200
#define TBASE0 0x204
#define RCTRL 0x300
#define RSTAT 0x304
#define RQUEUE 0x314
#define MRBLR 0x340
#define RBASEH 0x400
#define RBASE0 0x404
#define MACCFG1 0x500
#define MACCFG2 0x504
#define MAXFRM 0x510
#define MIIMCFG 0x520
#define MIIMIND 0x534
#define MACSTNADDR1 0x540
#define MACSTNADDR2 0x544
#define RMON_RPKT 0x6a0
#define RMON_ROVR 0x6d0
#define RMON_RDRP 0x6dc
#define RMON_TPKT 0x6e4
#define RMON_TUND 0x728

static volatile uint8_t *ccsr, *win;
static uint32_t rd(uint32_t o) { return __builtin_bswap32(*(volatile uint32_t *)(ccsr + o)); }
static void wr(uint32_t o, uint32_t v) { *(volatile uint32_t *)(ccsr + o) = __builtin_bswap32(v); }
static uint32_t er(uint32_t r) { return rd(E + r); }
static void ew(uint32_t r, uint32_t v) { wr(E + r, v); }
static uint32_t wrd(uint32_t o) { return __builtin_bswap32(*(volatile uint32_t *)(win + o)); }
static void wwr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(win + o) = __builtin_bswap16(v); }
static void wwr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(win + o) = __builtin_bswap32(v); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static volatile int stop; static void on_int(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 5;
	int flen = argc > 2 ? atoi(argv[2]) : 1514;
	const char *dev = getenv("DEV") ? getenv("DEV") : "0000:07:00.0";
	char p0[128], p1[128];
	snprintf(p0, sizeof p0, "/sys/bus/pci/devices/%s/resource0", dev);
	snprintf(p1, sizeof p1, "/sys/bus/pci/devices/%s/resource1", dev);
	int f0 = open(p0, O_RDWR | O_SYNC), f1 = open(p1, O_RDWR | O_SYNC);
	if (f0 < 0 || f1 < 0) { perror("open"); return 1; }
	ccsr = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE, MAP_SHARED, f0, 0);
	win = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, f1, 0);
	if (ccsr == MAP_FAILED || win == MAP_FAILED) { perror("mmap"); return 1; }
	if (rd(0x108) >> 16 != 0x8101) { fprintf(stderr, "not MPC8308\n"); return 1; }
	if (*(volatile uint32_t *)(ccsr + 0x9de4) != (DDR_WIN | 1)) { fprintf(stderr, "BAR1 not at DDR; run ddr-window.sh apply\n"); return 1; }
	signal(SIGINT, on_int);
	printf("ACR %08x SPCR %08x  frame %d bytes, %d s, MAC loopback, rings in card DDR, no DMA engine\n",
	       rd(0x800), rd(0x110), flen, seconds);

	/* MAC: reset, gigabit full duplex, LOOPBACK */
	ew(MACCFG1, 0x80000000u); usleep(1000); ew(MACCFG1, 0);
	ew(MIIMCFG, 0x80000000u); ew(MIIMCFG, 7);
	ew(MACCFG2, 0x7205);
	ew(ECNTRL, (er(ECNTRL) & ~0x8u) | 0x1000);
	ew(FIFO_TX_THR, 0x180);
	ew(MAXFRM, 1536);
	ew(MACSTNADDR1, 0x524c4c49); ew(MACSTNADDR2, 0x4b020000);
	ew(IEVENT, 0xffffffffu);
	ew(RCTRL, 0x8);                                   /* promiscuous */

	/* frames + TX ring */
	for (int i = 0; i < NTX; i++) {
		for (int j = 0; j < flen; j += 4) wwr32(TXBUF_OFF + i * 1536 + j, 0x02000000u | (i << 16) | j);
		wwr32(TXBD_OFF + i * 8 + 4, DDR_WIN + TXBUF_OFF + i * 1536);
		wwr16(TXBD_OFF + i * 8 + 2, flen);
		wwr16(TXBD_OFF + i * 8, 0x8000 | 0x0800 | 0x0400 | (i == NTX - 1 ? 0x2000 : 0));   /* R L TC (W) */
	}
	for (int i = 0; i < NRX; i++) {
		wwr32(RXBD_OFF + i * 8 + 4, RXBUF_CARD + i * BUFSZ);
		wwr16(RXBD_OFF + i * 8 + 2, 0);
		wwr16(RXBD_OFF + i * 8, 0x8000 | (i == NRX - 1 ? 0x2000 : 0));
	}
	ew(MRBLR, BUFSZ);
	ew(RBASEH, 0); ew(RBASE0, DDR_WIN + RXBD_OFF);
	ew(TBASEH, 0); ew(TBASE0, DDR_WIN + TXBD_OFF);
	ew(RQUEUE, 0x00800080); ew(TQUEUE, 0x8000);
	ew(DMACTRL, 0xc0);
	uint32_t c_rpkt = er(RMON_RPKT), c_rovr = er(RMON_ROVR), c_rdrp = er(RMON_RDRP), c_tpkt = er(RMON_TPKT), c_tund = er(RMON_TUND);
	ew(RSTAT, 0x00800000); ew(TSTAT, 0x80000000);
	ew(MACCFG1, 0x100 | 0x4 | 0x1);                   /* LOOPBACK | RX_EN | TX_EN */

	unsigned long tx = 0, rx = 0, ov = 0, tr = 0, other = 0, rx_bytes = 0;
	unsigned tcur = 0, rcur = 0;
	double t0 = now(), tlast = t0;
	while (!stop && now() - t0 < seconds) {
		/* re-arm any transmitted TX BDs */
		for (int n = 0; n < NTX; n++) {
			uint32_t st = wrd(TXBD_OFF + tcur * 8) >> 16;
			if (st & 0x8000) break;
			wwr16(TXBD_OFF + tcur * 8, 0x8000 | 0x0800 | 0x0400 | (tcur == NTX - 1 ? 0x2000 : 0));
			tcur = (tcur + 1) % NTX; tx++;
		}
		ew(TSTAT, 0x80000000);
		/* consume received BDs */
		for (int n = 0; n < 128; n++) {
			uint32_t sl = wrd(RXBD_OFF + rcur * 8);
			uint16_t st = sl >> 16, len = sl & 0xffff;
			if (st & 0x8000) break;
			rx++; rx_bytes += len;
			if (st & 0x0002) ov++;
			else if (st & 0x0001) tr++;
			else if (st & 0x003f) other++;
			wwr16(RXBD_OFF + rcur * 8, 0x8000 | (rcur == NRX - 1 ? 0x2000 : 0));
			rcur = (rcur + 1) % NRX;
		}
		ew(RSTAT, 0x00800000);
		if (now() - tlast >= 1.0) {
			tlast = now();
			printf("  t=%.0fs tx %lu rx %lu  ov %lu tr %lu other %lu  %.0f Mbit/s\n", tlast - t0, tx, rx, ov, tr, other,
			       rx_bytes * 8.0 / (tlast - t0) / 1e6);
		}
	}
	double el = now() - t0;
	ew(MACCFG1, 0);
	printf("\nloopback %.1f s: tx %lu, rx %lu (%.0f Mbit/s), RX BDs with OV %lu, TR %lu, other errors %lu\n",
	       el, tx, rx, rx_bytes * 8.0 / el / 1e6, ov, tr, other);
	printf("MAC counters: RPKT +%u ROVR +%u RDRP +%u TPKT +%u TUND +%u  IEVENT %08x\n",
	       er(RMON_RPKT) - c_rpkt, er(RMON_ROVR) - c_rovr, er(RMON_RDRP) - c_rdrp, er(RMON_TPKT) - c_tpkt, er(RMON_TUND) - c_tund, er(IEVENT));
	printf("verdict: %s\n", ov + tr ? "the MAC cannot drain its RX FIFO into card DDR at line rate on its own" :
	       "the MAC receives at line rate into card DDR cleanly on its own");
	return 0;
}
