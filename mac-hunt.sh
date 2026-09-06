#!/bin/bash
# u-boot reads the card's MAC from an EEPROM at boot ("EEPROM MAC Address:")
# and normally stores it as the "ethaddr" environment variable in RAM.
# u-boot relocates itself to the top of the 128 MB DDR, so dump the top 2 MB
# and look. Also scans the I2C bus. Read-only apart from BAR1 retargeting.
set -u
HERE=$(dirname "$(readlink -f "$0")")
"$HERE/ddr-dump" 0x07E00000 0x200000 > "$HERE/card-ddr-top.bin" || exit 1
echo "==== ethaddr / EEPROM strings in u-boot's RAM ===="
strings -n 6 "$HERE/card-ddr-top.bin" | grep -iE 'ethaddr|eeprom|mac addr|serial#|board' | sort -u | head -20
echo "==== MAC-looking strings ===="
strings -n 17 "$HERE/card-ddr-top.bin" | grep -oE '([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}' | sort | uniq -c | sort -rn | head
echo; echo "==== I2C scan ===="
"$HERE/i2c" scan
