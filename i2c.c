// i2c - drive the MPC8308's I2C1 controller (CCSR+0x3000) from the host to
// find and read the EEPROM that holds the card's real MAC address.
// Registers are single bytes: ADR 0x00 FDR 0x04 CR 0x08 SR 0x0C DR 0x10 DFSRR 0x14.
//
//   i2c scan                      probe every 7-bit address
//   i2c dump <addr> [len] [off]   read len bytes from an 8-bit-offset EEPROM
//   i2c dump16 <addr> [len] [off] same, 16-bit offset (24C32 and larger)
//   -f <fdr>  clock divider register value (default 0x3f = slowest)
//   -b 1|2    which I2C controller (0x3000 or 0x3100)
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static uint32_t I2C = 0x3000;
#define ADR 0x00
#define FDR 0x04
#define CR  0x08
#define SR  0x0c
#define DR  0x10
#define DFSRR 0x14
#define CR_MEN 0x80
#define CR_MSTA 0x20
#define CR_MTX 0x10
#define CR_TXAK 0x08
#define CR_RSTA 0x04
#define SR_MCF 0x80
#define SR_MBB 0x20
#define SR_MAL 0x10
#define SR_MIF 0x02
#define SR_RXAK 0x01

static volatile uint8_t *ccsr;
static uint8_t r8(uint32_t o) { return ccsr[I2C + o]; }
static void w8(uint32_t o, uint8_t v) { ccsr[I2C + o] = v; }
static long now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000L + t.tv_nsec / 1000L; }

static int wait_mif(void)              /* wait for transfer complete; 0 = ack, 1 = nack, -1 = timeout/arb lost */
{
	long t0 = now_us();
	for (;;) {
		uint8_t s = r8(SR);
		if (s & SR_MIF) {
			w8(SR, 0);
			if (s & SR_MAL) return -1;
			return (s & SR_RXAK) ? 1 : 0;
		}
		if (now_us() - t0 > 20000) return -1;
	}
}
static void stop(void)
{
	w8(CR, CR_MEN);
	long t0 = now_us();
	while ((r8(SR) & SR_MBB) && now_us() - t0 < 20000) ;
}
static int start(int addr, int read, int repeated)
{
	if (!repeated && (r8(SR) & SR_MBB)) { fprintf(stderr, "bus busy\n"); return -1; }
	w8(CR, CR_MEN | CR_MSTA | CR_MTX | (repeated ? CR_RSTA : 0));
	w8(DR, (addr << 1) | (read ? 1 : 0));
	return wait_mif();
}
static int put(uint8_t b) { w8(DR, b); return wait_mif(); }
static int get(uint8_t *buf, int n)
{
	w8(CR, CR_MEN | CR_MSTA | (n == 1 ? CR_TXAK : 0));
	(void)r8(DR);                      /* dummy read starts the receive */
	for (int i = 0; i < n; i++) {
		if (wait_mif() < 0) return -1;
		if (i == n - 2) w8(CR, CR_MEN | CR_MSTA | CR_TXAK);
		if (i == n - 1) w8(CR, CR_MEN);          /* stop before reading the last byte */
		buf[i] = r8(DR);
	}
	return 0;
}

int main(int argc, char **argv)
{
	int fdr = 0x3f, ai = 1;
	while (argc > ai + 1 && argv[ai][0] == '-') {
		if (!strcmp(argv[ai], "-f")) fdr = strtol(argv[ai + 1], 0, 0);
		else if (!strcmp(argv[ai], "-b")) I2C = atoi(argv[ai + 1]) == 2 ? 0x3100 : 0x3000;
		else break;
		ai += 2;
	}
	if (argc <= ai) { fprintf(stderr, "usage: i2c [-f fdr] scan | dump <addr> [len] [off] | dump16 <addr> [len] [off]\n"); return 2; }
	const char *dev = getenv("DEV") ? getenv("DEV") : "0000:07:00.0";
	char path[128]; snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", dev);
	int fd = open(path, O_RDWR | O_SYNC); if (fd < 0) { perror(path); return 1; }
	ccsr = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ccsr == MAP_FAILED) { perror("mmap"); return 1; }
	if (__builtin_bswap32(*(volatile uint32_t *)(ccsr + 0x108)) >> 16 != 0x8101) { fprintf(stderr, "not MPC8308\n"); return 1; }

	w8(CR, 0); w8(FDR, fdr); w8(DFSRR, 0x10); w8(CR, CR_MEN); usleep(1000);
	printf("I2C at CCSR+0x%x: FDR=%02x SR=%02x\n", I2C, r8(FDR), r8(SR));

	if (!strcmp(argv[ai], "scan")) {
		int found = 0;
		for (int a = 0x08; a < 0x78; a++) {
			int r = start(a, 0, 0);
			stop();
			if (r == 0) { printf("  device at 0x%02x%s\n", a, (a & 0x78) == 0x50 ? "  (EEPROM range)" : a == 0x68 ? "  (RTC per device tree)" : ""); found++; }
			else if (r < 0) { printf("  0x%02x: bus error/timeout\n", a); }
		}
		printf("%d device(s)\n", found);
		return 0;
	}
	int wide = !strcmp(argv[ai], "dump16");
	if (wide || !strcmp(argv[ai], "dump")) {
		if (argc <= ai + 1) { fprintf(stderr, "need address\n"); return 2; }
		int addr = strtol(argv[ai + 1], 0, 0);
		int len = argc > ai + 2 ? strtol(argv[ai + 2], 0, 0) : 256;
		int off = argc > ai + 3 ? strtol(argv[ai + 3], 0, 0) : 0;
		uint8_t *buf = calloc(len, 1);
		if (start(addr, 0, 0)) { fprintf(stderr, "no ack from 0x%02x\n", addr); stop(); return 1; }
		if (wide) put(off >> 8);
		if (put(off & 0xff)) { fprintf(stderr, "offset nack\n"); stop(); return 1; }
		if (start(addr, 1, 1)) { fprintf(stderr, "no ack on read\n"); stop(); return 1; }
		if (get(buf, len)) { fprintf(stderr, "read failed\n"); stop(); return 1; }
		for (int i = 0; i < len; i += 16) {
			printf("%04x:", off + i);
			for (int j = 0; j < 16 && i + j < len; j++) printf(" %02x", buf[i + j]);
			printf("  ");
			for (int j = 0; j < 16 && i + j < len; j++) putchar(buf[i + j] >= 32 && buf[i + j] < 127 ? buf[i + j] : '.');
			putchar('\n');
		}
		return 0;
	}
	fprintf(stderr, "unknown command\n");
	return 2;
}
