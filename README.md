# Linux driver for the Bigfoot Killer E2100

Linux network driver for the Bigfoot Networks Killer E2100 (also sold as Killer 2100
and Killer Xeno Pro). Found on-board the Gigabyte G1.Sniper series and on PCIe cards.
Bigfoot only shipped Windows drivers. This driver gives the card a normal Linux
network interface.

## Status

| | |
|---|---|
| Link | 10/100/1000, negotiated by phylib. Gigabit is the default. |
| Receive at gigabit | Lossless. In a 14k-frame TCP download the MAC counted 13,919 frames and the driver delivered 13,919. MAC loopback runs at 987 Mbit/s into card DDR with zero overruns. |
| Throughput | Browser speedtest: 844 Mbit/s down, 95 up, which is the ISP cap on the test line. Single TCP stream from a nearby server: 497 Mbit/s on a 20 MB transfer including slow start. Earlier versions on the same port: v0.2 316 Mbit/s, v0.1 15.7 Mbit/s. |
| Transmit | Host PIO into a 20-entry ring in card DDR. Fine for a home uplink. Expect roughly 300 to 400 Mbit/s maximum. |
| MAC address | Placeholder `02:4b:49:4c:4c:52`. The on-board variant tested has no readable EEPROM. Set your own with `ip link set` or NetworkManager if needed. |
| ethtool | Link settings via phylib, driver info, `-S` statistics. |
| Jumbo frames, checksum offload, WoL | No. |
| Bigfoot features (UDP offload, traffic shaping) | No. The card's CPU is not used. |

Tested on: Gigabyte G1.Sniper 2 (Z68, on-board E2100), Linux Mint 22.3, kernel 7.0.

## What the card is

The E2100 is not an ethernet controller. It is a Freescale MPC8308 PowerPC SoC with
128 MB DDR2 and 8 MB flash on a PCIe x1 link. It enumerates with PCI class `0b20`
(PowerPC), so no ethernet driver binds to it:

```
07:00.0 Power PC: Freescale Semiconductor Inc MPC8308 (rev 10)
        Subsystem: Rivet Networks Bigfoot Killer E2100 Gigabit Ethernet Controller
```

Bigfoot's firmware ran Linux 2.6.31 on the card with a proprietary module that spoke a
mailbox and DMA protocol to the Windows driver. On the boards examined, the card's
firmware never boots past u-boot.

BAR0 is the SoC's 1 MB CCSR register window. Every peripheral in the MPC8308 is
reachable from the host through it: the eTSEC gigabit MAC, MDIO, the PCIe address
translation units, I2C, the DDR controller. The MPC8308 is documented public silicon,
so the driver programs the MAC directly instead of reverse engineering Bigfoot's
protocol.

## How it works

* BAR0 maps the CCSR. Registers are big-endian, except the PCIe controller block at
  offset `0x9000`, which is little-endian.
* The eTSEC1 MAC at CCSR+`0x24000` is programmed with the same register sequences as
  the in-tree `gianfar` driver.
* The Marvell 88E1116R PHY at MDIO address 1 is handled by phylib in `RGMII_ID` mode.
  The board straps both RGMII clock delays into the PHY.
* Receive: the MAC's ring and buffers are in card DDR. Each NAPI poll collects the
  frames that arrived, builds a descriptor chain for the PCIe block's write-DMA
  engine, and starts it once. The engine writes the frames into a 1 MB coherent
  region in host RAM as 128-byte PCIe writes. A final marker descriptor copies a
  sequence number after the data. PCIe posted writes are ordered, so when the marker
  is visible all frames before it have landed. The SoC's PCIe outbound window 1 maps
  card address `0xB0000000` onto that 1 MB region only, so the card cannot write
  anywhere else in host memory. Many boards this card ships on have no IOMMU, so
  this bound matters.
* Transmit: the ring and frames are also in card DDR. The host writes them through
  BAR1. Reason: if the MAC reads its transmit descriptors across PCIe, those reads
  queue behind the write-DMA engine's posted writes (PCIe ordering rules). The MAC has
  one DMA unit for both directions, so receive stalls and the RX FIFO overruns.
  Keeping every MAC access on the card side took receive loss from about 20% to zero
  at gigabit.
* Bus: the card's u-boot leaves the internal bus arbiter at one outstanding
  transaction. The driver sets four (`pipe_dep`), as Freescale's reference boards do.
* Interrupts: the MAC's IRQ lines go to the card's own interrupt controller, which
  the host cannot see. NAPI is driven by an hrtimer, default 500 µs (`poll_us`).
  Ping RTT to a gateway is about 0.6 ms.

## Module parameters

