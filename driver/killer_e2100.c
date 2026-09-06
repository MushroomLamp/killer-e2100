// SPDX-License-Identifier: GPL-2.0
/*
 * killer_e2100 - Linux network driver for the Bigfoot Networks Killer E2100
 * ("Xeno": a Freescale MPC8308 PowerPC SoC on PCIe, 1957:c006 / 1a56:1201).
 *
 * The card's own firmware never boots past u-boot, so the host drives the
 * SoC's eTSEC1 ethernet MAC directly through BAR0, which is the SoC's 1 MB
 * CCSR register window (big-endian registers, except the PCIe block at
 * 0x9000 which is little-endian).
 *
 * Data path (v0.3):
 *  TX: descriptor ring and frame buffers live in a 1 MB DMA-coherent region
 *      in host RAM. The SoC's PCIe outbound window 1 maps card-local
 *      0xB0000000..+1MB onto that region, so the MAC's DMA reads frames from
 *      host memory through it (store-and-forward, see FIFO_TX_THR).
 *  RX: the MAC's ring and buffers live in the card's own DDR, which it fills
 *      at wire speed. Each poll gathers the frames that arrived, builds a
 *      chain of descriptors for the PCIe block's write-DMA engine, and starts
 *      it once. The engine streams the frames into the host region as full
 *      128-byte PCIe writes; a final marker descriptor copies a sequence
 *      number to host RAM, and because PCIe posted writes are ordered, seeing
 *      it guarantees every frame before it has landed.
 *  The outbound window is exactly the size of the host region, so the card
 *  cannot address any other host memory: that keeps an IOMMU-less host safe.
 *
 * The MAC's interrupts terminate in the card's own interrupt controller, not
 * the host, so NAPI is driven by an hrtimer. The PHY (Marvell 88E1116R) is
 * handled by phylib over the eTSEC's MDIO block.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>
#include <linux/rtnetlink.h>

#define DRV_NAME "killer_e2100"
#define DRV_VERSION "0.3"

/* ---- card address map ---- */
#define CCSR_SPRIDR   0x108
#define CCSR_PECR1    0x140    /* PCIe controller: bits 26-31 = DMA/descriptor/PIO CSB priority */
#define CCSR_ACR      0x800    /* CSB arbiter: bit 7 COREDIS, bits 13-15 PIPE_DEP (outstanding transactions - 1) */
#define ACR_COREDIS   0x01000000u
#define ACR_PIPE_DEP_MASK 0x00070000u
#define ACR_PIPE_DEP(n)   (((n) & 7) << 16)
#define CCSR_SPCR     0x110    /* system priority: bits 18-23 = eTSEC data/BD/emergency CSB priority */
#define SPCR_TSEC_PRIO(p) ((((p) & 3) << 13) | (((p) & 3) << 11) | (((p) & 3) << 9))
#define SPCR_TSEC_MASK    SPCR_TSEC_PRIO(3)
#define PEX_CSB_CTRL  0x9808   /* LE. bit2 WDMAE, bit3 RDMAE, bit1 IBPIOE, bit0 OBPIOE */
#define PEX_DMA_DSTMR 0x9814
#define PEX_WDMA_CTRL 0x99a0   /* bit0 START, bit1 SUS */
#define PEX_WDMA_ADDR 0x99a4   /* CSB address of the first descriptor */
#define PEX_WDMA_STAT 0x99a8   /* w1c: bit0 CHCPL bit1 DSCPL bit2 DSFER bit4 DSUER bit5 BRER bit6 DAFER */
#define PEX_OWAR1     0x9cb0   /* outbound window 1: ar, bar, tarl, tarh */
#define PEX_EPIWTAR1  0x9de4   /* BAR1 -> card DDR */
#define DDR_WIN       0x04000000u
#define OB_BAR        0xB0000000u
#define OB_SIZE       0x100000u
#define OWAR_EN       0x1u
#define OWAR_TYPE_MEM 0x4u
#define WDMA_START    0x1u
#define CSB_WDMAE     0x4u
#define ETSEC         0x24000u

/* WDMA descriptor: five little-endian words. Control word: */
#define DESC_VALID    0x1u
#define DESC_NEXT_OK  0x2u
#define DESC_FBE(x)   ((x) << 4)
#define DESC_LBE(x)   ((x) << 8)
#define DESC_LEN(dw)  ((dw) << 12)
#define DESC_DONE     0x1u                  /* status word */

/* ---- eTSEC registers ---- */
#define IEVENT 0x010
#define IMASK 0x014
#define ECNTRL 0x020
#define DMACTRL 0x02c
#define FIFO_RX_PAUSE 0x050
#define FIFO_RX_PAUSE_SHUTOFF 0x054
#define FIFO_RX_ALARM 0x058
#define FIFO_RX_ALARM_SHUTOFF 0x05c
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
#define MIIMCOM 0x524
#define MIIMADD 0x528
#define MIIMCON 0x52c
#define MIIMSTAT 0x530
#define MIIMIND 0x534
#define MACSTNADDR1 0x540
#define MACSTNADDR2 0x544
#define RMON_RBYT 0x69c
#define RMON_RPKT 0x6a0
#define RMON_RFCS 0x6a4
#define RMON_RUND 0x6cc
#define RMON_ROVR 0x6d0
#define RMON_RFRG 0x6d4
#define RMON_RJBR 0x6d8
#define RMON_RDRP 0x6dc
#define RMON_TBYT 0x6e0
#define RMON_TPKT 0x6e4
#define RMON_TDRP 0x714
#define RMON_TFCS 0x71c
#define RMON_TUND 0x728
#define GADDR0 0x880

#define MACCFG1_SOFT_RESET 0x80000000u
#define MACCFG1_RX_FLOW 0x20u
#define MACCFG1_TX_FLOW 0x10u
#define MACCFG1_RX_EN 0x4u
#define MACCFG1_TX_EN 0x1u
#define ECNTRL_STEN 0x1000u
#define ECNTRL_R100 0x8u
#define DMACTRL_GRS 0x10u
#define DMACTRL_GTS 0x8u
#define TSTAT_THLT0 0x80000000u
#define RSTAT_RHLT0 0x00800000u
#define TQUEUE_EN0 0x8000u
#define RQUEUE_EN0 0x00800080u
#define RCTRL_PROM 0x8u
#define IEVENT_GRSC 0x100u
#define IEVENT_GTSC 0x02000000u
#define IEVENT_BSY 0x20000000u
#define IEVENT_EBERR 0x10000000u
#define IEVENT_TXE 0x00400000u
#define IEVENT_XFUN 0x00010000u
#define IEVENT_ERRS (IEVENT_BSY | IEVENT_EBERR | IEVENT_TXE | IEVENT_XFUN)
#define MIIMIND_BUSY 0x1u
#define MIIMIND_NOTVALID 0x4u

