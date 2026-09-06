// SPDX-License-Identifier: GPL-2.0
/*
 * killer_e2100 - Linux network driver for the Bigfoot Networks Killer E2100
 * ("Xeno", Freescale MPC8308 based NIC, PCI 1957:c006 / 1a56:1201).
 *
 * The card is a PowerPC SoC. Its own firmware never boots past u-boot, so we
 * ignore it and drive the SoC's eTSEC1 ethernet MAC directly from the host:
 *   BAR0 = the SoC's 1 MB CCSR register window (big-endian registers).
 *   BAR1 = 64 KB, retargeted at probe to card DDR 0x04000000 by writing the
 *          PCIe endpoint inbound translation register. Rings and packet
 *          buffers live there, so the MAC's DMA never touches host memory.
 *          (v1 = PIO copies; v2 will DMA straight into host RAM through the
 *          PCIe outbound window.)
 * No host interrupt exists for the MAC (its IRQs go to the card's own PIC),
 * so NAPI is driven by an hrtimer.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/version.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>

#define DRV_NAME "killer_e2100"

/* ---- card address map ---- */
#define CCSR_SPRIDR   0x108
#define PEX_EPIWTAR1  0x9de4        /* little-endian, PCIe block */
#define DDR_WIN       0x04000000u   /* card DDR address BAR1 points at */
#define ETSEC         0x24000u

/* ---- eTSEC registers ---- */
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
#define GADDR0 0x880

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
#define RXBD_CRCERR 0x0004
#define RXBD_ERRS 0x003f

/* ---- PHY (Marvell 88E1116R at MDIO address 1) ---- */
#define PHY_ADDR 1
#define MII_BMSR 1
#define MII_PHYID1 2
#define MII_PHYID2 3
#define MARVELL_CSTAT 17
#define CSTAT_LINK 0x0400
#define CSTAT_RESOLVED 0x0800
#define CSTAT_FD 0x2000

/* ---- ring layout inside the 64 KB BAR1 window ---- */
#define NTX 8
#define NRX 16
#define BUFSZ 2048
#define TXBD_OFF 0x0000
#define RXBD_OFF 0x0100
#define TXBUF_OFF 0x1000
#define RXBUF_OFF 0x5000

static int poll_us = 500;
module_param(poll_us, int, 0444);
MODULE_PARM_DESC(poll_us, "NAPI poll period in microseconds (no host IRQ exists)");

struct kl {
	struct pci_dev *pdev;
	struct net_device *ndev;
	void __iomem *ccsr, *win;
	struct napi_struct napi;
	struct hrtimer timer;
	ktime_t period;
	struct delayed_work link_work;
	struct mutex mdio_lock;
	spinlock_t tx_lock;
	unsigned int tx_head, tx_tail, tx_count, rx_cur;
	int link, speed, duplex;
};

static inline u32 er(struct kl *k, u32 r) { return ioread32be(k->ccsr + ETSEC + r); }
static inline void ew(struct kl *k, u32 r, u32 v) { iowrite32be(v, k->ccsr + ETSEC + r); }

/* ---------------- MDIO ---------------- */
static int miim_wait(struct kl *k, u32 mask)
{
	int i;
	for (i = 0; i < 2000; i++) {
		if (!(er(k, MIIMIND) & mask))
			return 0;
		udelay(5);
	}
	return -ETIMEDOUT;
}

static void miim_init(struct kl *k)
{
	ew(k, MIIMCFG, 0x80000000u);
	ew(k, MIIMCFG, 7);
	miim_wait(k, 1);
}

static int phy_read(struct kl *k, int reg)
{
	ew(k, MIIMADD, (PHY_ADDR << 8) | reg);
	ew(k, MIIMCOM, 0);
	ew(k, MIIMCOM, 1);
	if (miim_wait(k, 1 | 4))
		return -ETIMEDOUT;
	return er(k, MIIMSTAT) & 0xffff;
}

/* returns 1 if link up, fills speed/duplex */
static int phy_status(struct kl *k, int *speed, int *duplex)
{
	int cs;
	mutex_lock(&k->mdio_lock);
	cs = phy_read(k, MARVELL_CSTAT);
	mutex_unlock(&k->mdio_lock);
	if (cs < 0)
		return 0;
	*speed = (cs >> 14) == 2 ? 1000 : (cs >> 14) == 1 ? 100 : 10;
	*duplex = !!(cs & CSTAT_FD);
	return !!(cs & CSTAT_LINK);
}

/* ---------------- MAC ---------------- */
static void mac_set_speed(struct kl *k, int speed, int duplex)
{
	u32 cfg2 = 0x7000 | (speed == 1000 ? 0x200 : 0x100) | 0x4 | (duplex ? 1 : 0);
	u32 ec = (er(k, ECNTRL) & ~ECNTRL_R100) | ECNTRL_STEN;
	if (speed == 100)
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
	/* group hash: all-ones accepts every multicast, good enough for v1 */
	for (i = 0; i < 8; i++)
		ew(k, GADDR0 + i * 4, allmulti ? 0xffffffffu : 0);
}

