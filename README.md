# Linux driver for the Bigfoot Killer E2100

A working Linux network driver for the **Bigfoot Networks Killer E2100** (also sold as
Killer 2100 / Killer Xeno Pro), the gaming NIC found on-board the Gigabyte G1.Sniper
series and on PCIe cards. Bigfoot only ever shipped Windows drivers, and the card is
not a normal NIC, so no Linux driver has existed since it launched in 2010.

This one drives the card's ethernet MAC directly and gives you an ordinary `enp7s0`
that NetworkManager, DHCP and everything else treat like any wired port.

## What the card actually is

The E2100 is not an ethernet controller. It is a **Freescale MPC8308 PowerPC SoC**
with 128 MB of DDR2 and 8 MB of flash, sitting on PCIe as an endpoint. It enumerates
with PCI class `0b20` ("Power PC"), not `0200` ("Ethernet"), which is why nothing
binds to it:

```
07:00.0 Power PC: Freescale Semiconductor Inc MPC8308 (rev 10)
        Subsystem: Rivet Networks Bigfoot Killer E2100 Gigabit Ethernet Controller
```

Bigfoot's design ran Linux 2.6.31 *on the card* with a proprietary module that
spoke a mailbox-and-DMA protocol to the Windows driver, and offloaded UDP game
traffic onto the card's own CPU.

The key discovery behind this driver: **BAR0 is the SoC's 1 MB CCSR register
window.** Every peripheral register in the MPC8308 is reachable from the host,
including the eTSEC gigabit MAC, the MDIO block, the PCIe address translation
units, the I2C controller, and the DDR controller. The MPC8308 is fully
documented public silicon. So instead of reverse-engineering Bigfoot's protocol,
the host simply programs the MAC itself, exactly the way the card's own kernel
would have. On the boards examined, the card's firmware never boots past u-boot,
so nothing on the card competes for the hardware.

## How it works

* **BAR0** maps the CCSR. Registers are big-endian, except the PCIe controller
  block at `0x9000`, which is little-endian.
* The **eTSEC1** MAC at CCSR+`0x24000` is driven with the same register sequences
  as the in-tree `gianfar` driver.
* The **Marvell 88E1116R** PHY at MDIO address 1 is handled by phylib, in
  `RGMII_ID` mode (the board straps both clock delays into the PHY).
* **Receive**: the MAC's ring and buffers live in the card's own DDR, which it
  fills at wire speed. Each NAPI poll gathers the frames that arrived, builds a
  descriptor chain for the PCIe block's write-DMA engine, and starts it once. The
  engine streams the frames into a 1 MB coherent region in host RAM as full
  128-byte PCIe writes; a final marker descriptor copies a sequence number after
  the data, and because PCIe posted writes are ordered, seeing it guarantees every
  frame before it has landed. The SoC's PCIe outbound window 1 maps card-local
  `0xB0000000` onto that region and nothing else, so the card cannot address any
  other host memory. That matters because many boards this card ships on (Sandy
  Bridge era) have no IOMMU.
* **Transmit**: the ring and frames also live in card DDR; the host writes them
  through BAR1. This is not for speed, it is for receive: if the MAC has to read
  its transmit descriptors across PCIe, those reads queue behind the write-DMA
  engine's posted writes (PCIe ordering), the MAC's single DMA unit stalls, and
  its receive FIFO overruns. Keeping every MAC access on the card side was the
  change that took receive from ~20% loss to zero at gigabit.
* **Bus tuning**: the card's u-boot leaves the internal bus arbiter at a pipeline
  depth of one outstanding transaction; the driver sets four, as Freescale's own
  boards do.
* **No interrupts**: the MAC's IRQ lines terminate in the card's own interrupt
  controller, which the host cannot see. NAPI is driven by an hrtimer (default
  500 µs, `poll_us=` module parameter). Ping RTT to the gateway is ~0.6 ms.

## Status

| | |
|---|---|
| Link, autoneg, 10/100/1000 | works (phylib + Marvell driver) |
| TX / RX, DHCP, DNS, browsing | works |
| Link | 10/100/1000 negotiated by phylib; gigabit is the default |
| Receive at gigabit | **lossless**: in a 14k-frame TCP download the MAC counted 13,919 frames and the driver delivered 13,919; MAC-internal loopback runs at 987 Mbit into card DDR with zero overruns |
| Throughput | **844 Mbit/s down, 95 up on a browser speedtest**, which is the ISP's cap on this line (a desktop on the same switch gets the same). Single TCP stream from a nearby server: 497 Mbit/s on a 20 MB transfer including slow start. For comparison: v0.2 316, v0.1 15.7, same port |
| MAC address | **locally administered placeholder** (`02:4b:49:4c:4c:52`). The real one is in the card's I2C EEPROM; reading it is in progress |
| ethtool | link settings via phylib, drvinfo |
| Jumbo frames, checksum offload, WoL | no |
| Bigfoot's UDP offload / "Killer" features | never; the card's CPU is not used at all |

