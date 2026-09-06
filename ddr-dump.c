// ddr-dump — copy a range of the CARD's DDR to stdout by sliding the BAR1
// window (64 KB) across it. Read-only apart from rewriting BAR1's translation
// register (pex_epiwtar1, CCSR+0x9DE4, little-endian) for each step, exactly
// the write ddr-window.sh already proved safe. Restores the window to
// 0x04000001 when done.
//
//   ddr-dump [start] [len] > card-ddr.bin        (defaults: 0, 128 MB)
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define WIN 0x10000u
#define RESTORE 0x04000001u

int main(int argc, char **argv)
{
	uint32_t start = argc > 1 ? strtoul(argv[1], 0, 0) : 0;
	uint32_t len   = argc > 2 ? strtoul(argv[2], 0, 0) : 0x08000000u;
	if (start % WIN || len % WIN || start + len > 0x08000000u) {
		fprintf(stderr, "start/len must be 64 KB multiples within 128 MB\n"); return 2;
	}
	const char *dev = getenv("DEV") ? getenv("DEV") : "0000:07:00.0";
	char p0[128], p1[128];
	snprintf(p0, sizeof p0, "/sys/bus/pci/devices/%s/resource0", dev);
	snprintf(p1, sizeof p1, "/sys/bus/pci/devices/%s/resource1", dev);
	int f0 = open(p0, O_RDWR | O_SYNC), f1 = open(p1, O_RDONLY | O_SYNC);
	if (f0 < 0 || f1 < 0) { perror("open"); return 1; }
	volatile uint8_t *ccsr = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE, MAP_SHARED, f0, 0);
	volatile uint32_t *win = mmap(NULL, WIN, PROT_READ, MAP_SHARED, f1, 0);
	if (ccsr == MAP_FAILED || win == MAP_FAILED) { perror("mmap"); return 1; }
	if (__builtin_bswap32(*(volatile uint32_t *)(ccsr + 0x108)) >> 16 != 0x8101) { fprintf(stderr, "not MPC8308\n"); return 1; }
	volatile uint32_t *epiwtar1 = (volatile uint32_t *)(ccsr + 0x9de4);

	uint32_t buf[WIN / 4];
	for (uint32_t a = start; a < start + len; a += WIN) {
		*epiwtar1 = a | 1;
		(void)*epiwtar1;                       /* flush the posted write before touching BAR1 */
		for (uint32_t i = 0; i < WIN / 4; i++) buf[i] = win[i];
		fwrite(buf, 1, WIN, stdout);
		if ((a / WIN) % 16 == 0) { fprintf(stderr, "\r  %3u MB", (a - start) / 1048576); }
	}
	*epiwtar1 = RESTORE;
	fprintf(stderr, "\r  done: 0x%08x..0x%08x, BAR1 restored to %08x\n", start, start + len, *epiwtar1);
	return 0;
}
