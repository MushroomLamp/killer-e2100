#!/bin/bash
# Install killer_e2100.ko for the running kernel so it autoloads at boot.
# The kernel matches the card's PCI ID against the module's alias table
# (via depmod), so no modules-load.d entry is needed.
# Note: this is tied to the current kernel version. After a kernel update,
# rebuild (make) and run this again. (DKMS would automate that; later.)
set -eu
HERE=$(dirname "$(readlink -f "$0")")
KVER=$(uname -r)
DEST=/lib/modules/$KVER/extra
install -d "$DEST"
install -m 644 "$HERE/killer_e2100.ko" "$DEST/"
depmod -a "$KVER"
echo "installed to $DEST, depmod done"
echo -n "kernel will autoload for: "; modprobe -R "pci:v00001957d0000C006sv00001A56sd00001201bc0Bsc20i00" 2>/dev/null || echo "(alias lookup failed)"
modprobe killer_e2100 && echo "loaded now"