### Roadmap

* Transmit is host-PIO into card DDR through a 20-entry ring: plenty for a home
  uplink, not for a gigabit LAN sender. The PCIe block's read-DMA engine (the twin of
  the write engine already in use) is the obvious next step.
* Interrupts: the MAC's IRQs terminate on the card. The PCIe block has inbound and
  outbound mailbox registers that can raise MSI on the host; using them would replace
  the 500 µs timer.
* Real MAC address from the card's EEPROM, where one is populated.

Tested on: Gigabyte G1.Sniper 2 (Z68, on-board E2100), Linux Mint 22.3, kernel 7.0.

## Building and installing

```
cd driver
make                      # builds killer_e2100.ko against the running kernel
sudo ./try.sh             # loads it, waits for DHCP, pings the gateway
sudo rmmod killer_e2100   # unload
```

For a permanent install that survives kernel updates:

```
sudo ./driver/dkms-install.sh          # register with DKMS, build, install, load
sudo ./driver/dkms-install.sh remove   # undo
```

The module is unsigned. On a Secure Boot system you will need to sign it or enroll a
MOK; legacy-BIOS boards (like the G1.Sniper) do not care.

## Tools

The `tools` used to reverse-engineer the card, all userspace, all through sysfs BAR
mmaps. Useful if you have a different board or want to poke at the SoC.

| tool | what |
|---|---|
| `bar-peek` | read a BAR with strict 32-bit loads; hexdump, strings, raw |
| `recon.sh` | dump and decode the CCSR: identity, address windows, DDR, PCIe block, both MACs |
| `mdio` | scan / read / write the PHY, watch link state |
| `poke` | one 32-bit read or write, byte order stated explicitly |
| `ddr-window.sh` | retarget BAR1 onto the card's DDR |
| `ddr-dump` | copy a range of the card's DDR to a file |
| `flash-dump.sh` | dump the 8 MB boot flash (u-boot + Bigfoot's Linux image) via BAR2 |
| `txtest`, `rxtest` | send / receive frames with rings in card DDR; the proofs the driver grew from |
| `i2c` | drive the SoC's I2C controller: scan, dump EEPROMs |

`recon-*/` holds the register dumps from the first board this was done on.

### A warning about BAR1

On a freshly booted card, **reading BAR1 hangs the PCIe bus** and needs a hard reset.
Its endpoint translation register points at card address `0xD0000000`, where
nothing exists, and Sandy Bridge treats the resulting completion timeout as fatal.
`ddr-window.sh` retargets it to DDR first; the driver does the same at probe. Never
read BAR1 (or BAR4, which is disabled) blind. BAR2 is the flash and is safe to read
within its first 8 MB.

## Layout of the card (for the curious)

| | |
|---|---|
| SoC | MPC8308 rev 1.0, core 400 MHz, CSB 133 MHz, big-endian |
| DDR2 | 128 MB at local `0x0`; the card's own kernel was given 64 MB |
| Flash | 8 MB 16-bit NOR at `0xF0000000`: HRCW, u-boot, env, Linux uImage (kernel + ext2 ramdisk + FDT) |
| PCIe | endpoint, x1. Inbound: BAR0→CCSR, BAR1→(void), BAR2→flash, BAR4→disabled. Outbound window 0: `0xA0000000`→host 0 (u-boot's), window 1: ours |
| MAC | eTSEC1 at CCSR+`0x24000`, RGMII. eTSEC2 unused |
| PHY | Marvell 88E1116R, MDIO addr 1, both RGMII delays strapped on |
| Other | I2C at `0x3000` (EEPROM, DS1339 RTC), two 16550 UARTs (probably on test pads), 32 KB local-bus device at `0xE1ED0000` |

## License

GPL-2.0. The driver is a Linux kernel module and is licensed accordingly; the tools
are under the same license for simplicity.

Bigfoot's firmware (the flash image and the files inside it) is their copyright and is
deliberately not included here. `flash-dump.sh` will read your own card's copy.

## Credits

Written by Mitch, with Claude (Anthropic) doing the register-level archaeology,
in one day in September 2026, starting from "I don't see my NIC listed, do we need
drivers?" and ending at the ISP's speed cap.

Reference material: NXP MPC8308 Reference Manual; u-boot `immap_83xx.h` and
`arch/powerpc/cpu/mpc83xx/pcie.c` for the PCIe block layout; the Linux `gianfar`
and `marvell` drivers.
