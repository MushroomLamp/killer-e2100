// mdio - talk to the PHY behind the Killer E2100's eTSEC through the
// MPC8308's MII-management block, from userspace, via BAR0 (CCSR).
//
// This is the project's FIRST tool that writes to the device. Every write
// lands inside the eTSEC MIIM register block (CCSR+0x24520..0x24534). Those
// registers drive a slow two-wire serial bus to the PHY and nothing else:
// no DMA, no interrupts, nothing the host can see. The MAC itself stays
// disabled.
//
//   mdio scan   [-e 1|2]                    probe PHY addresses 0..31
//   mdio read   [-e 1|2] <phy> <reg>        read one 16-bit PHY register
//   mdio write  [-e 1|2] <phy> <reg> <val>  write one (careful)
//   mdio watch  [-e 1|2] <phy>              poll link status; plug/unplug
//
// -e picks which eTSEC's MIIM block to use. On MPC83xx the MDIO pins are
// normally owned by eTSEC1, so that is the default. DEV=<pci addr> overrides
// the device.

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

#define CCSR_SIZE 0x100000
#define ETSEC_BASE(n) ((n) == 2 ? 0x25000u : 0x24000u)
#define MIIMCFG  0x520
#define MIIMCOM  0x524
#define MIIMADD  0x528
#define MIIMCON  0x52c
#define MIIMSTAT 0x530
#define MIIMIND  0x534

#define MIIMCFG_RESET    0x80000000u
#define MIIMCFG_CLKDIV   0x00000007u   /* the value gianfar uses everywhere */
#define MIIMCOM_READ     0x00000001u
#define MIIMIND_BUSY     0x1u
#define MIIMIND_NOTVALID 0x4u

/* Standard IEEE 802.3 clause-22 PHY registers */
#define MII_BMCR   0
#define MII_BMSR   1
#define MII_PHYID1 2
#define MII_PHYID2 3
#define BMSR_LINK     0x0004
#define BMSR_ANEGDONE 0x0020

static volatile uint8_t *ccsr;
static uint32_t base;

/* Big-endian 32-bit accessors. Never touch CCSR any other way. */
static uint32_t rd(uint32_t off) { return __builtin_bswap32(*(volatile uint32_t *)(ccsr + off)); }
static void     wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(ccsr + off) = __builtin_bswap32(v); }

