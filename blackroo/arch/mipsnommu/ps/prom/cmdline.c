/*
 * cmdline.c: the command line the kernel boots with.
 *
 * The PlayStation BIOS gives a PS-EXE no argv, so there is nothing to read
 * from a PROM. Two sources, in order:
 *
 *   1. A loader-supplied line at 0x80000180 (the BIOS argument area).
 *      kloader's kernel_launch() writes the command line there before
 *      jumping to us — see bootloader/src/kernel.c. It is only trusted when
 *      it starts with the magic "BRCL" and is NUL-terminated within bounds,
 *      so random RAM contents after a cold boot can't be mistaken for one.
 *
 *   2. The compiled-in default below (plain BIOS CD boot: the BIOS hands us
 *      nothing at all).
 *
 * Attribution: Blackroo Linux (2026). Original Runix had no cmdline.
 * Reference: kernel Documentation/kernel-parameters.txt
 */
#include <linux/init.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include <asm/bootinfo.h>

char arcs_cmdline[COMMAND_LINE_SIZE];

/*
 * Default command line.
 *
 *   brmon                — enter the in-kernel serial monitor before the
 *                          root filesystem is mounted. This is what makes a
 *                          kernel-only CD (no initrd) useful: you get a
 *                          prompt on SIO1 with no userspace at all.
 *   console=ttyS0,115200 — serial first, so panics reach the FTDI cable
 *   console=tty0         — GPU console on the TV as well
 *   root=/dev/ram0       — used only if you type "cont" at the monitor and
 *                          an initrd was embedded
 *   init=/bin/sh         — ditto
 */
#ifndef CONFIG_CMDLINE
#define CONFIG_CMDLINE \
	"brmon root=/dev/ram0 init=/bin/sh console=ttyS0,115200 console=tty0"
#endif

#define BOOT_ARG_ADDR	0x80000180
#define BOOT_ARG_MAGIC	"BRCL"

void __init prom_init_cmdline(int argc, char **argv, unsigned long magic)
{
	const char *loader = (const char *)BOOT_ARG_ADDR;
	int i;

	arcs_cmdline[0] = 0;

	if (!strncmp(loader, BOOT_ARG_MAGIC, 4)) {
		const char *line = loader + 4;

		for (i = 0; i < COMMAND_LINE_SIZE - 1; i++) {
			if (line[i] == '\0') {
				strcpy(arcs_cmdline, line);
				return;
			}
			/* anything unprintable means this isn't a command line */
			if (line[i] < 0x20 || line[i] > 0x7e)
				break;
		}
		arcs_cmdline[0] = 0;
	}

	strcpy(arcs_cmdline, CONFIG_CMDLINE);
}