| parameter | default | meaning |
|---|---|---|
| `gigabit` | 1 | advertise 1000BASE-T |
| `poll_us` | 500 | NAPI poll period in microseconds |
| `tx_thr` | 0x180 | eTSEC transmit FIFO threshold (store-and-forward), 4-byte units |
| `dmactrl` | 0xc0 | eTSEC DMACTRL value |
| `flowctrl` | 1 | advertise 802.3x pause |
| `pipe_dep` | 3 | CSB arbiter pipeline depth field |
| `etsec_prio` | 3 | eTSEC bus priority 0..3 |
| `tx_in_card` | 1 | transmit ring in card DDR (1) or host RAM (0) |
| `dma_chunk` | 512 | bytes per write-DMA descriptor |
| `rx_batch` | 64 | frames per write-DMA chain |
| `spin_us` | 40 | wait for a chain in the same poll, microseconds |

## Build and install

```
cd driver
make                      # builds killer_e2100.ko against the running kernel
sudo ./try.sh             # loads it, waits for DHCP, pings the gateway
sudo rmmod killer_e2100   # unload
```

Permanent install with DKMS (rebuilds on kernel updates, loads at boot):

```
sudo ./driver/dkms-install.sh
sudo ./driver/dkms-install.sh remove   # undo
```

The module is unsigned. Secure Boot systems need it signed or a MOK enrolled.
Legacy BIOS boards do not care.

## Tools

Userspace tools used to work out the card, all through sysfs BAR mmaps.

| tool | what |
|---|---|
| `bar-peek` | read a BAR with 32-bit loads: hexdump, strings, raw |
| `recon.sh` | dump and decode the CCSR: identity, address windows, DDR, PCIe block, both MACs |
| `mdio` | scan, read, write the PHY; watch link state |
| `poke` | one 32-bit read or write with explicit byte order |
| `ddr-window.sh` | retarget BAR1 onto the card's DDR |
| `ddr-dump` | copy a range of card DDR to a file |
| `flash-dump.sh` | dump the 8 MB boot flash via BAR2 |
| `i2c` | drive the SoC's I2C controller: scan, dump |
| `txtest`, `rxtest` | send and receive frames with rings in card DDR |
| `looptest` | MAC loopback at line rate into card DDR, reports overruns |
| `perf.sh` | download, parallel download, upload against Cloudflare, plus counters. `hold` pauses for a browser speedtest |
| `rxdiag.sh` | flood ping with payload check, kernel TCP counters around a download, MAC vs driver frame counts |
| `diag.sh`, `tcpdiag.sh`, `tcpcap.sh` | older diagnostics, kept for reference |

`recon-*/` holds register dumps from the first board this was done on.

### Warning about BAR1

On a freshly booted card, reading BAR1 hangs the PCIe bus and needs a hard reset.
Its endpoint translation register points at card address `0xD0000000`, where nothing
exists. Sandy Bridge treats the resulting completion timeout as fatal. `ddr-window.sh`
retargets BAR1 to DDR first. The driver does the same at probe. Do not read BAR1 or
BAR4 blind. BAR2 is the flash and is safe to read within its first 8 MB.

## Card layout

| | |
|---|---|
| SoC | MPC8308 rev 1.0, core 400 MHz, CSB 133 MHz, big-endian |
| DDR2 | 128 MB at card address 0. The card's own kernel was given 64 MB. |
| Flash | 8 MB 16-bit NOR at `0xF0000000`: HRCW, u-boot, env, Linux uImage (kernel, ext2 ramdisk, FDT) |
| PCIe | endpoint, x1 Gen1. Inbound: BAR0 to CCSR, BAR1 unmapped, BAR2 to flash, BAR4 disabled. Outbound window 0: `0xA0000000` to host 0 (u-boot's). Window 1: this driver's. |
| Write-DMA engine | PEX bridge at CCSR+`0x9800`. Descriptors are five little-endian words, control word first. Max single descriptor tested: 8192 bytes. Notes in `docs/pex-dma-notes.txt`. |
| MAC | eTSEC1 at CCSR+`0x24000`, RGMII. eTSEC2 unused. |
| PHY | Marvell 88E1116R, MDIO address 1, both RGMII delays strapped on |
| Other | I2C at `0x3000` (nothing answers on the tested board), two 16550 UARTs, 32 KB local-bus device at `0xE1ED0000` |

## Roadmap

* Transmit through the PCIe block's read-DMA engine, for gigabit transmit.
* MSI through the PCIe block's mailbox registers, to replace the polling timer.
* MAC address from EEPROM on boards that have one.

## License

GPL-2.0.

Bigfoot's firmware (the flash image and its contents) is their copyright and is not
included. `flash-dump.sh` reads your own card's copy.

## Credits

Mitch, with Claude (Anthropic), September 2026.

References: NXP MPC8308 Reference Manual; u-boot `immap_83xx.h` and
`arch/powerpc/cpu/mpc83xx/pcie.c` for the PCIe block layout; the Linux `gianfar` and
`marvell` drivers.
