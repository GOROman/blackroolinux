#
# Makefile for the PlayStation family specific parts of the kernel
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#

# BLACKROO: add -mno-abicalls -fno-pic to assembly too
.S.s:
	$(CPP) $(AFLAGS) -G 0 -mno-abicalls -fno-pic $< -o $@
.S.o:
	$(CC) $(AFLAGS) -G 0 -mno-abicalls -fno-pic -c $< -o $@

O_TARGET := ps.o

all: ps.o

# BLACKROO: PlayStation machine support
obj-y	 := int-handler.o setup.o irq.o time.o reset.o promcon.o

int-handler.o:	int-handler.S

include $(TOPDIR)/Rules.make