#define TXBD_READY 0x8000
#define TXBD_WRAP 0x2000
#define TXBD_LAST 0x0800
#define TXBD_CRC 0x0400
#define RXBD_EMPTY 0x8000
#define RXBD_WRAP 0x2000
#define RXBD_LAST 0x0800
#define RXBD_FIRST 0x0400
#define RXBD_LG 0x0020
#define RXBD_NO 0x0010
#define RXBD_SH 0x0008
#define RXBD_CRCERR 0x0004
#define RXBD_OV 0x0002
#define RXBD_TR 0x0001
#define RXBD_ERRS 0x003f

#define PHY_ADDR 1

/* ---- host DMA region (card-local OB_BAR + off) ---- */
#define NTX 64
#define BUFSZ 2048
#define BATCH 64                                     /* frames per WDMA chain = NAPI budget */
#define TXBD_OFF 0x0000
#define TXBUF_OFF 0x2000
#define RXSLOT_OFF (TXBUF_OFF + NTX * BUFSZ)         /* BATCH x 2 KB */
#define MARKER_OFF (RXSLOT_OFF + BATCH * BUFSZ)
#define HSCR_OFF (MARKER_OFF + 0x1000)               /* self-test scratch */
#define AREA_END (HSCR_OFF + 0x1000)

/* ---- card DDR through BAR1 (card-local DDR_WIN + off, 64 KB) ---- */
#define NRX 512
#define C_RXBD_OFF 0x0000                            /* NRX x 8 */
#define C_DESC_OFF 0x1000                            /* (BATCH + 2) x 32; RX BDs use 0x0000..0x0FFF */
#define C_SEQ_OFF  0x3000
#define C_SCR_OFF  0x3800                            /* self-test pattern, 256 B */
#define DESC_SZ 32
#define C_RXBUF (DDR_WIN + 0x100000)                 /* NRX x 2 KB, not host-visible */

struct bd {
	__be16 status;
	__be16 len;
	__be32 buf;
};

static int poll_us = 500;
module_param(poll_us, int, 0444);
MODULE_PARM_DESC(poll_us, "NAPI poll period in microseconds (the MAC has no host IRQ)");

static int tx_thr = 0x180;
module_param(tx_thr, int, 0444);
MODULE_PARM_DESC(tx_thr, "eTSEC FIFO_TX_THR: store-and-forward threshold, 4-byte units");

static int dmactrl = 0xc0;
module_param(dmactrl, int, 0444);
MODULE_PARM_DESC(dmactrl, "eTSEC DMACTRL (default 0xc0: TDSEN|TBDSEN)");

static int gigabit;
module_param(gigabit, int, 0444);
MODULE_PARM_DESC(gigabit, "advertise 1000BASE-T (default 0)");

static int flowctrl = 1;
module_param(flowctrl, int, 0444);
MODULE_PARM_DESC(flowctrl, "advertise and use 802.3x PAUSE flow control (default 1)");

/* frames per WDMA chain: shorter chains = shorter bursts of card-memory reads */
static int rx_batch = BATCH;
module_param(rx_batch, int, 0444);
MODULE_PARM_DESC(rx_batch, "max frames per write-DMA chain (1..64, default 64)");

/*
 * CSB arbitration priority for the eTSEC (0..3). The PCIe block's DMA engine
 * requests the bus at level 0 with many reads in flight; at level 0 the MAC
 * loses the round-robin and its RX FIFO overruns while it waits to write
 * frames into DDR. Level 3 makes the arbiter serve the MAC first.
 */
static int etsec_prio = 3;
module_param(etsec_prio, int, 0444);
MODULE_PARM_DESC(etsec_prio, "eTSEC bus priority 0..3 (default 3, highest)");

/*
 * CSB arbiter pipeline depth: how many bus transactions may be outstanding.
 * The card's u-boot leaves it at 0 = ONE transaction at a time, so every
 * 32-byte access pays full DRAM latency and the MAC and the DMA engine take
 * turns. u-boot on Freescale's own boards uses 3 (depth 4).
 */
static int pipe_dep = 3;
module_param(pipe_dep, int, 0444);
MODULE_PARM_DESC(pipe_dep, "CSB arbiter pipeline depth field 0..7 (default 3 = 4 outstanding)");

/* experiments */
static int core_off;
module_param(core_off, int, 0444);
MODULE_PARM_DESC(core_off, "1: deny the card's idle PowerPC core the bus (ACR[COREDIS]) so it cannot compete");
static int fifo_defaults;
module_param(fifo_defaults, int, 0444);
MODULE_PARM_DESC(fifo_defaults, "1: leave the eTSEC RX FIFO pause/alarm thresholds at their reset values");

static int spin_us = 40;
module_param(spin_us, int, 0444);
MODULE_PARM_DESC(spin_us, "after starting a WDMA chain, wait up to this long for it in the same poll");

static int rx_pause_on = 0x140, rx_pause_off = 0x0c0, rx_alarm_on = 0x100, rx_alarm_off = 0x080;
module_param(rx_pause_on, int, 0444);
module_param(rx_pause_off, int, 0444);
module_param(rx_alarm_on, int, 0444);
module_param(rx_alarm_off, int, 0444);

struct kl {
	struct pci_dev *pdev;
	struct net_device *ndev;
	void __iomem *ccsr, *win;
	void *area;
	dma_addr_t area_dma;
	struct bd *txbd;
	struct mii_bus *mii_bus;
	struct napi_struct napi;
	struct hrtimer timer;
	ktime_t period;
	struct work_struct reset_work;
	spinlock_t tx_lock;
	unsigned int tx_head, tx_tail, tx_count, rx_cur;
	int speed, duplex;
	/* WDMA */
	int desc_order;                   /* 0 = next pointer first in memory, 1 = control first */
	u32 seq;
	struct {
		bool active;
		int n;
		u32 seq;
		unsigned long started;
		u16 idx[BATCH], len[BATCH], st[BATCH];
		s8 slot[BATCH];
	} batch;
	u64 bd_tr, bd_ov, bd_cr, bd_sh, bd_no, bd_lg, bd_frag;
	u64 ev_xfun, ev_txe, ev_bsy, ev_eberr;
	u64 wdma_chains, wdma_timeouts, wdma_errors;
};

