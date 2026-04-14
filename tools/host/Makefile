# Build host-side tools for Blackroo Linux
#
# These tools run on the build host (x86_64 Linux) and convert
# kernel binaries into PlayStation-executable formats.
#
# Source attribution:
#   elf2psx.c        — New Blackroo work (GPL v2)
#   addpsexe_initrd.c — New Blackroo work (GPL v2)
#   elf2ecoff.c      — Runix project (GPL v2), from arch/mipsnommu/boot/

# Force host compiler (not cross-compiler)
CC = /usr/bin/gcc
CFLAGS = -Wall -O2
BIN_DIR = ../bin

TOOLS = elf2psx addpsexe_initrd mkmemcard

.PHONY: all clean install

all: $(TOOLS)

elf2psx: elf2psx.c
	$(CC) $(CFLAGS) -o $@ $<

addpsexe_initrd: addpsexe_initrd.c
	$(CC) $(CFLAGS) -o $@ $<

mkmemcard: mkmemcard.c
	$(CC) $(CFLAGS) -o $@ $<

mkinit: mkinit.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	mkdir -p $(BIN_DIR)
	cp $(TOOLS) $(BIN_DIR)/

clean:
	rm -f $(TOOLS)
