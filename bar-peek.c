// bar-peek - read a PCI BAR through its sysfs resourceN file using strict
// 32-bit volatile loads. Never byte accesses (MMIO may reject them), and
// the mapping is PROT_READ only, so this tool structurally cannot write
// to the device.
//
//   bar-peek dump    <resourceN> <offset> <len> [be]
//   bar-peek word    <resourceN> <offset>       [be]
//   bar-peek strings <resourceN> <offset> <len> [minlen]
//   bar-peek raw     <resourceN> <offset> <len>   > file   (bytes, memory order)
//
// "be" byte-swaps each word for display. Use it when the far side is a
// big-endian SoC (MPC8308) and you want registers shown the way the
// reference manual prints them.

#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void die(const char *m) { perror(m); exit(1); }

static volatile uint32_t *map_bar(const char *path, unsigned long off,
                                  unsigned long len, unsigned long *nwords)
{
	int fd = open(path, O_RDONLY | O_SYNC);
	if (fd < 0) die("open");
	struct stat st;
	if (fstat(fd, &st)) die("fstat");
	if (off + len > (unsigned long)st.st_size) {
		fprintf(stderr, "range 0x%lx+0x%lx exceeds BAR size 0x%lx\n",
			off, len, (long)st.st_size);
		exit(2);
	}
	long pg = sysconf(_SC_PAGESIZE);
	unsigned long map_off = off & ~(pg - 1), delta = off - map_off;
	void *base = mmap(NULL, delta + len, PROT_READ, MAP_SHARED, fd, map_off);
	if (base == MAP_FAILED) die("mmap");
	*nwords = len / 4;
	return (volatile uint32_t *)((volatile char *)base + delta);
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: bar-peek dump|word|strings <resourceN> <offset> [len] [be|minlen]\n");
		return 2;
	}
	const char *mode = argv[1], *path = argv[2];
	unsigned long off = strtoul(argv[3], 0, 0);
	unsigned long nwords;

	if (!strcmp(mode, "word")) {
		int be = argc > 4 && !strcmp(argv[4], "be");
		volatile uint32_t *p = map_bar(path, off, 4, &nwords);
		uint32_t v = p[0];
		if (be) v = __builtin_bswap32(v);
		printf("%08x\n", v);
		return 0;
	}

	if (argc < 5) { fprintf(stderr, "need <len>\n"); return 2; }
	unsigned long len = strtoul(argv[4], 0, 0);
	volatile uint32_t *p = map_bar(path, off, len, &nwords);

	if (!strcmp(mode, "dump")) {
		int be = argc > 5 && !strcmp(argv[5], "be");
		unsigned long ff = 0, zero = 0;
		for (unsigned long i = 0; i < nwords; i += 4) {
			printf("%08lx:", off + i * 4);
			for (int j = 0; j < 4 && i + j < nwords; j++) {
				uint32_t v = p[i + j];
				if (v == 0xffffffffu) ff++;
				if (v == 0) zero++;
				if (be) v = __builtin_bswap32(v);
				printf(" %08x", v);
			}
			printf("\n");
		}
		// The "is anything home?" summary: all-FF means nothing is
		// decoding the read; all-zero means a live but empty block.
		fprintf(stderr, "# %lu words: %lu are 0xFFFFFFFF, %lu are 0\n",
			nwords, ff, zero);
		return 0;
	}

	if (!strcmp(mode, "raw")) {
		for (unsigned long i = 0; i < nwords; i++) {
			uint32_t v = p[i];
			fwrite(&v, 4, 1, stdout);
		}
		fprintf(stderr, "# wrote %lu bytes\n", nwords * 4);
		return 0;
	}

	if (!strcmp(mode, "strings")) {
		int minlen = argc > 5 ? atoi(argv[5]) : 8;
		if (minlen <= 0) minlen = 8;
		// Pull the window into RAM with 32-bit loads, then scan bytes.
		unsigned char *buf = malloc(len);
		if (!buf) die("malloc");
		for (unsigned long i = 0; i < nwords; i++) {
			uint32_t v = p[i];
			memcpy(buf + i * 4, &v, 4);
		}
		unsigned long start = 0;
		int run = 0;
		for (unsigned long i = 0; i <= len; i++) {
			int ok = i < len && (isprint(buf[i]) || buf[i] == '\t');
			if (ok) { if (!run) start = i; run++; }
			else {
				if (run >= minlen)
					printf("%08lx: %.*s\n", off + start, run, buf + start);
				run = 0;
			}
		}
		free(buf);
		return 0;
	}

	fprintf(stderr, "unknown mode %s\n", mode);
	return 2;
}
