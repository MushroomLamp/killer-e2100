CFLAGS ?= -O2 -Wall -Wextra
all: bar-peek mdio poke txtest ddr-dump rxtest
bar-peek: bar-peek.c
mdio: mdio.c
poke: poke.c
txtest: txtest.c
clean:
	rm -f bar-peek mdio poke txtest ddr-dump rxtest
.PHONY: all clean
ddr-dump: ddr-dump.c
rxtest: rxtest.c
