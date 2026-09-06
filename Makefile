CFLAGS ?= -O2 -Wall -Wextra
all: bar-peek mdio poke txtest ddr-dump rxtest i2c looptest
bar-peek: bar-peek.c
mdio: mdio.c
poke: poke.c
txtest: txtest.c
clean:
	rm -f bar-peek mdio poke txtest ddr-dump rxtest i2c looptest
.PHONY: all clean
ddr-dump: ddr-dump.c
rxtest: rxtest.c
i2c: i2c.c
looptest: looptest.c