static inline u32 er(struct kl *k, u32 r) { return ioread32be(k->ccsr + ETSEC + r); }
static inline void ew(struct kl *k, u32 r, u32 v) { iowrite32be(v, k->ccsr + ETSEC + r); }
static inline u32 pr(struct kl *k, u32 r) { return ioread32(k->ccsr + r); }          /* PCIe block, LE */
static inline void pw(struct kl *k, u32 r, u32 v) { iowrite32(v, k->ccsr + r); }
static inline u8 *txbuf(struct kl *k, unsigned int i) { return k->area + TXBUF_OFF + i * BUFSZ; }
static inline u8 *rxslot(struct kl *k, unsigned int i) { return k->area + RXSLOT_OFF + i * BUFSZ; }
static inline u32 *marker(struct kl *k) { return k->area + MARKER_OFF; }

/* ---------------- MDIO (process context only) ---------------- */
static int miim_wait(struct kl *k, u32 mask)
{
	int i;
	for (i = 0; i < 1000; i++) {
		if (!(er(k, MIIMIND) & mask))
			return 0;
		usleep_range(5, 15);
	}
	return -ETIMEDOUT;
}

static void miim_init(struct kl *k)
{
	ew(k, MIIMCFG, 0x80000000u);
	ew(k, MIIMCFG, 7);
	miim_wait(k, MIIMIND_BUSY);
}

static int kl_mdio_read(struct mii_bus *bus, int addr, int reg)
{
	struct kl *k = bus->priv;
	ew(k, MIIMADD, (addr << 8) | reg);
	ew(k, MIIMCOM, 0);
	ew(k, MIIMCOM, 1);
	if (miim_wait(k, MIIMIND_BUSY | MIIMIND_NOTVALID))
		return -ETIMEDOUT;
	return er(k, MIIMSTAT) & 0xffff;
}

static int kl_mdio_write(struct mii_bus *bus, int addr, int reg, u16 val)
{
	struct kl *k = bus->priv;
	ew(k, MIIMADD, (addr << 8) | reg);
	ew(k, MIIMCON, val);
	return miim_wait(k, MIIMIND_BUSY);
}

/* ---------------- WDMA ---------------- */
/*
 * Write one descriptor at card offset `off` (inside the BAR1 window).
 * Words are little-endian. The word carrying VALID is written last.
 */
static void wdma_desc(struct kl *k, u32 off, u32 src, u32 dst, u32 len_dw, u32 next_card)
{
	void __iomem *d = k->win + off;
	u32 ctrl = DESC_LEN(len_dw) | DESC_LBE(0xf) | DESC_FBE(0xf) | DESC_NEXT_OK | DESC_VALID;

	if (k->desc_order == 0) {           /* next, dst, src, status, control */
		iowrite32(next_card, d + 0);
		iowrite32(dst, d + 4);
		iowrite32(src, d + 8);
		iowrite32(0, d + 12);
		iowrite32(ctrl, d + 16);
	} else {                            /* control, status, src, dst, next */
		iowrite32(0, d + 4);
		iowrite32(src, d + 8);
		iowrite32(dst, d + 12);
		iowrite32(next_card, d + 16);
		iowrite32(ctrl, d + 0);
	}
}

static void wdma_desc_null(struct kl *k, u32 off)
{
	int i;
	for (i = 0; i < DESC_SZ; i += 4)
		iowrite32(0, k->win + off + i);
}

static void wdma_kick(struct kl *k, u32 first_card, u32 seq)
{
	iowrite32(seq, k->win + C_SEQ_OFF);          /* what the marker descriptor will copy */
	(void)ioread32(k->win + C_SEQ_OFF);          /* flush posted writes into card DDR */
	pw(k, PEX_WDMA_STAT, 0x7f);
	pw(k, PEX_WDMA_ADDR, first_card);
	pw(k, PEX_WDMA_CTRL, WDMA_START);
}

/* park and re-arm the engine: disable/enable WDMAE forces it to drop any stale chain */
static void wdma_reset(struct kl *k)
{
	u32 c = pr(k, PEX_CSB_CTRL);
	pw(k, PEX_WDMA_CTRL, 0);
	pw(k, PEX_CSB_CTRL, c & ~CSB_WDMAE);
	udelay(10);
	pw(k, PEX_WDMA_STAT, 0x7f);
	pw(k, PEX_CSB_CTRL, c | CSB_WDMAE);
	udelay(10);
}

static void wdma_dump_desc(struct kl *k, const char *what, u32 off)
{
	dev_info(&k->pdev->dev, "  %s @%05x: %08x %08x %08x %08x %08x\n", what, off,
		 ioread32(k->win + off), ioread32(k->win + off + 4), ioread32(k->win + off + 8),
		 ioread32(k->win + off + 12), ioread32(k->win + off + 16));
}

/* returns 0 on success, -EIO if the engine did not deliver the marker */
static int wdma_selftest_order(struct kl *k, int order)
{
	u32 *hscr = k->area + HSCR_OFF;
	u32 seq = 0xC0DE0000u | order, d0 = C_DESC_OFF, d1 = d0 + DESC_SZ, d2 = d1 + DESC_SZ;
	int i;

	k->desc_order = order;
	for (i = 0; i < 64; i++)
		iowrite32(0x11111111u * (i + 1) ^ order, k->win + C_SCR_OFF + i * 4);
	memset(hscr, 0, 256);
	WRITE_ONCE(*marker(k), 0);
	wmb();
	wdma_desc(k, d0, DDR_WIN + C_SCR_OFF, OB_BAR + HSCR_OFF, 64, DDR_WIN + d1);
	wdma_desc(k, d1, DDR_WIN + C_SEQ_OFF, OB_BAR + MARKER_OFF, 1, 0);
	wdma_desc_null(k, d2);
	wdma_kick(k, DDR_WIN + d0, seq);
	for (i = 0; i < 5000; i++) {
		if (READ_ONCE(*marker(k)) == seq)
			break;
		udelay(1);
	}
	if (READ_ONCE(*marker(k)) != seq) {
		dev_info(&k->pdev->dev, "wdma selftest order %d: no marker (stat %08x ctrl %08x addr %08x csb %08x, marker %08x)\n",
			 order, pr(k, PEX_WDMA_STAT), pr(k, PEX_WDMA_CTRL), pr(k, PEX_WDMA_ADDR),
			 pr(k, PEX_CSB_CTRL), READ_ONCE(*marker(k)));
		wdma_dump_desc(k, "data  ", d0);
		wdma_dump_desc(k, "marker", d1);
		return -EIO;
	}
	for (i = 0; i < 64; i++)
		if (hscr[i] != (0x11111111u * (i + 1) ^ order)) {
			dev_info(&k->pdev->dev, "wdma selftest order %d: marker ok but data wrong at %d (%08x)\n",
				 order, i, hscr[i]);
			return -EIO;
		}
	return 0;
}