static void rings_init(struct kl *k)
{
	int i;
	for (i = 0; i < NTX; i++) {
		iowrite32be(DDR_WIN + TXBUF_OFF + i * BUFSZ, k->win + TXBD_OFF + i * 8 + 4);
		iowrite16be(0, k->win + TXBD_OFF + i * 8 + 2);
		iowrite16be(i == NTX - 1 ? TXBD_WRAP : 0, k->win + TXBD_OFF + i * 8);
	}
	for (i = 0; i < NRX; i++) {
		iowrite32be(DDR_WIN + RXBUF_OFF + i * BUFSZ, k->win + RXBD_OFF + i * 8 + 4);
		iowrite16be(0, k->win + RXBD_OFF + i * 8 + 2);
		iowrite16be(RXBD_EMPTY | (i == NRX - 1 ? RXBD_WRAP : 0), k->win + RXBD_OFF + i * 8);
	}
	k->tx_head = k->tx_tail = k->tx_count = 0;
	k->rx_cur = 0;
}

static void hw_start(struct kl *k)
{
	ew(k, MACCFG1, MACCFG1_SOFT_RESET);
	udelay(10);
	ew(k, MACCFG1, 0);
	miim_init(k);
	mac_set_speed(k, k->speed, k->duplex);
	ew(k, MAXFRM, 1536);
	mac_set_addr(k);
	ew(k, IMASK, 0);
	ew(k, IEVENT, 0xffffffffu);

	rings_init(k);
	ew(k, MRBLR, BUFSZ);
	ew(k, RBASEH, 0);
	ew(k, RBASE0, DDR_WIN + RXBD_OFF);
	ew(k, TBASEH, 0);
	ew(k, TBASE0, DDR_WIN + TXBD_OFF);
	ew(k, RQUEUE, RQUEUE_EN0);
	ew(k, TQUEUE, TQUEUE_EN0);
	kl_set_rx_mode(k->ndev);

	ew(k, DMACTRL, (er(k, DMACTRL) | DMACTRL_INIT) & ~(DMACTRL_GRS | DMACTRL_GTS));
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
}

/* ---------------- TX ---------------- */
static netdev_tx_t kl_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct kl *k = netdev_priv(ndev);
	unsigned long flags;
	unsigned int i;
	u16 st;

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
	memcpy_toio(k->win + TXBUF_OFF + i * BUFSZ, skb->data, skb->len);
	iowrite16be(skb->len, k->win + TXBD_OFF + i * 8 + 2);
	wmb();
	st = TXBD_READY | TXBD_LAST | TXBD_CRC | (i == NTX - 1 ? TXBD_WRAP : 0);
	iowrite16be(st, k->win + TXBD_OFF + i * 8);
	ew(k, TSTAT, TSTAT_THLT0);           /* wake the TX DMA if it halted */
	k->tx_head = (i + 1) % NTX;
	if (++k->tx_count == NTX)
		netif_stop_queue(ndev);
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	spin_unlock_irqrestore(&k->tx_lock, flags);

	dev_kfree_skb_any(skb);              /* data is already in card DDR */
	return NETDEV_TX_OK;
}

static void tx_reap(struct kl *k)
{
	unsigned long flags;
	spin_lock_irqsave(&k->tx_lock, flags);
	while (k->tx_count) {
		if (ioread16be(k->win + TXBD_OFF + k->tx_tail * 8) & TXBD_READY)
			break;
		k->tx_tail = (k->tx_tail + 1) % NTX;
		k->tx_count--;
	}
	if (netif_queue_stopped(k->ndev) && k->tx_count < NTX)
		netif_wake_queue(k->ndev);
	spin_unlock_irqrestore(&k->tx_lock, flags);
}

/* ---------------- RX ---------------- */
static int rx_poll(struct kl *k, int budget)
{
	struct net_device *ndev = k->ndev;
	int work = 0;

	while (work < budget) {
		void __iomem *bd = k->win + RXBD_OFF + k->rx_cur * 8;
		u16 st = ioread16be(bd);
		u16 len;

		if (st & RXBD_EMPTY)
			break;
		len = ioread16be(bd + 2);

		if ((st & RXBD_ERRS) || (st & (RXBD_FIRST | RXBD_LAST)) != (RXBD_FIRST | RXBD_LAST) ||
		    len < ETH_HLEN + ETH_FCS_LEN || len > BUFSZ) {
			ndev->stats.rx_errors++;
			if (st & RXBD_CRCERR)
				ndev->stats.rx_crc_errors++;
		} else {
			struct sk_buff *skb;
			len -= ETH_FCS_LEN;
			skb = netdev_alloc_skb_ip_align(ndev, len);
			if (!skb) {
				ndev->stats.rx_dropped++;
			} else {
				memcpy_fromio(skb_put(skb, len), k->win + RXBUF_OFF + k->rx_cur * BUFSZ, len);
				skb->protocol = eth_type_trans(skb, ndev);
				ndev->stats.rx_packets++;
				ndev->stats.rx_bytes += len;
				napi_gro_receive(&k->napi, skb);
			}
		}
		/* hand the buffer back */
		iowrite16be(RXBD_EMPTY | (k->rx_cur == NRX - 1 ? RXBD_WRAP : 0), bd);
		k->rx_cur = (k->rx_cur + 1) % NRX;
		work++;
	}
	if (work)
		ew(k, RSTAT, RSTAT_RHLT0);      /* restart RX DMA if it ran out of buffers */
	return work;
}

