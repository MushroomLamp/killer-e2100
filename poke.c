// poke — a single 32-bit read or write into a PCI BAR via sysfs, with the
// byte order stated explicitly every time. The MPC8308's CCSR is big-endian
// EXCEPT the PCI Express block (CCSR+0x9000..0x9FFF), which is little-endian.
// Plain memory targets (DDR, flash) are byte-order neutral.
//
//   poke rd <resourceN> <off> le|be
//   poke wr <resourceN> <off> <val> le|be      writes, then reads back
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int wrmode = argc >= 5 && !strcmp(argv[1], "wr");
	if (!(argc == 5 && !strcmp(argv[1], "rd")) && !(argc == 6 && wrmode)) {
		fprintf(stderr, "usage: poke rd <resourceN> <off> le|be\n"
		                "       poke wr <resourceN> <off> <val> le|be\n");
		return 2;
	}
	const char *path = argv[2];
	unsigned long off = strtoul(argv[3], 0, 0);
	const char *endian = argv[wrmode ? 5 : 4];
	int be = !strcmp(endian, "be");
	if (!be && strcmp(endian, "le")) { fprintf(stderr, "say le or be\n"); return 2; }

	int fd = open(path, (wrmode ? O_RDWR : O_RDONLY) | O_SYNC);
	if (fd < 0) { perror(path); return 1; }
	struct stat st; fstat(fd, &st);
	if (off + 4 > (unsigned long)st.st_size) { fprintf(stderr, "offset beyond BAR\n"); return 2; }
	long pg = sysconf(_SC_PAGESIZE);
	unsigned long mo = off & ~(pg - 1);
	volatile uint8_t *m = mmap(NULL, pg, PROT_READ | (wrmode ? PROT_WRITE : 0), MAP_SHARED, fd, mo);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	volatile uint32_t *p = (volatile uint32_t *)(m + (off - mo));

	if (wrmode) {
		uint32_t v = strtoul(argv[4], 0, 0);
		*p = be ? __builtin_bswap32(v) : v;
	}
	uint32_t r = *p;
	if (be) r = __builtin_bswap32(r);
	printf("%s+0x%lx %s= %08x\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, off,
	       wrmode ? "<= written, reads back " : "", r);
	return 0;
}