static int wdma_selftest(struct kl *k)
{
	static const int orders[] = { 1, 0 };     /* control-word-first is the one that works */
	int attempt, i;

	pw(k, PEX_DMA_DSTMR, 0x100);
	for (attempt = 0; attempt < 3; attempt++) {
		for (i = 0; i < 2; i++) {
			wdma_reset(k);
			if (!wdma_selftest_order(k, orders[i])) {
				dev_info(&k->pdev->dev, "wdma selftest passed: descriptor order %d (%s)%s\n",
					 orders[i], orders[i] ? "control word first" : "next pointer first",
					 attempt ? " after retry" : "");
				return 0;
			}
		}
		msleep(20);
	}
	return -EIO;
}

/* ---------------- MAC ---------------- */
static void mac_set_speed(struct kl *k, int speed, int full)
{
	u32 cfg2 = 0x7000 | (speed == SPEED_1000 ? 0x200 : 0x100) | 0x4 | (full ? 1 : 0);
	u32 ec = (er(k, ECNTRL) & ~ECNTRL_R100) | ECNTRL_STEN;
	if (speed == SPEED_100)
		ec |= ECNTRL_R100;
	ew(k, MACCFG2, cfg2);
	ew(k, ECNTRL, ec);
}

static void mac_set_addr(struct kl *k)
{
	const u8 *a = k->ndev->dev_addr;
	ew(k, MACSTNADDR1, a[5] << 24 | a[4] << 16 | a[3] << 8 | a[2]);
	ew(k, MACSTNADDR2, a[1] << 24 | a[0] << 16);
}

static void kl_set_rx_mode(struct net_device *ndev)
{
	struct kl *k = netdev_priv(ndev);
	u32 rctrl = er(k, RCTRL) & ~RCTRL_PROM;
	bool allmulti = (ndev->flags & IFF_ALLMULTI) || !netdev_mc_empty(ndev);
	int i;

	if (ndev->flags & IFF_PROMISC)
		rctrl |= RCTRL_PROM;
	ew(k, RCTRL, rctrl);
	for (i = 0; i < 8; i++)
		ew(k, GADDR0 + i * 4, allmulti ? 0xffffffffu : 0);
}

static void rings_init(struct kl *k)
{
	int i;
	for (i = 0; i < NTX; i++) {
		k->txbd[i].buf = cpu_to_be32(OB_BAR + TXBUF_OFF + i * BUFSZ);
		k->txbd[i].len = 0;
		k->txbd[i].status = cpu_to_be16(i == NTX - 1 ? TXBD_WRAP : 0);
	}
	for (i = 0; i < NRX; i++) {
		void __iomem *bd = k->win + C_RXBD_OFF + i * 8;
		iowrite32be(C_RXBUF + i * BUFSZ, bd + 4);
		iowrite16be(0, bd + 2);
		iowrite16be(RXBD_EMPTY | (i == NRX - 1 ? RXBD_WRAP : 0), bd);
	}
	k->tx_head = k->tx_tail = k->tx_count = 0;
	k->rx_cur = 0;
	k->batch.active = false;
	dma_wmb();
}

/* MAC reset also resets the MDIO block, so hold the bus lock across it */
static void hw_start(struct kl *k)
{
	mutex_lock(&k->mii_bus->mdio_lock);
	ew(k, MACCFG1, MACCFG1_SOFT_RESET);
	udelay(10);
	ew(k, MACCFG1, 0);
	miim_init(k);
	mutex_unlock(&k->mii_bus->mdio_lock);

	iowrite32be((ioread32be(k->ccsr + CCSR_SPCR) & ~SPCR_TSEC_MASK) | SPCR_TSEC_PRIO(etsec_prio),
		    k->ccsr + CCSR_SPCR);
	dev_info(&k->pdev->dev, "bus: SPCR %08x PECR1 %08x ACR %08x (etsec_prio %d core_off %d fifo_defaults %d rx_batch %d)\n",
		 ioread32be(k->ccsr + CCSR_SPCR), ioread32be(k->ccsr + CCSR_PECR1), ioread32be(k->ccsr + CCSR_ACR),
		 etsec_prio, core_off, fifo_defaults, rx_batch);
	mac_set_speed(k, k->speed, k->duplex == DUPLEX_FULL);
	ew(k, FIFO_TX_THR, tx_thr);
	if (!fifo_defaults) {
		ew(k, FIFO_RX_PAUSE, rx_pause_on);
		ew(k, FIFO_RX_PAUSE_SHUTOFF, rx_pause_off);
		ew(k, FIFO_RX_ALARM, rx_alarm_on);
		ew(k, FIFO_RX_ALARM_SHUTOFF, rx_alarm_off);
	}
	ew(k, MAXFRM, 1536);
	mac_set_addr(k);
	ew(k, IMASK, 0);
	ew(k, IEVENT, 0xffffffffu);

	rings_init(k);
	ew(k, MRBLR, BUFSZ);
	ew(k, RBASEH, 0);
	ew(k, RBASE0, DDR_WIN + C_RXBD_OFF);
	ew(k, TBASEH, 0);
	ew(k, TBASE0, OB_BAR + TXBD_OFF);
	ew(k, RQUEUE, RQUEUE_EN0);
	ew(k, TQUEUE, TQUEUE_EN0);
	kl_set_rx_mode(k->ndev);

	ew(k, DMACTRL, dmactrl & ~(DMACTRL_GRS | DMACTRL_GTS));
	ew(k, RSTAT, RSTAT_RHLT0);
	ew(k, TSTAT, TSTAT_THLT0);
	ew(k, MACCFG1, er(k, MACCFG1) | MACCFG1_RX_EN | MACCFG1_TX_EN);
}

