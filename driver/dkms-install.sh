#!/bin/bash
# Register the driver with DKMS so it is rebuilt automatically for every new
# kernel and loads at boot (the kernel matches the card's PCI ID against the
# module's alias table). Run as root. Undo with: dkms-install.sh remove
set -eu
HERE=$(dirname "$(readlink -f "$0")")
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$HERE/dkms.conf")
NAME=killer-e2100
SRC=/usr/src/$NAME-$VER
if [ "${1:-}" = remove ]; then
	modprobe -r killer_e2100 2>/dev/null || true
	dkms remove "$NAME/$VER" --all || true
	rm -rf "$SRC"
	echo "removed"; exit 0
fi
rm -rf "$SRC"; mkdir -p "$SRC"
cp "$HERE/killer_e2100.c" "$HERE/Makefile" "$HERE/dkms.conf" "$SRC/"
dkms add "$NAME/$VER" 2>/dev/null || true
dkms build "$NAME/$VER"
dkms install "$NAME/$VER"
modprobe killer_e2100
echo "installed via DKMS; it will rebuild itself on kernel updates and load at boot"
dkms status "$NAME"