static long now_ms(void)
{
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int wait_idle(uint32_t mask)
{
	long t0 = now_ms();
	while (rd(base + MIIMIND) & mask)
		if (now_ms() - t0 > 100) return -1;
	return 0;
}

static int mdio_init(void)
{
	wr(base + MIIMCFG, MIIMCFG_RESET);
	wr(base + MIIMCFG, MIIMCFG_CLKDIV);
	return wait_idle(MIIMIND_BUSY);
}

static int mdio_read(int phy, int reg, uint16_t *val)
{
	wr(base + MIIMADD, ((phy & 0x1f) << 8) | (reg & 0x1f));
	wr(base + MIIMCOM, 0);
	wr(base + MIIMCOM, MIIMCOM_READ);
	if (wait_idle(MIIMIND_BUSY | MIIMIND_NOTVALID)) return -1;
	*val = rd(base + MIIMSTAT) & 0xffff;
	return 0;
}

static int mdio_write(int phy, int reg, uint16_t val)
{
	wr(base + MIIMADD, ((phy & 0x1f) << 8) | (reg & 0x1f));
	wr(base + MIIMCON, val);
	return wait_idle(MIIMIND_BUSY);
}

static const char *vendor(uint32_t id)
{
	switch (id >> 10) {              /* OUI portion of the 32-bit PHY id */
	case 0x01410cc2 >> 10: return "Marvell";
	case 0x00206000 >> 10: case 0x00143000 >> 10: return "Broadcom";
	case 0x001cc800 >> 10: return "Realtek";
	case 0x004dd000 >> 10: return "Atheros";
	case 0x00221400 >> 10: return "Micrel";
	case 0x000fc400 >> 10: return "Vitesse";
	case 0x20005c00 >> 10: return "National";
	case 0x02430c00 >> 10: return "Intel";
	default: return "?";
	}
}

static int link_now(int phy)
{
	uint16_t s;
	/* BMSR link bit is latched-low: read twice for the live value. */
	if (mdio_read(phy, MII_BMSR, &s) || mdio_read(phy, MII_BMSR, &s)) return -1;
	return s;
}

static volatile int stop;
static void on_int(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
	int etsec = 1, ai = 1;
	if (argc > 2 && !strcmp(argv[1], "-e")) { etsec = atoi(argv[2]); ai = 3; }
	else if (argc > 3 && !strcmp(argv[2], "-e")) { etsec = atoi(argv[3]); }
	/* accept "-e N" either before or right after the verb */
	const char *verb = argc > ai ? argv[ai] : NULL;
	if (verb && argc > ai + 2 && !strcmp(argv[ai + 1], "-e")) { etsec = atoi(argv[ai + 2]); ai += 2; }
	char **rest = argv + ai + 1; int nrest = argc - ai - 1;
	if (!verb) {
		fprintf(stderr, "usage: mdio scan|read|write|watch [-e 1|2] [args]\n");
		return 2;
	}
	if (etsec != 1 && etsec != 2) { fprintf(stderr, "-e must be 1 or 2\n"); return 2; }
	base = ETSEC_BASE(etsec);

	const char *dev = getenv("DEV") ? getenv("DEV") : "0000:07:00.0";
	char path[128];
	snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", dev);
	int fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) { perror(path); return 1; }
	ccsr = mmap(NULL, CCSR_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ccsr == MAP_FAILED) { perror("mmap"); return 1; }

	if (rd(0x108) >> 16 != 0x8101) {
		fprintf(stderr, "SPRIDR=%08x: this does not look like an MPC8308 CCSR, refusing\n", rd(0x108));
		return 1;
	}
	if (mdio_init()) { fprintf(stderr, "MIIM never went idle after reset\n"); return 1; }
	printf("eTSEC%d MIIM at CCSR+0x%x, MIIMCFG=%08x\n", etsec, base + MIIMCFG, rd(base + MIIMCFG));

	if (!strcmp(verb, "scan")) {
		int found = 0;
		for (int phy = 0; phy < 32; phy++) {
			uint16_t id1, id2;
			if (mdio_read(phy, MII_PHYID1, &id1) || mdio_read(phy, MII_PHYID2, &id2)) {
				printf("phy %2d: MIIM timeout\n", phy); continue;
			}
			uint32_t id = ((uint32_t)id1 << 16) | id2;
			if (id == 0xffffffffu || id == 0) continue;
			uint16_t bmcr = 0; mdio_read(phy, MII_BMCR, &bmcr);
			int bmsr = link_now(phy);
			printf("phy %2d: id %08x (%s, model %02x rev %x)  bmcr %04x  bmsr %04x  link %s%s\n",
			       phy, id, vendor(id), (id2 >> 4) & 0x3f, id2 & 0xf, bmcr, bmsr & 0xffff,
			       (bmsr & BMSR_LINK) ? "UP" : "down",
			       (bmcr & 0x0800) ? "  [PHY is in power-down]" : "");
			found++;
		}
		if (!found)
			printf("no PHY answered on any address (all 0xFFFF).\n"
			       "  -> try -e 2, or the PHY may be held in reset (GPIO0?) or unclocked.\n");
		return 0;
	}
	if (!strcmp(verb, "read") && nrest >= 2) {
		uint16_t v;
		int phy = strtol(rest[0], 0, 0), reg = strtol(rest[1], 0, 0);
		if (mdio_read(phy, reg, &v)) { fprintf(stderr, "timeout\n"); return 1; }
		printf("phy %d reg %d = %04x\n", phy, reg, v);
		return 0;
	}
	if (!strcmp(verb, "write") && nrest >= 3) {
		int phy = strtol(rest[0], 0, 0), reg = strtol(rest[1], 0, 0);
		uint16_t v = strtol(rest[2], 0, 0), back = 0;
		if (mdio_write(phy, reg, v)) { fprintf(stderr, "timeout\n"); return 1; }
		mdio_read(phy, reg, &back);
		printf("phy %d reg %d <= %04x, reads back %04x\n", phy, reg, v, back);
		return 0;
	}
	if (!strcmp(verb, "watch") && nrest >= 1) {
		int phy = strtol(rest[0], 0, 0), last = -2;
		signal(SIGINT, on_int);
		printf("watching phy %d link; plug and unplug the cable, ^C to stop\n", phy);
		while (!stop) {
			int s = link_now(phy);
			int up = s < 0 ? -1 : !!(s & BMSR_LINK);
			if (up != last) {
				if (s < 0) printf("  MIIM timeout\n");
				else printf("  link %s   (bmsr %04x%s)\n", up ? "UP  " : "down",
				            s, (s & BMSR_ANEGDONE) ? ", autoneg done" : "");
				last = up;
			}
			usleep(250000);
		}
		return 0;
	}
	fprintf(stderr, "bad arguments\n");
	return 2;
}