static void hw_stop(struct kl *k)
{
	int i;
	ew(k, DMACTRL, er(k, DMACTRL) | DMACTRL_GRS | DMACTRL_GTS);
	for (i = 0; i < 200; i++) {
		if ((er(k, IEVENT) & (IEVENT_GRSC | IEVENT_GTSC)) == (IEVENT_GRSC | IEVENT_GTSC))
			break;
		udelay(100);
	}
	ew(k, MACCFG1, er(k, MACCFG1) & ~(MACCFG1_RX_EN | MACCFG1_TX_EN));
	ew(k, IEVENT, 0xffffffffu);
	/* let any WDMA chain drain, then park the engine */
	for (i = 0; i < 100 && k->batch.active && READ_ONCE(*marker(k)) != k->batch.seq; i++)
		udelay(100);
	k->batch.active = false;
	wdma_reset(k);
}

/* ---------------- TX ---------------- */
static netdev_tx_t kl_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct kl *k = netdev_priv(ndev);
	unsigned long flags;
	unsigned int i;

	if (skb_put_padto(skb, ETH_ZLEN)) {
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	if (skb->len > BUFSZ) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&k->tx_lock, flags);
	if (k->tx_count >= NTX) {
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&k->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}
	i = k->tx_head;
	skb_copy_from_linear_data(skb, txbuf(k, i), skb->len);
	k->txbd[i].len = cpu_to_be16(skb->len);
	dma_wmb();
	k->txbd[i].status = cpu_to_be16(TXBD_READY | TXBD_LAST | TXBD_CRC | (i == NTX - 1 ? TXBD_WRAP : 0));
	wmb();
	ew(k, TSTAT, TSTAT_THLT0);
	k->tx_head = (i + 1) % NTX;
	if (++k->tx_count == NTX)
		netif_stop_queue(ndev);
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	spin_unlock_irqrestore(&k->tx_lock, flags);

	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void tx_reap(struct kl *k)
{
	unsigned long flags;
	spin_lock_irqsave(&k->tx_lock, flags);
	while (k->tx_count) {
		if (be16_to_cpu(READ_ONCE(k->txbd[k->tx_tail].status)) & TXBD_READY)
			break;
		k->tx_tail = (k->tx_tail + 1) % NTX;
		k->tx_count--;
	}
	if (netif_queue_stopped(k->ndev) && k->tx_count < NTX)
		netif_wake_queue(k->ndev);
	spin_unlock_irqrestore(&k->tx_lock, flags);
}

/* ---------------- RX ---------------- */
static void rx_bd_rearm(struct kl *k, unsigned int idx)
{
	iowrite16be(RXBD_EMPTY | (idx == NRX - 1 ? RXBD_WRAP : 0), k->win + C_RXBD_OFF + idx * 8);
}

static void rx_count_error(struct kl *k, u16 st)
{
	struct net_device *ndev = k->ndev;
	ndev->stats.rx_errors++;
	if (st & RXBD_CRCERR) { ndev->stats.rx_crc_errors++; k->bd_cr++; }
	if (st & RXBD_TR) { ndev->stats.rx_fifo_errors++; k->bd_tr++; }
	if (st & RXBD_OV) { ndev->stats.rx_over_errors++; k->bd_ov++; }
	if (st & RXBD_SH) { ndev->stats.rx_length_errors++; k->bd_sh++; }
	if (st & RXBD_LG) { ndev->stats.rx_length_errors++; k->bd_lg++; }
	if (st & RXBD_NO) { ndev->stats.rx_frame_errors++; k->bd_no++; }
	if ((st & (RXBD_FIRST | RXBD_LAST)) != (RXBD_FIRST | RXBD_LAST)) k->bd_frag++;
}

/* the chain finished: hand the frames to the stack and give the card its buffers back */
static int rx_batch_deliver(struct kl *k)
{
	struct net_device *ndev = k->ndev;
	int i, delivered = 0;
	dma_rmb();                        /* marker seen: now the slot data is safe to read */
	for (i = 0; i < k->batch.n; i++) {
		if (k->batch.slot[i] >= 0) {
			u16 len = k->batch.len[i] - ETH_FCS_LEN;
			struct sk_buff *skb = napi_alloc_skb(&k->napi, len);
			if (!skb) {
				ndev->stats.rx_dropped++;
			} else {
				skb_copy_to_linear_data(skb, rxslot(k, k->batch.slot[i]), len);
				skb_put(skb, len);
				skb->protocol = eth_type_trans(skb, ndev);
				ndev->stats.rx_packets++;
				ndev->stats.rx_bytes += len;
				napi_gro_receive(&k->napi, skb);
				delivered++;
			}
		}
		rx_bd_rearm(k, k->batch.idx[i]);
	}
	k->rx_cur = (k->batch.idx[k->batch.n - 1] + 1) % NRX;
	k->batch.active = false;
	wmb();
	ew(k, RSTAT, RSTAT_RHLT0);
	return delivered;
}

static void rx_batch_abort(struct kl *k, const char *why)
{
	int i;
	net_err_ratelimited("%s: wdma %s (stat %08x ctrl %08x), dropping %d frames\n", k->ndev->name, why,
			    pr(k, PEX_WDMA_STAT), pr(k, PEX_WDMA_CTRL), k->batch.n);
	pw(k, PEX_WDMA_STAT, 0x7f);
	for (i = 0; i < k->batch.n; i++)
		rx_bd_rearm(k, k->batch.idx[i]);
	k->ndev->stats.rx_dropped += k->batch.n;
	k->rx_cur = (k->batch.idx[k->batch.n - 1] + 1) % NRX;
	k->batch.active = false;
	ew(k, RSTAT, RSTAT_RHLT0);
}