static int kl_napi_poll(struct napi_struct *napi, int budget)
{
	struct kl *k = container_of(napi, struct kl, napi);
	int work = rx_poll(k, budget);
	tx_reap(k);
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

/* ---------------- link monitor ---------------- */
static void kl_link_work(struct work_struct *w)
{
	struct kl *k = container_of(to_delayed_work(w), struct kl, link_work);
	int speed = 1000, duplex = 1;
	int link = phy_status(k, &speed, &duplex);

	if (link != k->link) {
		k->link = link;
		if (link) {
			netif_carrier_on(k->ndev);
			netdev_info(k->ndev, "link up, %d Mb/s %s duplex\n", speed, duplex ? "full" : "half");
		} else {
			netif_carrier_off(k->ndev);
			netdev_info(k->ndev, "link down\n");
		}
	}
	if (link && (speed != k->speed || duplex != k->duplex)) {
		k->speed = speed;
		k->duplex = duplex;
		mac_set_speed(k, speed, duplex);
	}
	schedule_delayed_work(&k->link_work, HZ);
}

/* ---------------- netdev ops ---------------- */
static int kl_open(struct net_device *ndev)
{
	struct kl *k = netdev_priv(ndev);

	miim_init(k);
	k->speed = 1000;
	k->duplex = 1;
	k->link = phy_status(k, &k->speed, &k->duplex);

	hw_start(k);
	napi_enable(&k->napi);
	hrtimer_start(&k->timer, k->period, HRTIMER_MODE_REL);
	if (k->link) {
		netif_carrier_on(ndev);
		netdev_info(ndev, "link up, %d Mb/s %s duplex\n", k->speed, k->duplex ? "full" : "half");
	} else {
		netif_carrier_off(ndev);
	}
	netif_start_queue(ndev);
	schedule_delayed_work(&k->link_work, HZ);
	return 0;
}

static int kl_stop(struct net_device *ndev)
{
	struct kl *k = netdev_priv(ndev);

	cancel_delayed_work_sync(&k->link_work);
	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	hrtimer_cancel(&k->timer);
	napi_disable(&k->napi);
	hw_stop(k);
	return 0;
}

static const struct net_device_ops kl_netdev_ops = {
	.ndo_open = kl_open,
	.ndo_stop = kl_stop,
	.ndo_start_xmit = kl_xmit,
	.ndo_set_rx_mode = kl_set_rx_mode,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static const struct ethtool_ops kl_ethtool_ops = {
	.get_link = ethtool_op_get_link,
};

/* ---------------- PCI ---------------- */
static int kl_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct net_device *ndev;
	struct kl *k;
	u32 spridr, tar;
	int err, id1, id2;
	static const u8 mac[ETH_ALEN] = { 0x02, 0x4b, 0x49, 0x4c, 0x4c, 0x52 };

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

	/* point BAR1 at card DDR */
	iowrite32(DDR_WIN | 1, k->ccsr + PEX_EPIWTAR1);
	tar = ioread32(k->ccsr + PEX_EPIWTAR1);
	if (tar != (DDR_WIN | 1)) {
		dev_err(&pdev->dev, "could not retarget BAR1 (epiwtar1=%08x)\n", tar);
		err = -EIO;
		goto out_unmap;
	}

	spin_lock_init(&k->tx_lock);
	mutex_init(&k->mdio_lock);
	INIT_DELAYED_WORK(&k->link_work, kl_link_work);
	k->period = ns_to_ktime((u64)poll_us * NSEC_PER_USEC);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	hrtimer_setup(&k->timer, kl_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
	hrtimer_init(&k->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	k->timer.function = kl_timer_fn;
#endif
	netif_napi_add(ndev, &k->napi, kl_napi_poll);

	ndev->netdev_ops = &kl_netdev_ops;
	ndev->ethtool_ops = &kl_ethtool_ops;
	ndev->max_mtu = ETH_DATA_LEN;
	eth_hw_addr_set(ndev, mac);          /* TODO: real MAC lives in the card's i2c EEPROM */
	netif_carrier_off(ndev);

	miim_init(k);
	id1 = phy_read(k, MII_PHYID1);
	id2 = phy_read(k, MII_PHYID2);

	err = register_netdev(ndev);
	if (err)
		goto out_napi;
	pci_set_drvdata(pdev, ndev);
	dev_info(&pdev->dev, "Killer E2100: MPC8308 rev %u.%u, PHY id %04x%04x at addr %d, netdev %s\n",
		 (spridr >> 4) & 0xf, spridr & 0xf, id1 & 0xffff, id2 & 0xffff, PHY_ADDR, ndev->name);
	return 0;

out_napi:
	netif_napi_del(&k->napi);
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
	netif_napi_del(&k->napi);
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
MODULE_LICENSE("GPL");