/* gather frames waiting in card DDR and start a chain moving them to host RAM */
static int rx_batch_start(struct kl *k)
{
	unsigned int idx = k->rx_cur;
	int n = 0, slots = 0, i;
	u32 d = C_DESC_OFF, next_used = 0;

	while (n < rx_batch) {
		u32 sl = ioread32be(k->win + C_RXBD_OFF + idx * 8);
		u16 st = sl >> 16, len = sl & 0xffff;

		if (st & RXBD_EMPTY)
			break;
		k->batch.idx[n] = idx;
		k->batch.st[n] = st;
		k->batch.len[n] = len;
		if ((st & RXBD_ERRS) || (st & (RXBD_FIRST | RXBD_LAST)) != (RXBD_FIRST | RXBD_LAST) ||
		    len < ETH_HLEN + ETH_FCS_LEN || len > BUFSZ) {
			rx_count_error(k, st);
			k->batch.slot[n] = -1;
		} else {
			k->batch.slot[n] = slots++;
		}
		idx = (idx + 1) % NRX;
		n++;
	}
	if (!n)
		return 0;

	/* one descriptor per good frame, chained in memory order */
	for (i = 0; i < n; i++) {
		if (k->batch.slot[i] < 0)
			continue;
		wdma_desc(k, d, C_RXBUF + k->batch.idx[i] * BUFSZ,
			  OB_BAR + RXSLOT_OFF + k->batch.slot[i] * BUFSZ,
			  DIV_ROUND_UP(k->batch.len[i], 4), DDR_WIN + d + DESC_SZ);
		d += DESC_SZ;
		next_used++;
	}
	/* marker: copies the sequence word into host RAM after all the data */
	wdma_desc(k, d, DDR_WIN + C_SEQ_OFF, OB_BAR + MARKER_OFF, 1, 0);
	wdma_desc_null(k, d + DESC_SZ);

	k->batch.n = n;
	k->batch.seq = ++k->seq;
	k->batch.started = jiffies;
	k->batch.active = true;
	k->wdma_chains++;
	wdma_kick(k, DDR_WIN + C_DESC_OFF, k->batch.seq);
	return n;
}

static int rx_poll(struct kl *k, int budget)
{
	int work = 0, i;

	if (k->batch.active) {
		if (READ_ONCE(*marker(k)) == k->batch.seq) {
			work = rx_batch_deliver(k);
		} else if (time_after(jiffies, k->batch.started + HZ / 20)) {
			k->wdma_timeouts++;
			rx_batch_abort(k, "timeout");
		} else {
			return budget;               /* in flight: ask NAPI to call us straight back */
		}
	}
	if (work >= budget)
		return work;
	if (!rx_batch_start(k))
		return work;
	/* small chains usually land in a few microseconds: deliver them now */
	for (i = 0; i < spin_us; i++) {
		if (READ_ONCE(*marker(k)) == k->batch.seq)
			return work + rx_batch_deliver(k);
		udelay(1);
	}
	return budget;                       /* big chain still copying: keep polling */
}

static void check_errors(struct kl *k)
{
	struct net_device *ndev = k->ndev;
	u32 ev = er(k, IEVENT) & IEVENT_ERRS;

	if (!ev)
		return;
	ew(k, IEVENT, ev);
	if (ev & IEVENT_XFUN) { ndev->stats.tx_fifo_errors++; k->ev_xfun++; }
	if (ev & IEVENT_TXE) { ndev->stats.tx_errors++; k->ev_txe++; }
	if (ev & IEVENT_BSY) { ndev->stats.rx_missed_errors++; k->ev_bsy++; }
	if (ev & IEVENT_EBERR) {
		k->ev_eberr++;
		net_err_ratelimited("%s: DMA bus error (IEVENT %08x)\n", ndev->name, ev);
	}
	if (ev & (IEVENT_TXE | IEVENT_XFUN))
		ew(k, TSTAT, TSTAT_THLT0);
	if (ev & IEVENT_BSY)
		ew(k, RSTAT, RSTAT_RHLT0);
}

static int kl_napi_poll(struct napi_struct *napi, int budget)
{
	struct kl *k = container_of(napi, struct kl, napi);
	int work = rx_poll(k, budget);
	tx_reap(k);
	check_errors(k);
	if (work < budget)
		napi_complete_done(napi, work);
	return work;
}

static enum hrtimer_restart kl_timer_fn(struct hrtimer *t)
{
	struct kl *k = container_of(t, struct kl, timer);
	napi_schedule(&k->napi);
	hrtimer_forward_now(t, k->period);
	return HRTIMER_RESTART;
}

/* ---------------- phylib ---------------- */
static void kl_adjust_link(struct net_device *ndev)
{
	struct kl *k = netdev_priv(ndev);
	struct phy_device *phydev = ndev->phydev;

	if (phydev->link) {
		bool tx_pause = false, rx_pause = false;
		u32 cfg1;

		if (phydev->speed != k->speed || phydev->duplex != k->duplex) {
			k->speed = phydev->speed;
			k->duplex = phydev->duplex;
			mac_set_speed(k, k->speed, k->duplex == DUPLEX_FULL);
		}
		if (flowctrl)
			phy_get_pause(phydev, &tx_pause, &rx_pause);
		cfg1 = er(k, MACCFG1) & ~(MACCFG1_RX_FLOW | MACCFG1_TX_FLOW);
		if (rx_pause)
			cfg1 |= MACCFG1_RX_FLOW;
		if (tx_pause)
			cfg1 |= MACCFG1_TX_FLOW;
		ew(k, MACCFG1, cfg1);
	}
	phy_print_status(phydev);
}

/* ---------------- recovery ---------------- */
static void kl_reset_work(struct work_struct *w)
{
	struct kl *k = container_of(w, struct kl, reset_work);
	struct net_device *ndev = k->ndev;

	rtnl_lock();
	if (netif_running(ndev)) {
		netif_stop_queue(ndev);
		hrtimer_cancel(&k->timer);
		napi_disable(&k->napi);
		hw_stop(k);
		hw_start(k);
		napi_enable(&k->napi);
		hrtimer_start(&k->timer, k->period, HRTIMER_MODE_REL);
		netif_wake_queue(ndev);
		netdev_warn(ndev, "MAC reset after TX timeout\n");
	}
	rtnl_unlock();
}

static void kl_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
	struct kl *k = netdev_priv(ndev);
	ndev->stats.tx_errors++;
	schedule_work(&k->reset_work);
}

/* ---------------- netdev ops ---------------- */
static int kl_open(struct net_device *ndev)
{
	struct kl *k = netdev_priv(ndev);
	struct phy_device *phydev = mdiobus_get_phy(k->mii_bus, PHY_ADDR);
	int err;

	if (!phydev)
		return -ENODEV;
	err = wdma_selftest(k);
	if (err) {
		netdev_err(ndev, "PCIe write-DMA self-test failed, not opening\n");
		return err;
	}
	err = phy_connect_direct(ndev, phydev, kl_adjust_link, PHY_INTERFACE_MODE_RGMII_ID);
	if (err)
		return err;
	if (!gigabit) {
		phy_remove_link_mode(phydev, ETHTOOL_LINK_MODE_1000baseT_Full_BIT);
		phy_remove_link_mode(phydev, ETHTOOL_LINK_MODE_1000baseT_Half_BIT);
	}
	if (flowctrl)
		phy_support_asym_pause(phydev);
	phy_attached_info(phydev);

	k->speed = SPEED_1000;
	k->duplex = DUPLEX_FULL;
	hw_start(k);
	napi_enable(&k->napi);
	hrtimer_start(&k->timer, k->period, HRTIMER_MODE_REL);
	netif_start_queue(ndev);
	phy_start(phydev);
	return 0;
}

static int kl_stop(struct net_device *ndev)
{
	struct kl *k = netdev_priv(ndev);

	phy_stop(ndev->phydev);
	netif_stop_queue(ndev);
	hrtimer_cancel(&k->timer);
	napi_disable(&k->napi);
	hw_stop(k);
	phy_disconnect(ndev->phydev);
	return 0;
}

static const struct net_device_ops kl_netdev_ops = {
	.ndo_open = kl_open,
	.ndo_stop = kl_stop,
	.ndo_start_xmit = kl_xmit,
	.ndo_set_rx_mode = kl_set_rx_mode,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_tx_timeout = kl_tx_timeout,
	.ndo_eth_ioctl = phy_do_ioctl_running,
};

static void kl_get_drvinfo(struct net_device *ndev, struct ethtool_drvinfo *info)
{
	struct kl *k = netdev_priv(ndev);
	strscpy(info->driver, DRV_NAME, sizeof(info->driver));
	strscpy(info->version, DRV_VERSION, sizeof(info->version));
	strscpy(info->bus_info, pci_name(k->pdev), sizeof(info->bus_info));
}

static const char kl_stat_names[][ETH_GSTRING_LEN] = {
	"mac_rx_packets", "mac_rx_bytes", "mac_rx_crc_err", "mac_rx_undersize", "mac_rx_overrun",
	"mac_rx_fragments", "mac_rx_jabber", "mac_rx_dropped",
	"mac_tx_packets", "mac_tx_bytes", "mac_tx_dropped", "mac_tx_crc_err", "mac_tx_underrun",
	"bd_rx_truncated", "bd_rx_overrun", "bd_rx_crc", "bd_rx_short", "bd_rx_nonoctet", "bd_rx_large", "bd_rx_fragmented",
	"ev_tx_underrun", "ev_tx_error", "ev_rx_busy", "ev_bus_error",
	"wdma_chains", "wdma_timeouts", "wdma_errors",
};

static int kl_get_sset_count(struct net_device *ndev, int sset)
{
	return sset == ETH_SS_STATS ? ARRAY_SIZE(kl_stat_names) : -EOPNOTSUPP;
}

static void kl_get_strings(struct net_device *ndev, u32 sset, u8 *data)
{
	if (sset == ETH_SS_STATS)
		memcpy(data, kl_stat_names, sizeof(kl_stat_names));
}

static void kl_get_ethtool_stats(struct net_device *ndev, struct ethtool_stats *stats, u64 *data)
{
	struct kl *k = netdev_priv(ndev);
	static const u16 rmon[] = { RMON_RPKT, RMON_RBYT, RMON_RFCS, RMON_RUND, RMON_ROVR, RMON_RFRG, RMON_RJBR, RMON_RDRP,
				    RMON_TPKT, RMON_TBYT, RMON_TDRP, RMON_TFCS, RMON_TUND };
	int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(rmon); i++)
		data[n++] = er(k, rmon[i]);
	data[n++] = k->bd_tr; data[n++] = k->bd_ov; data[n++] = k->bd_cr; data[n++] = k->bd_sh;
	data[n++] = k->bd_no; data[n++] = k->bd_lg; data[n++] = k->bd_frag;
	data[n++] = k->ev_xfun; data[n++] = k->ev_txe; data[n++] = k->ev_bsy; data[n++] = k->ev_eberr;
	data[n++] = k->wdma_chains; data[n++] = k->wdma_timeouts; data[n++] = k->wdma_errors;
}

static const struct ethtool_ops kl_ethtool_ops = {
	.get_drvinfo = kl_get_drvinfo,
	.get_sset_count = kl_get_sset_count,
	.get_strings = kl_get_strings,
	.get_ethtool_stats = kl_get_ethtool_stats,
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = phy_ethtool_get_link_ksettings,
	.set_link_ksettings = phy_ethtool_set_link_ksettings,
	.nway_reset = phy_ethtool_nway_reset,
};

/* ---------------- PCI ---------------- */
static void ob_window_set(struct kl *k, bool enable)
{
	pw(k, PEX_OWAR1, 0);
	if (!enable)
		return;
	pw(k, PEX_OWAR1 + 4, OB_BAR);
	pw(k, PEX_OWAR1 + 8, lower_32_bits(k->area_dma));
	pw(k, PEX_OWAR1 + 12, upper_32_bits(k->area_dma));
	pw(k, PEX_OWAR1, OB_SIZE | OWAR_TYPE_MEM | OWAR_EN);
}

static int kl_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct net_device *ndev;
	struct kl *k;
	u32 spridr;
	int err;
	static const u8 mac[ETH_ALEN] = { 0x02, 0x4b, 0x49, 0x4c, 0x4c, 0x52 };

	BUILD_BUG_ON(AREA_END > OB_SIZE);
	BUILD_BUG_ON(C_DESC_OFF + (BATCH + 2) * DESC_SZ > C_SEQ_OFF);
	BUILD_BUG_ON(C_RXBD_OFF + NRX * 8 > C_DESC_OFF);
	rx_batch = clamp(rx_batch, 1, BATCH);

	err = pci_enable_device(pdev);
	if (err)
		return err;
	err = pci_request_regions(pdev, DRV_NAME);
	if (err)
		goto out_disable;
	if (pci_resource_len(pdev, 0) < 0x100000 || pci_resource_len(pdev, 1) < 0x10000) {
		dev_err(&pdev->dev, "unexpected BAR sizes\n");
		err = -ENODEV;
		goto out_release;
	}
	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (err)
		goto out_release;
	pci_set_master(pdev);

	ndev = alloc_etherdev(sizeof(*k));
	if (!ndev) {
		err = -ENOMEM;
		goto out_release;
	}
	k = netdev_priv(ndev);
	k->pdev = pdev;
	k->ndev = ndev;
	SET_NETDEV_DEV(ndev, &pdev->dev);

	k->ccsr = pci_iomap(pdev, 0, 0);
	k->win = pci_iomap(pdev, 1, 0);
	if (!k->ccsr || !k->win) {
		err = -EIO;
		goto out_unmap;
	}
	spridr = ioread32be(k->ccsr + CCSR_SPRIDR);
	if (spridr >> 16 != 0x8101) {
		dev_err(&pdev->dev, "SPRIDR %08x is not an MPC8308, refusing\n", spridr);
		err = -ENODEV;
		goto out_unmap;
	}
	/* BAR1 -> card DDR, before anything touches it */
	pw(k, PEX_EPIWTAR1, DDR_WIN | 1);
	if (pr(k, PEX_EPIWTAR1) != (DDR_WIN | 1)) {
		dev_err(&pdev->dev, "could not retarget BAR1\n");
		err = -EIO;
		goto out_unmap;
	}

	k->area = dma_alloc_coherent(&pdev->dev, OB_SIZE, &k->area_dma, GFP_KERNEL);
	if (!k->area) {
		err = -ENOMEM;
		goto out_unmap;
	}
	if (k->area_dma & (PAGE_SIZE - 1)) {
		dev_err(&pdev->dev, "DMA region not page aligned (%pad)\n", &k->area_dma);
		err = -EIO;
		goto out_dma;
	}
	k->txbd = k->area + TXBD_OFF;
	{
		u32 acr = ioread32be(k->ccsr + CCSR_ACR);
		iowrite32be((acr & ~ACR_PIPE_DEP_MASK) | ACR_PIPE_DEP(pipe_dep) | (core_off ? ACR_COREDIS : 0),
			    k->ccsr + CCSR_ACR);
		dev_info(&pdev->dev, "CSB arbiter ACR %08x -> %08x (pipe_dep %d, core_off %d)\n",
			 acr, ioread32be(k->ccsr + CCSR_ACR), pipe_dep, core_off);
	}
	ob_window_set(k, true);

	spin_lock_init(&k->tx_lock);
	INIT_WORK(&k->reset_work, kl_reset_work);
	k->period = ns_to_ktime((u64)poll_us * NSEC_PER_USEC);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	hrtimer_setup(&k->timer, kl_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
	hrtimer_init(&k->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	k->timer.function = kl_timer_fn;
#endif
	netif_napi_add(ndev, &k->napi, kl_napi_poll);

	k->mii_bus = devm_mdiobus_alloc(&pdev->dev);
	if (!k->mii_bus) {
		err = -ENOMEM;
		goto out_napi;
	}
	k->mii_bus->name = DRV_NAME " mdio";
	k->mii_bus->read = kl_mdio_read;
	k->mii_bus->write = kl_mdio_write;
	k->mii_bus->priv = k;
	k->mii_bus->parent = &pdev->dev;
	k->mii_bus->phy_mask = ~(u32)BIT(PHY_ADDR);
	snprintf(k->mii_bus->id, MII_BUS_ID_SIZE, "%s", pci_name(pdev));
	miim_init(k);
	err = mdiobus_register(k->mii_bus);
	if (err) {
		dev_err(&pdev->dev, "mdiobus_register failed (%d)\n", err);
		goto out_napi;
	}

	ndev->netdev_ops = &kl_netdev_ops;
	ndev->ethtool_ops = &kl_ethtool_ops;
	ndev->watchdog_timeo = 5 * HZ;
	ndev->max_mtu = ETH_DATA_LEN;
	eth_hw_addr_set(ndev, mac);          /* the on-board variant has no readable EEPROM */

	err = register_netdev(ndev);
	if (err)
		goto out_mdio;
	pci_set_drvdata(pdev, ndev);
	dev_info(&pdev->dev, "Killer E2100: MPC8308 rev %u.%u, DMA region %pad, netdev %s\n",
		 (spridr >> 4) & 0xf, spridr & 0xf, &k->area_dma, ndev->name);
	return 0;

out_mdio:
	mdiobus_unregister(k->mii_bus);
out_napi:
	netif_napi_del(&k->napi);
	ob_window_set(k, false);
out_dma:
	dma_free_coherent(&pdev->dev, OB_SIZE, k->area, k->area_dma);
out_unmap:
	if (k->win)
		pci_iounmap(pdev, k->win);
	if (k->ccsr)
		pci_iounmap(pdev, k->ccsr);
	free_netdev(ndev);
out_release:
	pci_release_regions(pdev);
out_disable:
	pci_disable_device(pdev);
	return err;
}

static void kl_remove(struct pci_dev *pdev)
{
	struct net_device *ndev = pci_get_drvdata(pdev);
	struct kl *k = netdev_priv(ndev);

	unregister_netdev(ndev);
	cancel_work_sync(&k->reset_work);
	mdiobus_unregister(k->mii_bus);
	netif_napi_del(&k->napi);
	wdma_reset(k);
	pw(k, PEX_CSB_CTRL, pr(k, PEX_CSB_CTRL) & ~CSB_WDMAE);
	ob_window_set(k, false);           /* the card must lose its view of host RAM first */
	dma_free_coherent(&pdev->dev, OB_SIZE, k->area, k->area_dma);
	pci_iounmap(pdev, k->win);
	pci_iounmap(pdev, k->ccsr);
	free_netdev(ndev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static const struct pci_device_id kl_ids[] = {
	{ PCI_DEVICE_SUB(0x1957, 0xc006, 0x1a56, 0x1201) },
	{ }
};
MODULE_DEVICE_TABLE(pci, kl_ids);

static struct pci_driver kl_driver = {
	.name = DRV_NAME,
	.id_table = kl_ids,
	.probe = kl_probe,
	.remove = kl_remove,
};
module_pci_driver(kl_driver);

MODULE_AUTHOR("Mitch");
MODULE_DESCRIPTION("Bigfoot Killer E2100 (MPC8308 eTSEC) ethernet driver");
MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: marvell");
