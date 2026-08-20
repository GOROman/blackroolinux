/*
 * brmon.c — Blackroo Monitor: an interactive poke/peek shell on SIO1.
 *
 * Why this exists: this kernel has CONFIG_BINFMT_FLAT only (no ELF loader
 * is built — see fs/Makefile), so the prebuilt MIPS ELF BusyBox cannot be
 * exec'd and the initrd path has never reached a prompt. The monitor gives
 * an interactive prompt on the PlayStation's serial port *without* any
 * userspace at all: it runs in kernel context, so a kernel-only PS-EXE
 * (no initrd) fits a stock 2 MB console and still lets you read and write
 * every register on the machine.
 *
 * It talks to SIO1 directly rather than through the console/tty layer, so
 * it works whether or not a tty ever came up. Registers and the RTS
 * requirement come from docs/13-SIO1-HARDWARE-RESEARCH.md:
 *   - MODE must use the 16x baud multiplier (SIO_BRS16)
 *   - CTRL must assert RTS, or real silicon refuses to transmit
 *   Source: psx-spx Serial Interfaces — https://psx-spx.consoledev.net/serialinterfacessio/
 *
 * Entry: brmon_main(), called from init/main.c when the kernel command line
 * contains "brmon", and as the fallback when no init program can be exec'd.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/locks.h>

#include <asm/io.h>
#include <asm/ps/sio.h>

#define MON_LINE_MAX	128
#define MON_ARGS_MAX	8
#define MON_TX_SPINS	400000		/* give up on a byte if host never CTSes */

#define GPU_STAT_REG	0x1814		/* 0x1f801814 */
#define INT_STAT_REG	0x1070
#define INT_MASK_REG	0x1074
#define DMA_DPCR_REG	0x10f0
#define RAM_SIZE_REG	0x1060
#define JOY_STAT_REG	0x1044

static char mon_line[MON_LINE_MAX];
static char mon_buf[192];
static unsigned long mon_last_addr = 0x80010000;

/* ------------------------------------------------------------------ */
/* Raw SIO1 I/O                                                        */
/* ------------------------------------------------------------------ */

static void mon_tty(int c);

static void mon_putc(int c)
{
	long spins = 0;

	mon_tty(c);

	if (c == '\n')
		mon_putc('\r');

	while ((inw(SIO_STAT_REG) & SIO_RFW) == 0)
		if (++spins > MON_TX_SPINS)
			return;			/* nothing listening — drop it */

	outb(c, SIO_DATA_REG);
}

/*
 * Echo to the kernel console (GPU/TV) as well as the serial port.
 *
 * printk goes to every registered console, including the PSX GPU console, so
 * this puts the monitor on the television — which is the only output you have
 * when the serial link is being unhelpful. Line-buffered, because printk emits
 * per call and a per-character printk would be unusable.
 */
static int mon_tty_echo = 1;
static char mon_lbuf[128];
static int mon_lpos;

static void mon_tty(int c)
{
	if (!mon_tty_echo || c == '\r')
		return;

	if (c == '\n' || mon_lpos >= (int)sizeof(mon_lbuf) - 1) {
		mon_lbuf[mon_lpos] = '\0';
		printk("%s\n", mon_lbuf);
		mon_lpos = 0;
		if (c != '\n')
			mon_lbuf[mon_lpos++] = c;
		return;
	}
	mon_lbuf[mon_lpos++] = c;
}

static void mon_puts(const char *s)
{
	while (*s)
		mon_putc(*s++);
}

static void mon_printf(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsprintf(mon_buf, fmt, args);
	va_end(args);
	mon_puts(mon_buf);
}

static int mon_getc(void)
{
	/* RTS stays asserted the whole time we are waiting: the host will
	 * not raise our CTS otherwise and the link stalls in both
	 * directions (docs/13). */
	outw(inw(SIO_CTRL_REG) | SIO_RTS | SIO_RX | SIO_TX, SIO_CTRL_REG);

	while ((inw(SIO_STAT_REG) & SIO_RFR) == 0)
		;				/* the monitor owns the CPU */

	return inb(SIO_DATA_REG) & 0xff;
}

/* Is there a byte waiting from the host? Non-blocking, unlike mon_getc(). */
static int mon_pending(void)
{
	return (inw(SIO_STAT_REG) & SIO_RFR) != 0;
}

static void mon_serial_init(void)
{
	outw(SIO_B11520, SIO_RATE_REG);
	outw(SIO_BRS16 | SIO_CHR8 | SIO_SB1, SIO_MODE_REG);
	outw(SIO_TX | SIO_RX | SIO_DTR | SIO_RTS, SIO_CTRL_REG);
}

static void mon_getline(void)
{
	int n = 0, c;

	for (;;) {
		c = mon_getc();

		if (c == '\r' || c == '\n') {
			mon_putc('\n');
			break;
		}
		if (c == 0x08 || c == 0x7f) {		/* BS / DEL */
			if (n > 0) {
				n--;
				mon_puts("\b \b");
			}
			continue;
		}
		if (c == 0x03) {			/* ^C */
			mon_puts("^C\n");
			n = 0;
			break;
		}
		if (c < 0x20 || c > 0x7e)
			continue;
		if (n < MON_LINE_MAX - 1) {
			mon_line[n++] = c;
			mon_putc(c);
		}
	}
	mon_line[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static unsigned long mon_hex(const char *s, int *ok)
{
	unsigned long v = 0;
	int digits = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;

	while (*s) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else {
			if (ok)
				*ok = 0;
			return 0;
		}
		v = (v << 4) | d;
		digits++;
		s++;
	}
	if (ok)
		*ok = digits ? 1 : 0;
	return v;
}

/*
 * Turn a bare address into one the CPU can actually use:
 *   0x1f000000-0x1fffffff  hardware registers -> KSEG1 (uncached)
 *   below 8 MB             physical RAM       -> KSEG0 (cached)
 * Anything already in KSEG0/KSEG1 is left alone.
 */
static unsigned long mon_fixaddr(unsigned long a)
{
	if (a >= 0x1f000000 && a < 0x20000000)
		return a | 0xa0000000;
	if (a < 0x00800000)
		return a | 0x80000000;
	return a;
}

static int mon_split(char *line, char **argv)
{
	int argc = 0;

	while (*line && argc < MON_ARGS_MAX) {
		while (*line == ' ' || *line == '\t')
			*line++ = '\0';
		if (!*line)
			break;
		argv[argc++] = line;
		while (*line && *line != ' ' && *line != '\t')
			line++;
	}
	return argc;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void cmd_help(void)
{
	mon_puts("\n"
		 "  md <addr> [words]     dump memory (default 16 words)\n"
		 "  peek <addr> [b|h|w]   read one value\n"
		 "  poke <addr> <val> [b|h|w]\n"
		 "                        write one value\n"
		 "  fill <addr> <words> <val>\n"
		 "                        fill words with a value\n"
		 "  ram [probe]           RAM_SIZE + page count ('probe' WRITES)\n"
		 "  tty on|off            echo the monitor to the TV as well\n"
		 "  hw                    GPU / IRQ / DMA / SIO / JOY registers\n"
		 "  cpu                   COP0 PRId, SR, Cause, EPC\n"
		 "  mem                   kernel layout and page counts\n"
		 "  card [rd <blk> [slot]] memory cards: geometry, read a block\n"
		 "  kbd [watch|scan] [slot] [addr]  Lightspan keyboard on SIO0\n"
		 "  cd [init|stat|rd <lba>|id]  CD-ROM probe (polled, PIO)\n"
		 "  sio0 [slot] [addr] [n] polled SIO0 probe, n bytes (max 40)\n"
		 "  blk <maj> <min> <blk>  read a block via the block layer\n"
		 "  blktest <maj> <min> <blk>  write a signature and read it back\n"
		 "  reboot                jump to the BIOS reset vector\n"
		 "  cont                  leave the monitor, carry on booting\n"
		 "\n"
		 "  Addresses are hex. Bare 0x1f80xxxx is read uncached (KSEG1),\n"
		 "  bare RAM addresses are read through KSEG0.\n\n");
}

static void cmd_md(int argc, char **argv)
{
	unsigned long addr, i, words = 16;
	int ok = 1;

	if (argc > 1) {
		addr = mon_hex(argv[1], &ok);
		if (!ok) {
			mon_puts("bad address\n");
			return;
		}
		mon_last_addr = addr;
	}
	if (argc > 2) {
		words = mon_hex(argv[2], &ok);
		if (!ok || words == 0)
			words = 16;
		if (words > 1024)
			words = 1024;
	}

	addr = mon_fixaddr(mon_last_addr);

	for (i = 0; i < words; i += 4) {
		unsigned long j, n = words - i < 4 ? words - i : 4;

		mon_printf("%08lx: ", addr + i * 4);
		for (j = 0; j < n; j++)
			mon_printf("%08lx ",
				   *(volatile unsigned long *)(addr + (i + j) * 4));
		for (; j < 4; j++)
			mon_puts("         ");

		mon_puts(" |");
		for (j = 0; j < n * 4; j++) {
			int c = *(volatile unsigned char *)(addr + i * 4 + j);

			mon_putc(c >= 0x20 && c < 0x7f ? c : '.');
		}
		mon_puts("|\n");
	}
	mon_last_addr += words * 4;
}

static int mon_width(int argc, char **argv, int idx)
{
	if (argc > idx) {
		if (argv[idx][0] == 'b')
			return 1;
		if (argv[idx][0] == 'h')
			return 2;
	}
	return 4;
}

static void cmd_peek(int argc, char **argv)
{
	unsigned long addr, val;
	int ok = 1, w;

	if (argc < 2) {
		mon_puts("usage: peek <addr> [b|h|w]\n");
		return;
	}
	addr = mon_fixaddr(mon_hex(argv[1], &ok));
	if (!ok) {
		mon_puts("bad address\n");
		return;
	}
	w = mon_width(argc, argv, 2);

	if (w == 1)
		val = *(volatile unsigned char *)addr;
	else if (w == 2)
		val = *(volatile unsigned short *)(addr & ~1UL);
	else
		val = *(volatile unsigned long *)(addr & ~3UL);

	mon_printf("%08lx: %0*lx\n", addr, w * 2, val);
}

static void cmd_poke(int argc, char **argv)
{
	unsigned long addr, val;
	int ok = 1, ok2 = 1, w;

	if (argc < 3) {
		mon_puts("usage: poke <addr> <val> [b|h|w]\n");
		return;
	}
	addr = mon_fixaddr(mon_hex(argv[1], &ok));
	val = mon_hex(argv[2], &ok2);
	if (!ok || !ok2) {
		mon_puts("bad address or value\n");
		return;
	}
	w = mon_width(argc, argv, 3);

	if (w == 1)
		*(volatile unsigned char *)addr = val;
	else if (w == 2)
		*(volatile unsigned short *)(addr & ~1UL) = val;
	else
		*(volatile unsigned long *)(addr & ~3UL) = val;

	mon_printf("%08lx <- %0*lx\n", addr, w * 2, val);
}

static void cmd_fill(int argc, char **argv)
{
	unsigned long addr, words, val, i;
	int ok = 1, ok2 = 1, ok3 = 1;

	if (argc < 4) {
		mon_puts("usage: fill <addr> <words> <val>\n");
		return;
	}
	addr = mon_fixaddr(mon_hex(argv[1], &ok)) & ~3UL;
	words = mon_hex(argv[2], &ok2);
	val = mon_hex(argv[3], &ok3);
	if (!ok || !ok2 || !ok3) {
		mon_puts("bad argument\n");
		return;
	}
	for (i = 0; i < words; i++)
		*(volatile unsigned long *)(addr + i * 4) = val;

	mon_printf("filled %lx words at %08lx with %08lx\n", words, addr, val);
}

/*
 * Non-destructive mirror probe: RAM below the installed size answers at
 * its own address, above it the bus mirrors back to low RAM. Values are
 * saved and restored, so this is safe to run on a live kernel — except
 * that it writes to addresses the kernel may own, hence the KSEG1
 * (uncached) accesses and immediate restore.
 */
static void cmd_ram(int argc, char **argv)
{
	volatile unsigned long *base = (volatile unsigned long *)0xa0000000;
	volatile unsigned long *m2 = (volatile unsigned long *)0xa0200000;
	volatile unsigned long *m4 = (volatile unsigned long *)0xa0400000;
	unsigned long reg = *(volatile unsigned long *)0xbf801060;
	unsigned long s_base, s_m2, s_m4;
	const char *verdict;

	/* Read-only report — always safe. */
	mon_printf("RAM_SIZE (0x1f801060) = %08lx  ", reg);
	mon_puts(reg == 0x0888 ? "[2 MB setting]\n" :
		 reg == 0x0988 ? "[4 MB setting]\n" :
		 reg == 0x0b88 ? "[8 MB setting]\n" : "[non-standard]\n");
	mon_printf("kernel believes       : %lu KB (%lu pages)\n",
		   (unsigned long)num_physpages * (PAGE_SIZE >> 10),
		   (unsigned long)num_physpages);

	if (argc < 2 || strcmp(argv[1], "probe") != 0) {
		mon_puts("mirror probe          : skipped "
			 "(\"ram probe\" to run it)\n");
		return;
	}

	/*
	 * The probe WRITES to 0 MB, 2 MB and 4 MB through KSEG1 and restores
	 * what it found. On a machine smaller than 4 MB those addresses alias
	 * onto real RAM the kernel is using, and the restore cannot be correct
	 * when two of the three are the same location — which is why this is
	 * no longer the default. It killed a live kernel on a 2 MB console.
	 */
	mon_puts("mirror probe          : WRITES to live RAM, may crash...\n");

	s_base = *base; s_m2 = *m2; s_m4 = *m4;

	*base = 0xdeadbeef;
	*m2 = 0xcafebabe;
	*m4 = 0x12345678;

	if (*base != 0xdeadbeef)
		verdict = "2 MB or less (4 MB write wrapped to 0)";
	else if (*m2 == 0xcafebabe && *m4 == 0x12345678)
		verdict = "8 MB (no mirroring at 2 or 4 MB)";
	else if (*m2 == 0xcafebabe)
		verdict = "4 MB (mirrors at 4 MB)";
	else
		verdict = "2 MB (mirrors at 2 MB)";

	*m4 = s_m4;
	*m2 = s_m2;
	*base = s_base;

	mon_printf("mirror probe          : %s\n", verdict);
}

static void cmd_hw(void)
{
	mon_printf("GPUSTAT  0x1f801814 = %08lx\n",
		   *(volatile unsigned long *)0xbf801814);
	mon_printf("I_STAT   0x1f801070 = %04x\n", inw(INT_STAT_REG));
	mon_printf("I_MASK   0x1f801074 = %04x\n", inw(INT_MASK_REG));
	mon_printf("DPCR     0x1f8010f0 = %08lx\n",
		   *(volatile unsigned long *)0xbf8010f0);
	mon_printf("JOY_STAT 0x1f801044 = %04x\n", inw(JOY_STAT_REG));
	mon_printf("SIO_STAT 0x1f801054 = %04x  MODE %04x  CTRL %04x  RATE %04x\n",
		   inw(SIO_STAT_REG), inw(SIO_MODE_REG),
		   inw(SIO_CTRL_REG), inw(SIO_RATE_REG));
}

static void cmd_cpu(void)
{
	unsigned long prid, sr, cause, epc;

	__asm__ __volatile__("mfc0 %0, $15" : "=r"(prid));
	__asm__ __volatile__("mfc0 %0, $12" : "=r"(sr));
	__asm__ __volatile__("mfc0 %0, $13" : "=r"(cause));
	__asm__ __volatile__("mfc0 %0, $14" : "=r"(epc));

	mon_printf("PRId  = %08lx  (rev %lu.%lu)\n",
		   prid, (prid >> 4) & 0xf, prid & 0xf);
	mon_printf("SR    = %08lx  IEc=%lu KUc=%lu IsC=%lu BEV=%lu\n",
		   sr, sr & 1, (sr >> 1) & 1, (sr >> 16) & 1, (sr >> 22) & 1);
	mon_printf("Cause = %08lx  ExcCode=%lu\n", cause, (cause >> 2) & 0x1f);
	mon_printf("EPC   = %08lx\n", epc);
}

static void cmd_mem(void)
{
	extern char _ftext, _end;
	extern unsigned long initrd_start, initrd_end;

	mon_printf("kernel text starts   %08lx\n", (unsigned long)&_ftext);
	mon_printf("kernel _end          %08lx\n", (unsigned long)&_end);
	mon_printf("physical pages       %lu (%lu KB)\n",
		   (unsigned long)num_physpages,
		   (unsigned long)num_physpages * (PAGE_SIZE >> 10));
	mon_printf("free pages           %lu\n", (unsigned long)nr_free_pages());
#ifdef CONFIG_BLK_DEV_INITRD
	mon_printf("initrd               %08lx..%08lx\n",
		   initrd_start, initrd_end);
#endif
	mon_printf("jiffies              %lu\n", jiffies);
}

/*
 * Memory cards.
 *
 * bu.c registers its geometry into the block layer's blk_size[]/blksize_size[]
 * (its own bu_sizes[] is static), and by the time the monitor runs
 * do_initcalls() has already probed the slots — so this reports what the
 * driver actually found, not a fresh probe.
 *
 * With CONFIG_PSX_LARGE_CARD the cards are joined into one device on
 * BU_LARGE_MAJOR; the individual cards live on BU_MAJOR.
 */
static void cmd_card_format(int argc, char **argv);

static void cmd_card(int argc, char **argv)
{
	int major = BU_LARGE_MAJOR;
	int minor = 0;
	unsigned long block;
	struct buffer_head *bh;
	int ok = 1;

	if (argc > 1 && !strcmp(argv[1], "raw"))
		major = BU_MAJOR;

	if (argc < 2 || !strcmp(argv[1], "raw")) {
		int m;

		mon_printf("joined device (major %d):\n", BU_LARGE_MAJOR);
		if (blk_size[BU_LARGE_MAJOR])
			mon_printf("  /dev/bul  %d KB\n",
				   blk_size[BU_LARGE_MAJOR][0]);
		else
			mon_puts("  not registered\n");

		mon_printf("individual cards (major %d):\n", BU_MAJOR);
		if (blk_size[BU_MAJOR]) {
			for (m = 0; m < 2; m++)
				mon_printf("  /dev/bu%d  %d KB%s\n", m,
					   blk_size[BU_MAJOR][m],
					   blk_size[BU_MAJOR][m] ? "" : "  (empty)");
		} else {
			mon_puts("  not registered\n");
		}
		mon_puts("\n  card rd <block>          read a 1 KB block and dump it\n"
			 "  card format <slot> yes   write the Blackroo header (ERASES)\n");
		return;
	}

	if (!strcmp(argv[1], "format")) {
		cmd_card_format(argc, argv);
		return;
	}

	if (!strcmp(argv[1], "rd")) {
		if (argc < 3) {
			mon_puts("usage: card rd <block> [slot]\n");
			return;
		}
		block = mon_hex(argv[2], &ok);
		if (!ok) {
			mon_puts("bad block number\n");
			return;
		}
		if (argc > 3) {
			minor = (int)mon_hex(argv[3], &ok);
			major = BU_MAJOR;
		}

		mon_printf("reading block %lu from major %d minor %d ...\n",
			   block, major, minor);

		/* process context here, so sleeping in the block layer is fine */
		bh = bread(MKDEV(major, minor), block, 1024);
		if (!bh) {
			mon_puts("read FAILED (no card, or the driver said no)\n");
			return;
		}
		{
			int i, j;

			for (i = 0; i < 128; i += 16) {
				mon_printf("%04x: ", i);
				for (j = 0; j < 16; j++)
					mon_printf("%02x ",
						   (unsigned char)bh->b_data[i + j]);
				mon_puts(" |");
				for (j = 0; j < 16; j++) {
					int c = (unsigned char)bh->b_data[i + j];

					mon_putc(c >= 0x20 && c < 0x7f ? c : '.');
				}
				mon_puts("|\n");
			}
		}
		brelse(bh);
		return;
	}

	mon_printf("unknown: card %s\n", argv[1]);
}

/* ------------------------------------------------------------------ */
/* SIO0 (controller / memory card) low-level probe                     */
/* ------------------------------------------------------------------ */

/*
 * bu.c drives the cards from the SIO0 acknowledge interrupt, which tells us
 * nothing about *where* a probe fails — it either completes or the card is
 * "not found". This is a polled probe with explicit delays: it selects a
 * slot, clocks out the memory-card address byte 0x81 and the read command,
 * and prints every byte that comes back plus the ACK line state.
 *
 * A working card answers:  81 -> (idle)   52 'R' -> 5A   .. -> 5D
 * (0x5A5D is the memory-card ID; a controller answers 0x41xx instead.)
 *
 * Registers, psx-spx Controllers and Memory Cards:
 *   0x1f801040 JOY_TX_DATA / JOY_RX_DATA
 *   0x1f801044 JOY_STAT   bit1 = RX FIFO not empty, bit7 = /ACK level
 *   0x1f801048 JOY_MODE   0x000D = 8-bit, MUL1
 *   0x1f80104a JOY_CTRL   bit0 TXEN, bit1 /JOYn, bit2 RXEN, bit13 slot
 *   0x1f80104e JOY_BAUD   0x0088 -> ~250 kHz
 */
#define JOY_DATA  (*(volatile unsigned char  *)0xbf801040)
#define JOY_STAT_ (*(volatile unsigned long  *)0xbf801044)
#define JOY_MODE_ (*(volatile unsigned short *)0xbf801048)
#define JOY_CTRL_ (*(volatile unsigned short *)0xbf80104a)
#define JOY_BAUD_ (*(volatile unsigned short *)0xbf80104e)

/*
 * BLACKROO 2026-08-21: the monitor is no longer the only thing on SIO0.
 *
 * drivers/char/psxkbd.c polls this bus from a timer, so `sio0`, `kbd` and
 * `card format` have to take their turn like everyone else - otherwise the
 * monitor's own probes are the thing corrupting what they are trying to
 * measure. Defined in drivers/block/bu.c, which owns the lock.
 *
 * The monitor runs with the machine otherwise stopped, so it is fine to spin
 * here: whoever holds the bus is a timer that will finish in milliseconds.
 */
extern int psx_sio0_trylock(void);
extern void psx_sio0_unlock(void);

static void sio0_delay(int n);

static int mon_sio0_grab(void)
{
	int spins;

	for (spins = 0; spins < 2000; spins++) {
		if (psx_sio0_trylock())
			return 1;
		sio0_delay(2000);
	}

	mon_puts("  SIO0 is busy (the keyboard driver holds it) - try again\n");
	return 0;
}

static void sio0_delay(int n)
{
	volatile int i;

	for (i = 0; i < n; i++)
		;
}

/* Clock one byte out and back. Returns the received byte, or -1 on timeout. */
static int sio0_xfer(unsigned char out, int *acked)
{
	long spins;
	int rx;

	/* wait for TX ready */
	for (spins = 0; !(JOY_STAT_ & 0x1); spins++)
		if (spins > 100000)
			return -1;

	JOY_DATA = out;

	/* wait for a byte back */
	for (spins = 0; !(JOY_STAT_ & 0x2); spins++)
		if (spins > 100000)
			return -1;

	rx = JOY_DATA;

	/*
	 * The card pulls /ACK low for a few microseconds after each byte.
	 * Sample it for a while rather than once — this is the signal that
	 * tells a real card apart from an empty slot.
	 */
	*acked = 0;
	for (spins = 0; spins < 20000; spins++) {
		if (JOY_STAT_ & 0x80) {
			*acked = 1;
			break;
		}
	}

	sio0_delay(2000);
	return rx & 0xff;
}

static void cmd_sio0(int argc, char **argv)
{
	unsigned char seq[40];
	int slot = 0, i, ack, rx, n = 10;
	unsigned long addr = 0x81;

	if (argc > 1) {
		int ok = 1;

		slot = (int)mon_hex(argv[1], &ok);
		if (!ok || (slot != 0 && slot != 1)) {
			mon_puts("usage: sio0 [0|1] [addr]\n"
				 "   slot 0 = port 1, slot 1 = port 2\n"
				 "   addr 81..84 = memory card floors,\n"
				 "        01..04 = controller/keyboard floors\n");
			return;
		}
	}
	if (argc > 2) {
		int ok = 1;

		addr = mon_hex(argv[2], &ok);
		if (!ok)
			addr = 0x81;
	}

	/*
	 * First byte addresses the device, the rest clock the reply out.
	 * 0x81 = memory card, 0x01 = controller. A multitap answers the
	 * controller address with a long burst covering all four ports,
	 * which is why we clock out 10 bytes rather than 4.
	 */
	if (argc > 3) {
		int ok2 = 1;
		unsigned long want = mon_hex(argv[3], &ok2);

		if (ok2 && want >= 4 && want <= 40)
			n = (int)want;
	}

	/*
	 * Command byte by address CLASS, not by the single value 0x01.
	 *
	 * A multitap addresses its four floors as 0x01..0x04 for controllers and
	 * 0x81..0x84 for memory cards - exactly what bu.c does with 0x81+floor.
	 * This used to send 0x52 (the card read) to anything that was not
	 * literally 0x01, so probing a keyboard in tap port B asked it to read a
	 * memory card sector and learned nothing.
	 */
	seq[0] = (unsigned char)addr;
	seq[1] = (addr >= 0x01 && addr <= 0x04) ? 0x42 : 0x52;
	for (i = 2; i < n; i++)
		seq[i] = 0x00;

	mon_printf("probing SIO0 slot %d addr %02lx (polled, no interrupts)\n",
		   slot, addr);

	if (!mon_sio0_grab())
		return;

	/* Deselect, reprogram, then select — with settling time between. */
	JOY_CTRL_ = 0;
	sio0_delay(20000);
	JOY_MODE_ = 0x000d;
	JOY_BAUD_ = 0x0088;
	JOY_CTRL_ = 0x0003 | (slot ? 0x2000 : 0);	/* TXEN | /JOYn | slot */
	sio0_delay(20000);				/* card needs time after select */

	mon_printf("  CTRL=%04x MODE=%04x BAUD=%04x STAT=%08lx\n",
		   JOY_CTRL_, JOY_MODE_, JOY_BAUD_, JOY_STAT_);

	for (i = 0; i < n; i++) {
		rx = sio0_xfer(seq[i], &ack);
		if (rx < 0) {
			mon_printf("  byte %d TX %02x -> TIMEOUT\n", i, seq[i]);
			break;
		}
		mon_printf("  byte %d TX %02x -> RX %02x  ack=%s\n",
			   i, seq[i], rx, ack ? "YES" : "no");
		if (!ack && i >= 1 && i < 3)
			break;		/* nothing answered the address at all */
	}

	JOY_CTRL_ = 0;		/* leave the bus deselected */
	psx_sio0_unlock();
	mon_puts("  card answers 5A 5D | controller 41 5A | nothing = FF/no ack\n");
}

/* ------------------------------------------------------------------ */
/* Memory card format — writes the Blackroo header block                */
/* ------------------------------------------------------------------ */

/*
 * Why this exists: bu.c only claims a card whose block 0 starts with the
 * Blackroo id 0x1234 (bu_read_first_block -> "bad card id"), so a stock Sony
 * card is always reported "not found". The driver cannot write the header
 * either, because it refuses to touch a card it has not already accepted.
 * So the header has to be written from here, with the same polled SIO0 code
 * the probe uses.
 *
 * Write sequence, psx-spx Memory Card Read/Write Commands:
 *
 *   send 81h        addr (+floor for a multitap sub-port)
 *   send 57h 'W'    -> reply to 81h
 *   send MSB        -> FLAG
 *   send LSB        -> 5Ah
 *   send data[0]    -> 5Dh
 *   send data[1..]  -> data echo
 *   send CHK        (MSB ^ LSB ^ all data bytes)
 *   send 00h        -> 5Ch
 *   send 00h        -> 5Dh
 *   send 00h        -> end byte: 47h 'G' good, 4Eh 'N' bad checksum, FFh bad sector
 *
 * Replies lag one byte behind the sends, which is why the checks below look
 * offset by one — confirmed against the live card in the probe output.
 *
 * DESTRUCTIVE: block 0 is the card's directory. Anything on the card is lost.
 */

#define MC_SECTOR_SIZE 128

/* opening replies of the last write, for diagnosis */
static unsigned char mc_head[6];

/* trailing responses of the last write, for diagnosis */
#define MC_TAIL 8
static unsigned char mc_tail[MC_TAIL];

/*
 * Transfer without the ACK sampling loop.
 *
 * sio0_xfer() spends up to ~2.4ms per byte watching /ACK, which is fine for a
 * ten-byte probe but fatal for a 133-byte write: a memory card aborts the
 * transaction if the bus stalls between bytes, and the card then answers the
 * write with FFh "bad sector". This keeps the gap short.
 */
static int sio0_xfer_fast(unsigned char out)
{
	long spins;
	int rx;

	for (spins = 0; !(JOY_STAT_ & 0x1); spins++)
		if (spins > 100000)
			return -1;

	JOY_DATA = out;

	for (spins = 0; !(JOY_STAT_ & 0x2); spins++)
		if (spins > 100000)
			return -1;

	rx = JOY_DATA & 0xff;

	/*
	 * Wait for the card's acknowledge before clocking the next byte —
	 * /ACK is the flow control on this bus. Bounded to a few hundred
	 * microseconds: the card answers in ~100us, and waiting the full
	 * sampling loop used by the probe (~2.4ms per byte) stalls a 133-byte
	 * write long enough for the card to abort it. Not waiting at all is
	 * worse: the card is then clocked faster than it can accept and drops
	 * out mid-transfer.
	 */
	for (spins = 0; spins < 5000; spins++)
		if (JOY_STAT_ & 0x80)
			break;

	sio0_delay(100);
	return rx;
}

static void mc_select(int slot)
{
	JOY_CTRL_ = 0;
	sio0_delay(20000);
	JOY_MODE_ = 0x000d;
	JOY_BAUD_ = 0x0088;
	JOY_CTRL_ = 0x0003 | ((slot >> 2) & 1 ? 0x2000 : 0);
	sio0_delay(20000);
}

static void mc_deselect(void)
{
	JOY_CTRL_ = 0;
	sio0_delay(20000);
}

/* Returns the memory card's end byte, or -1 on a transfer timeout. */
static int mc_write_sector(int slot, unsigned int sector,
			   const unsigned char *buf)
{
	unsigned char addr = 0x81 + (slot & 3);
	unsigned char msb = (sector >> 8) & 0xff;
	unsigned char lsb = sector & 0xff;
	unsigned char chk = msb ^ lsb;
	int i, rx, end;

	mc_select(slot);

	/* opening handshake — capture what the card answers. A healthy card
	 * replies FLAG to 'W' and then 5Ah / 5Dh, exactly as it does to 'R'. */
	/*
	 * 81h, 'W', then TWO DUMMY BYTES while the card returns its 5Ah 5Dh
	 * id, and only THEN the sector number:
	 *
	 *   81h -> N/A    57h -> FLAG   00h -> 5Ah   00h -> 5Dh
	 *   MSB -> 00h    LSB -> pre    data[0..127]  CHK
	 *
	 * Sending MSB/LSB straight after the command (as this did first) makes
	 * the card take data[0]/data[1] as the sector number — 34h 12h, the low
	 * half of the 0x1234 magic, i.e. sector 13330 — and answer FFh "bad
	 * sector". Reads hid the mistake because only sector 0 was ever read,
	 * and its address bytes are 00 00 wherever they sit.
	 */
	rx = sio0_xfer_fast(addr);   if (rx < 0) goto fail; mc_head[0] = rx;
	rx = sio0_xfer_fast(0x57);   if (rx < 0) goto fail; mc_head[1] = rx;
	rx = sio0_xfer_fast(0x00);   if (rx < 0) goto fail; mc_head[2] = rx;
	rx = sio0_xfer_fast(0x00);   if (rx < 0) goto fail; mc_head[3] = rx;
	rx = sio0_xfer_fast(msb);    if (rx < 0) goto fail; mc_head[4] = rx;
	rx = sio0_xfer_fast(lsb);    if (rx < 0) goto fail; mc_head[5] = rx;

	for (i = 0; i < MC_SECTOR_SIZE; i++) {
		chk ^= buf[i];
		if (sio0_xfer_fast(buf[i]) < 0) goto fail;
	}

	if (sio0_xfer_fast(chk) < 0) goto fail;

	/*
	 * Replies lag one transfer behind sends on this port (the probe showed
	 * "TX 52 -> RX ff" with the FLAG arriving on the next byte), so the
	 * acknowledge pair and the end byte trail the checksum. Clock out
	 * enough zeros to cover it and report the lot rather than guessing
	 * which slot holds 'G' — the caller prints them.
	 */
	end = -1;
	for (i = 0; i < MC_TAIL; i++) {
		rx = sio0_xfer_fast(0x00);
		if (rx < 0)
			goto fail;
		mc_tail[i] = (unsigned char)rx;
	}

	/*
	 * Anchor on the 5Ch 5Dh acknowledge pair and take the byte after it as
	 * the status, instead of counting transfers. The first attempt assumed
	 * a fixed offset and read an intermediate FFh as "bad sector" when the
	 * real tail was 00 ff 00 5c 5d with 'G' still to come.
	 */
	for (i = 0; i + 2 < MC_TAIL; i++) {
		if (mc_tail[i] == 0x5c && mc_tail[i + 1] == 0x5d) {
			end = mc_tail[i + 2];
			break;
		}
	}

	mc_deselect();
	return end < 0 ? 0 : end;
fail:
	mc_deselect();
	return -1;
}

/* Reads one sector into buf. Returns 1 on success. */
static int mc_read_sector(int slot, unsigned int sector, unsigned char *buf)
{
	unsigned char addr = 0x81 + (slot & 3);
	int i, ack, rx, prev = -1, found = 0;

	mc_select(slot);

	/*
	 * Deliberately uses sio0_xfer() — the same transfer routine as the
	 * `sio0` probe. The probe's stream is the one we verified by hand
	 * (5C 5D at 7/8, echoes at 9/10, data from 11), and the reply
	 * alignment shifts if the inter-byte pacing changes: reading with the
	 * faster routine put Sony's "MC" magic at offset 2 instead of 0.
	 */
	if (sio0_xfer(addr, &ack) < 0) goto fail;
	if (sio0_xfer(0x52, &ack) < 0) goto fail;		/* 'R' */
	if (sio0_xfer(0x00, &ack) < 0) goto fail;		/* -> 5Ah */
	if (sio0_xfer(0x00, &ack) < 0) goto fail;		/* -> 5Dh */
	if (sio0_xfer((sector >> 8) & 0xff, &ack) < 0) goto fail;
	if (sio0_xfer(sector & 0xff, &ack) < 0) goto fail;

	for (i = 0; i < 16; i++) {
		rx = sio0_xfer(0x00, &ack);
		if (rx < 0)
			goto fail;
		if (prev == 0x5c && rx == 0x5d) {
			found = 1;
			break;
		}
		prev = rx;
	}
	if (!found)
		goto fail;

	if (sio0_xfer(0x00, &ack) < 0) goto fail;	/* sector MSB echo */
	if (sio0_xfer(0x00, &ack) < 0) goto fail;	/* sector LSB echo */

	for (i = 0; i < MC_SECTOR_SIZE; i++) {
		rx = sio0_xfer(0x00, &ack);
		if (rx < 0)
			goto fail;
		buf[i] = (unsigned char)rx;
	}

	mc_deselect();
	return 1;
fail:
	mc_deselect();
	return 0;
}

/* ------------------------------------------------------------------ */
/* Lightspan keyboard (SIO0 device ID 0x96)                            */
/* ------------------------------------------------------------------ */

/*
 * Bring-up tool for a PS1 keyboard on the controller port, before any of it
 * exists as a driver.
 *
 * The Lightspan protocol - the closest thing to a standard PS1 keyboard, and
 * what BlueRetro emulates - is a controller-class device (address 0x01, not
 * 0x81 like a memory card) that answers:
 *
 *   TX:  01 42 00 00 00 00 00 00 00 00 00 00 00 00 06
 *   RX: HiZ 96 5A nn  d  d  d  d  d  d  d  d  d  d  d
 *
 *   0x96 = keyboard device id      0x5A = ready marker
 *   nn   = count of valid scancode bytes, 0x00..0x0B, or 0xFF = no keyboard
 *   d    = raw PS/2 Scan Code Set 2, up to 11 bytes per frame
 *
 * The adapter is a pass-through: a PS/2 keyboard already emits Set 2, so
 * nothing is translated on the way in. That is why up to eleven bytes can
 * arrive at once - a fast typist generates several make/break codes between
 * two polls.
 *
 * SIO0 replies lag one transfer behind the sends, so this anchors on finding
 * 0x96 in the buffer rather than trusting a fixed offset. That lesson cost a
 * day on the memory cards; see docs/captures.
 *
 * Doing this in the monitor rather than as a driver is deliberate. bu.c owns
 * SIO0 from the ACK interrupt and has its own locking, so a keyboard driver
 * has to arbitrate with it - which is real work, and a bad place to discover
 * that the wiring is wrong. Here there are no interrupts and no other users
 * of the bus.
 */

#define KBD_FRAME_LEN	15
#define KBD_ID		0x96
#define KBD_READY	0x5a
#define KBD_NO_DEV	0xff

/* PS/2 Scan Code Set 2 make codes -> ASCII, enough to read what was typed.
 * Paired array rather than a sparse initialiser: EGCS 2.91 is the compiler. */
static const unsigned char kbd_set2[][2] = {
	{0x1c,'a'},{0x32,'b'},{0x21,'c'},{0x23,'d'},{0x24,'e'},{0x2b,'f'},
	{0x34,'g'},{0x33,'h'},{0x43,'i'},{0x3b,'j'},{0x42,'k'},{0x4b,'l'},
	{0x3a,'m'},{0x31,'n'},{0x44,'o'},{0x4d,'p'},{0x15,'q'},{0x2d,'r'},
	{0x1b,'s'},{0x2c,'t'},{0x3c,'u'},{0x2a,'v'},{0x1d,'w'},{0x22,'x'},
	{0x35,'y'},{0x1a,'z'},
	{0x45,'0'},{0x16,'1'},{0x1e,'2'},{0x26,'3'},{0x25,'4'},{0x2e,'5'},
	{0x36,'6'},{0x3d,'7'},{0x3e,'8'},{0x46,'9'},
	{0x29,' '},{0x4e,'-'},{0x55,'='},{0x4c,';'},{0x52,'\''},{0x41,','},
	{0x49,'.'},{0x4a,'/'},{0x54,'['},{0x5b,']'},{0x5d,'\\'},{0x0e,'`'},
	{0x00,0}
};

static int kbd_ascii(int code)
{
	int i;

	for (i = 0; kbd_set2[i][1]; i++)
		if (kbd_set2[i][0] == code)
			return kbd_set2[i][1];
	return 0;
}

/*
 * Clock one Lightspan frame at `addr`. Returns the number of bytes received.
 *
 * `addr` is 0x01 for a device straight in the controller port, or 0x01+floor
 * for one behind a multitap - 0x02 is tap port B, and so on.
 */
static int kbd_poll(int slot, unsigned long addr, unsigned char *rx)
{
	unsigned char tx[KBD_FRAME_LEN] = {
		0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06
	};
	int i, ack, got;

	tx[0] = (unsigned char)addr;

	if (!mon_sio0_grab())
		return 0;

	JOY_CTRL_ = 0;
	sio0_delay(20000);
	JOY_MODE_ = 0x000d;
	JOY_BAUD_ = 0x0088;
	JOY_CTRL_ = 0x0003 | (slot ? 0x2000 : 0);
	sio0_delay(20000);

	for (i = 0; i < KBD_FRAME_LEN; i++) {
		got = sio0_xfer(tx[i], &ack);
		if (got < 0)
			break;
		rx[i] = (unsigned char)got;
	}

	JOY_CTRL_ = 0;
	psx_sio0_unlock();
	return i;
}

/*
 * Decode one frame. Prints any keys found, and returns 1 if a keyboard
 * answered at all - so the watch loop can tell "nothing there" from "there,
 * but idle".
 */
static int kbd_decode(const unsigned char *rx, int n, int verbose)
{
	int i, at = -1, num, ext = 0, brk = 0, c;

	for (i = 0; i < n; i++)
		if (rx[i] == KBD_ID) {
			at = i;
			break;
		}

	if (at < 0) {
		if (verbose)
			mon_puts("  no 0x96 in the reply - no keyboard on this "
				 "port (a pad answers 41, a card 5A 5D)\n");
		return 0;
	}

	if (at + 2 >= n) {
		if (verbose)
			mon_puts("  found 0x96 but the frame ended early\n");
		return 0;
	}

	if (rx[at + 1] != KBD_READY && verbose)
		mon_printf("  warning: expected 5A after the id, got %02x\n",
			   rx[at + 1]);

	num = rx[at + 2];
	if (num == KBD_NO_DEV) {
		if (verbose)
			mon_puts("  adapter present, but it reports no keyboard "
				 "attached (nn=FF)\n");
		return 1;
	}

	if (verbose)
		mon_printf("  keyboard present, %d scancode byte(s)\n", num);

	if (num > n - (at + 3))
		num = n - (at + 3);

	for (i = 0; i < num; i++) {
		int b = rx[at + 3 + i];

		if (b == 0x00)
			continue;
		if (b == 0xe0) {		/* extended key follows */
			ext = 1;
			continue;
		}
		if (b == 0xf0) {		/* break: next byte is a release */
			brk = 1;
			continue;
		}

		c = ext ? 0 : kbd_ascii(b);
		if (brk)
			mon_printf("  [up   %s%02x]\n", ext ? "e0 " : "", b);
		else if (c)
			mon_printf("  [down %02x] '%c'\n", b, c);
		else
			mon_printf("  [down %s%02x]\n", ext ? "e0 " : "", b);

		ext = 0;
		brk = 0;
	}

	return 1;
}

/*
 * Sweep both ports and all four tap floors looking for a keyboard.
 * Returns 1 and fills in the slot and address if one answers.
 */
static int kbd_find(int *fslot, unsigned long *faddr)
{
	unsigned char rx[KBD_FRAME_LEN];
	int slot, n, i;
	unsigned long addr;

	for (slot = 0; slot < 2; slot++) {
		for (addr = 0x01; addr <= 0x04; addr++) {
			n = kbd_poll(slot, addr, rx);
			for (i = 0; i < n; i++) {
				if (rx[i] != KBD_ID)
					continue;
				if (i + 1 < n && rx[i + 1] != KBD_READY)
					continue;
				mon_printf("  found: port %d, address %02lx%s\n",
					   slot + 1, addr,
					   addr > 1 ? "  (multitap floor)" : "");
				*fslot = slot;
				*faddr = addr;
				return 1;
			}
		}
	}

	return 0;
}

static void cmd_kbd(int argc, char **argv)
{
	unsigned char rx[KBD_FRAME_LEN];
	int slot = 0, n, i, watch = 0, quiet_rounds = 0;
	unsigned long addr = 0x01;

	if (argc > 1 && !strcmp(argv[1], "watch")) {
		watch = 1;
		argc--;
		argv++;
	}

	if (argc > 1 && !strcmp(argv[1], "scan")) {
		mon_puts("sweeping both ports and all four multitap floors ...\n");
		if (!kbd_find(&slot, &addr))
			mon_puts("  no keyboard anywhere (nothing answered 0x96)\n");
		return;
	}

	if (argc > 1) {
		int ok = 1;

		slot = (int)mon_hex(argv[1], &ok);
		if (!ok || (slot != 0 && slot != 1)) {
			mon_puts("usage: kbd [watch|scan] [slot] [addr]\n"
				 "   slot 0 = port 1 (default), 1 = port 2\n"
				 "   addr 01 = straight in the port (default),\n"
				 "        02..04 = multitap floors B..D\n"
				 "   kbd scan   sweep everywhere and report\n"
				 "   kbd watch  poll until you press a key here\n");
			return;
		}
	}
	if (argc > 2) {
		int ok = 1;
		unsigned long a = mon_hex(argv[2], &ok);

		if (ok && a >= 0x01 && a <= 0x04)
			addr = a;
	}

	if (!watch) {
		mon_printf("polling Lightspan keyboard, SIO0 slot %d, addr %02lx\n",
			   slot, addr);
		n = kbd_poll(slot, addr, rx);
		mon_printf("  %d bytes:", n);
		for (i = 0; i < n; i++)
			mon_printf(" %02x", rx[i]);
		mon_puts("\n");
		kbd_decode(rx, n, 1);
		return;
	}

	mon_printf("watching SIO0 slot %d addr %02lx - type on the PS1 keyboard.\n"
		   "press any key HERE to stop.\n", slot, addr);

	while (!mon_pending()) {
		n = kbd_poll(slot, addr, rx);
		if (n > 0 && kbd_decode(rx, n, 0))
			quiet_rounds = 0;
		else if (++quiet_rounds == 200) {
			mon_puts("  (nothing answering address 01 - check the "
				 "adapter, or try the other port)\n");
			quiet_rounds = 0;
		}
		sio0_delay(60000);	/* roughly a controller poll interval */
	}

	(void)mon_getc();		/* eat the key that stopped us */
	mon_puts("stopped.\n");
}

/* ------------------------------------------------------------------ */
/* CD-ROM bring-up probe                                               */
/* ------------------------------------------------------------------ */

/*
 * A polled, PIO reader for the CD-ROM, so the command and data path can be
 * proven before any of it becomes a block driver. docs/24 §4.5 asks for
 * exactly this: "a brmon command that reads one sector by hand proves the
 * command/IRQ path without also proving DMA."
 *
 * Everything here is deliberately the *simple* choice rather than the fast
 * one, because this exists to answer "does the hardware do what the
 * documentation says", not to move data:
 *
 *   - Polled, not interrupt driven. The CD interrupt is masked off in I_MASK
 *     for the duration, so the kernel's do_IRQ never sees it. HINTSTS is the
 *     controller's own register and latches regardless, so polling it works
 *     and nothing has to be registered with the IRQ layer.
 *   - PIO, not DMA. 2048 byte-loads is slow and it does not matter once.
 *   - KSEG1 pointers, matching head.S, the rest of brmon.c and PSn00bSDK.
 *     docs/24 §1.7 flags that the tree is inconsistent about this; using
 *     KSEG1 explicitly costs nothing and removes the question.
 *
 * Never do a 32-bit load from a CD port: auto-increment is off, so a word read
 * returns one byte smeared four times, and turning auto-increment on to "fix"
 * that breaks DMA. 8-bit only - psx-spx lists 16-bit as untested. [docs/24 §1.6]
 */

#define CD0		(*(volatile unsigned char *)0xbf801800)
#define CD1		(*(volatile unsigned char *)0xbf801801)
#define CD2		(*(volatile unsigned char *)0xbf801802)
#define CD3		(*(volatile unsigned char *)0xbf801803)

#define D3_MADR		(*(volatile unsigned long *)0xbf8010b0)
#define D3_BCR		(*(volatile unsigned long *)0xbf8010b4)
#define D3_CHCR		(*(volatile unsigned long *)0xbf8010b8)
#define DMA_DPCR	(*(volatile unsigned long *)0xbf8010f0)

#define CHCR_BUSY	0x01000000
#define CHCR_CD_READ	0x11000000	/* device->RAM, burst, start */

#define CD_BUS_CFG	(*(volatile unsigned long *)0xbf801018)
#define CD_COM_DELAY	(*(volatile unsigned long *)0xbf801020)
#define IRQ_STAT	(*(volatile unsigned long *)0xbf801070)
#define IRQ_MASK	(*(volatile unsigned long *)0xbf801074)

#define HSTS_PRMWRDY	0x10
#define HSTS_RSLRRDY	0x20
#define HSTS_DRQSTS	0x40
#define HSTS_BUSYSTS	0x80

#define IRQ_CD		0x0004		/* I_MASK / I_STAT bit 2 */

/* Commands (docs/24 §2.2) */
#define CDC_NOP		0x01
#define CDC_SETLOC	0x02
#define CDC_READN	0x06
#define CDC_PAUSE	0x09
#define CDC_INIT	0x0a
#define CDC_DEMUTE	0x0c
#define CDC_SETMODE	0x0e
#define CDC_TEST	0x19

#define CD_SECTOR	2048

static unsigned long cd_saved_mask;
static int cd_ready;			/* `cd init` has been run */

static void cd_delay(int n)
{
	volatile int i;

	for (i = 0; i < n; i++)
		;
}

/* The CD interrupt belongs to us while we poll, not to the kernel. */
static void cd_take_irq(void)
{
	cd_saved_mask = IRQ_MASK;
	IRQ_MASK = cd_saved_mask & ~IRQ_CD;
}

static void cd_release_irq(void)
{
	IRQ_STAT = ~IRQ_CD;
	IRQ_MASK = cd_saved_mask;
}

/* Wait for the HC05 to finish chewing on the last command byte. */
static int cd_wait_busy(void)
{
	long spins;

	for (spins = 0; spins < 200000; spins++)
		if (!(CD0 & HSTS_BUSYSTS))
			return 1;

	return 0;
}

/* Pending interrupt type, 0 = none. */
static int cd_int(void)
{
	CD0 = 1;
	return CD3 & 7;
}

/*
 * Acknowledge, in the order docs/24 §2.4 insists on: the MIPS interrupt first,
 * then the controller.
 */
static void cd_ack(void)
{
	IRQ_STAT = ~IRQ_CD;
	CD0 = 1;
	CD3 = 0x1f;
}

/* Wait for any interrupt. Returns the type, or -1 on timeout. */
static int cd_wait_int(long spins)
{
	long i;

	for (i = 0; i < spins; i++) {
		int t = cd_int();

		if (t)
			return t;
	}

	return -1;
}

/* Drain the result FIFO. Never read a fixed count: reading past the end pads
 * with zeroes and then silently *repeats* the response. [docs/24 §1.3] */
static int cd_result(unsigned char *buf, int max)
{
	int n = 0;

	while ((CD0 & HSTS_RSLRRDY) && n < max)
		buf[n++] = CD1;

	return n;
}

static int cd_command(int cmd, const unsigned char *par, int npar)
{
	int i;

	if (!cd_wait_busy()) {
		mon_puts("  CD busy, command not sent\n");
		return 0;
	}

	CD0 = 0;
	for (i = 0; i < npar; i++)
		CD2 = par[i];
	CD1 = (unsigned char)cmd;

	return 1;
}

/*
 * Send a command and wait for its acknowledgement.
 * Returns the interrupt type; the response is left in `res`.
 */
static int cd_do(int cmd, const unsigned char *par, int npar,
		 unsigned char *res, int *nres, long spins)
{
	int t;

	if (!cd_command(cmd, par, npar))
		return -1;

	t = cd_wait_int(spins);
	if (t < 0) {
		mon_printf("  cmd %02x: no response\n", cmd);
		return -1;
	}

	*nres = cd_result(res, 16);
	cd_ack();

	return t;
}

static void cd_show_stat(unsigned char st)
{
	mon_printf("  stat %02x:%s%s%s%s%s%s%s%s\n", st,
		   (st & 0x01) ? " Error"     : "",
		   (st & 0x02) ? " MotorOn"   : "",
		   (st & 0x04) ? " SeekError" : "",
		   (st & 0x08) ? " IdError"   : "",
		   (st & 0x10) ? " ShellOpen" : "",
		   (st & 0x20) ? " Read"      : "",
		   (st & 0x40) ? " Seek"      : "",
		   (st & 0x80) ? " Play"      : "");
}

/*
 * Init, following the recipe in docs/24 §2.4 Phase 0.
 *
 * COM_DELAY must be written explicitly. PSn00bSDK gets away with leaving it to
 * the BIOS default; we cannot, because prom_free_prom_memory() has already
 * destroyed BIOS state by the time anything here runs.
 */
static int cd_init(void)
{
	unsigned char res[16];
	int nres, t, i;

	cd_take_irq();

	CD_BUS_CFG = 0x00020943;	/* 8-bit, auto-increment OFF */
	CD_COM_DELAY = 0x0000132c;

	DMA_DPCR |= 0x00008000;		/* DMA3 master enable */
	D3_CHCR = 0;			/* and make sure it is stopped */

	CD0 = 0;
	CD3 = 0x00;			/* HCHPCTL: no data request pending */
	CD0 = 1;
	CD3 = 0x1f;			/* HCLRCTL: acknowledge every flag */
	CD2 = 0x1f;			/* HINTMSK: enable every source */
	cd_delay(10000);

	/* psx-spx's init recipe sends two Nops before anything else. */
	for (i = 0; i < 2; i++) {
		t = cd_do(CDC_NOP, 0, 0, res, &nres, 400000);
		if (t < 0) {
			mon_puts("  no answer to Nop - is the drive there?\n");
			cd_release_irq();
			return 0;
		}
	}
	mon_printf("  Nop -> INT%d, %d result byte(s)\n", t, nres);
	if (nres)
		cd_show_stat(res[0]);

	/* Init: INT3 (late), then INT2. Sets mode=20h, so Setmode comes after. */
	t = cd_do(CDC_INIT, 0, 0, res, &nres, 2000000);
	mon_printf("  Init -> INT%d\n", t);
	if (t == 3) {
		t = cd_wait_int(4000000);
		if (t > 0) {
			cd_result(res, 16);
			cd_ack();
			mon_printf("  Init complete -> INT%d\n", t);
		} else {
			mon_puts("  Init: second interrupt never arrived\n");
		}
	}

	t = cd_do(CDC_DEMUTE, 0, 0, res, &nres, 400000);
	mon_printf("  Demute -> INT%d\n", t);

	{
		unsigned char mode = 0x80;	/* 2x speed, 2048-byte sectors */

		t = cd_do(CDC_SETMODE, &mode, 1, res, &nres, 400000);
		mon_printf("  Setmode 80 -> INT%d\n", t);
	}

	cd_release_irq();
	cd_ready = 1;

	return 1;
}

/* LBA -> BCD minute/second/frame, with the 2-second lead-in. [docs/24 §2.2] */
static void cd_lba_to_msf(unsigned long lba, unsigned char *msf)
{
	unsigned long t = lba + 150;
	unsigned long m = t / (75 * 60);
	unsigned long s = (t / 75) % 60;
	unsigned long f = t % 75;

	msf[0] = (unsigned char)(((m / 10) << 4) | (m % 10));
	msf[1] = (unsigned char)(((s / 10) << 4) | (s % 10));
	msf[2] = (unsigned char)(((f / 10) << 4) | (f % 10));
}

/*
 * Read one sector by hand.
 *
 * Always Setloc before ReadN. docs/24 §2.3: a bare ReadN after a Pause
 * "resumes at the most recently received sector", i.e. hands back a sector you
 * already had - silent duplication, which in a block device is data corruption.
 */
static int cd_read_sector(unsigned long lba, unsigned char *buf, int use_dma)
{
	unsigned char msf[3], res[16];
	int nres, t, i;

	cd_lba_to_msf(lba, msf);
	mon_printf("  LBA %lu -> MSF %02x:%02x:%02x (BCD)\n",
		   lba, msf[0], msf[1], msf[2]);

	cd_take_irq();

	t = cd_do(CDC_SETLOC, msf, 3, res, &nres, 400000);
	if (t != 3) {
		mon_printf("  Setloc -> INT%d (expected 3)\n", t);
		if (t == 5 && nres > 1)
			mon_printf("  error byte %02x\n", res[1]);
		cd_release_irq();
		return 0;
	}

	t = cd_do(CDC_READN, 0, 0, res, &nres, 400000);
	if (t != 3) {
		mon_printf("  ReadN -> INT%d (expected 3)\n", t);
		cd_release_irq();
		return 0;
	}

	/*
	 * The seek happens inside ReadN, so the first INT1 can be far later
	 * than the steady-state 6.6 ms. Be generous here. [docs/24 §2.4]
	 */
	t = cd_wait_int(8000000);
	if (t != 1) {
		mon_printf("  waiting for data: INT%d\n", t);
		cd_release_irq();
		return 0;
	}

	cd_result(res, 16);

	/*
	 * Request the data BEFORE acknowledging, and keep the BIOS's odd dummy
	 * accesses - both PSn00bSDK and the BIOS do this on real hardware and
	 * nobody knows whether they are required. [docs/24 §4.6]
	 */
	CD0 = 0; (void)CD0;
	CD3 = 0; (void)CD3;
	CD0 = 0;
	CD3 = 0x80;			/* HCHPCTL: BFRD, please send data */

	for (i = 0; i < 200000; i++)
		if (CD0 & HSTS_DRQSTS)
			break;

	if (!(CD0 & HSTS_DRQSTS)) {
		mon_puts("  data never became ready (DRQSTS stayed low)\n");
		cd_ack();
		cd_release_irq();
		return 0;
	}

	if (use_dma) {
		/*
		 * DMA3, burst, device->RAM. 512 words in one block.
		 *
		 * MADR takes a 24-bit *physical* address, and with no MMU the
		 * buffer's own address is already physical once the KSEG0 bit
		 * is masked off - no bounce buffer, no virt_to_bus.
		 *
		 * Polling CHCR bit 24 rather than taking the DMA completion
		 * interrupt, as PSn00bSDK does: a driver that already has a
		 * per-sector CD interrupt gains nothing from a second source.
		 * The spin costs little either, since psx-spx notes any RAM or
		 * I/O read stalls the CPU until the transfer finishes anyway.
		 */
		D3_MADR = ((unsigned long)buf) & 0x00ffffff;
		D3_BCR  = 0x00010200;		/* 1 block x 512 words */
		D3_CHCR = CHCR_CD_READ;

		for (i = 0; i < 2000000; i++)
			if (!(D3_CHCR & CHCR_BUSY))
				break;

		if (D3_CHCR & CHCR_BUSY) {
			mon_puts("  DMA never completed\n");
			cd_ack();
			cd_release_irq();
			return 0;
		}
	} else {
		/* 8-bit loads only. See the note at the top of this section. */
		for (i = 0; i < CD_SECTOR; i++)
			buf[i] = CD2;
	}

	cd_ack();

	/* ReadN never stops on its own. */
	t = cd_do(CDC_PAUSE, 0, 0, res, &nres, 400000);
	if (t == 3)
		cd_wait_int(2000000), cd_ack();

	cd_release_irq();

	return 1;
}

/* unsigned long, not char: DMA needs the buffer 4-byte aligned and a char
 * array carries no such guarantee. */
static unsigned long cd_buf_w[CD_SECTOR / 4];
static unsigned long cd_buf2_w[CD_SECTOR / 4];
#define cd_buf   ((unsigned char *)cd_buf_w)
#define cd_buf2  ((unsigned char *)cd_buf2_w)

static void cmd_cd(int argc, char **argv)
{
	unsigned char res[16];
	int nres, t, ok = 1;
	unsigned long lba;

	if (argc < 2) {
		unsigned char hsts = CD0;

		mon_printf("CD-ROM: HSTS %02x  bank %d%s%s%s%s\n", hsts,
			   hsts & 3,
			   (hsts & HSTS_PRMWRDY) ? " PRMWRDY" : "",
			   (hsts & HSTS_RSLRRDY) ? " RSLRRDY" : "",
			   (hsts & HSTS_DRQSTS)  ? " DRQSTS"  : "",
			   (hsts & HSTS_BUSYSTS) ? " BUSY"    : "");
		mon_printf("  HINTSTS %d   BUS_CFG %08lx  COM_DELAY %08lx\n",
			   cd_int(), CD_BUS_CFG, CD_COM_DELAY);
		mon_printf("  initialised: %s\n", cd_ready ? "yes" : "no");
		mon_puts("\n  cd init          spin up and configure the drive\n"
			 "  cd stat          Getstat (Nop)\n"
			 "  cd rd <lba>      read one sector by PIO and dump it\n"
			 "  cd dma <lba>     the same, by DMA\n"
			 "  cd cmp <lba>     read both ways and compare\n"
			 "  cd kseg          cached vs uncached view of the buffer\n"
			 "  cd id            drive firmware version (Test 20h)\n");
		return;
	}

	if (!strcmp(argv[1], "init")) {
		mon_puts("initialising CD-ROM (polled, no interrupts) ...\n");
		if (cd_init())
			mon_puts("  ready\n");
		return;
	}

	if (!strcmp(argv[1], "stat")) {
		cd_take_irq();
		t = cd_do(CDC_NOP, 0, 0, res, &nres, 400000);
		cd_release_irq();
		mon_printf("  Nop -> INT%d, %d byte(s)\n", t, nres);
		if (nres)
			cd_show_stat(res[0]);
		return;
	}

	if (!strcmp(argv[1], "id")) {
		unsigned char sub = 0x20;
		int i;

		cd_take_irq();
		t = cd_do(CDC_TEST, &sub, 1, res, &nres, 400000);
		cd_release_irq();
		mon_printf("  Test 20 -> INT%d:", t);
		for (i = 0; i < nres; i++)
			mon_printf(" %02x", res[i]);
		mon_puts("\n");
		return;
	}

	/*
	 * Read one sector twice - once by PIO, once by DMA - and compare all
	 * 2048 bytes. The cached-vs-uncached check lives in `cd kseg`.
	 *
	 * The second check is the one docs/24 §1.7 marks VERIFY, and it decides
	 * something structural: whether every DMA buffer in the block driver
	 * has to be touched through KSEG1. The claim is that the R3000A has no
	 * writeback data cache at all - its 1 KB "D-cache" is wired as the
	 * scratchpad - so a DMA into a cached KSEG0 buffer needs no invalidate.
	 * If the two views disagree, that claim is wrong and the driver design
	 * changes.
	 */
	if (!strcmp(argv[1], "cmp")) {
		int i, diff = 0, first = -1;

		if (argc < 3) {
			mon_puts("usage: cd cmp <lba>\n");
			return;
		}
		lba = mon_hex(argv[2], &ok);
		if (!ok) {
			mon_puts("bad LBA\n");
			return;
		}

		for (i = 0; i < CD_SECTOR; i++)
			cd_buf[i] = cd_buf2[i] = 0;

		mon_puts("  PIO read ...\n");
		if (!cd_read_sector(lba, cd_buf2, 0)) {
			mon_puts("  PIO read FAILED\n");
			return;
		}
		mon_puts("  DMA read ...\n");
		if (!cd_read_sector(lba, cd_buf, 1)) {
			mon_puts("  DMA read FAILED\n");
			return;
		}

		for (i = 0; i < CD_SECTOR; i++)
			if (cd_buf[i] != cd_buf2[i]) {
				if (first < 0)
					first = i;
				diff++;
			}

		mon_printf("  PIO vs DMA: %d of %d bytes differ", diff,
			   CD_SECTOR);
		if (first >= 0)
			mon_printf(", first at %04x (pio %02x, dma %02x)",
				   first, cd_buf2[first], cd_buf[first]);
		mon_puts("\n");

		return;
	}


	/*
	 * Read the DMA buffer back through KSEG1 and compare with the KSEG0
	 * view - docs/24 1.7's VERIFY, which decides whether every DMA buffer
	 * in the block driver has to be touched uncached.
	 *
	 * Kept as its own command, and announced a step at a time, because the
	 * first attempt at this hung the machine outright with no output: worth
	 * knowing exactly which access does that rather than losing the useful
	 * PIO-vs-DMA result along with it.
	 */
	if (!strcmp(argv[1], "kseg")) {
		volatile unsigned char *k1;
		unsigned long a;
		int i, kdiff = 0;

		/*
		 * This kernel is linked in KUSEG - its symbols are plain
		 * physical addresses like 0015758c, not KSEG0 8015758c. So the
		 * uncached alias is physical | 0xa0000000, and the obvious
		 * `addr | 0x20000000` is wrong: it produced 2015758c, a KUSEG
		 * address 537 MB into a machine with two, and reading it hung
		 * the console outright.
		 *
		 * The same fact is why DMA needs no address translation here:
		 * MADR wants a physical address and the buffer's own address
		 * already is one.
		 */
		a = (unsigned long)cd_buf;
		mon_printf("  buffer %08lx (KUSEG) -> %08lx (KSEG1)\n",
			   a, (a & 0x1fffffff) | 0xa0000000);
		mon_puts("  reading ONE byte uncached ...\n");

		k1 = (volatile unsigned char *)((a & 0x1fffffff) | 0xa0000000);
		i = k1[0];
		mon_printf("  ok, got %02x (KSEG0 says %02x)\n", i, cd_buf[0]);

		mon_puts("  comparing all 2048 ...\n");
		for (i = 0; i < CD_SECTOR; i++)
			if (k1[i] != cd_buf[i])
				kdiff++;

		mon_printf("  %d byte(s) differ\n", kdiff);
		if (!kdiff)
			mon_puts("  -> no cache coherency problem; DMA buffers "
				 "need no invalidate (docs/24 1.7)\n");
		else
			mon_puts("  -> DMA buffers MUST be read through KSEG1\n");
		return;
	}

	if (!strcmp(argv[1], "rd") || !strcmp(argv[1], "dma")) {
		int i, j;
		int use_dma = (argv[1][0] == 'd');

		if (argc < 3) {
			mon_puts("usage: cd rd <lba>   (sector 16 is the ISO9660 "
				 "descriptor and should read \"CD001\")\n");
			return;
		}
		lba = mon_hex(argv[2], &ok);
		if (!ok) {
			mon_puts("bad LBA\n");
			return;
		}
		if (!cd_ready)
			mon_puts("  (drive not initialised - try `cd init` "
				 "first)\n");

		for (i = 0; i < CD_SECTOR; i++)
			cd_buf[i] = 0;

		if (!cd_read_sector(lba, cd_buf, use_dma)) {
			mon_puts("  read FAILED\n");
			return;
		}

		mon_printf("  read OK (%s)\n", use_dma ? "DMA" : "PIO");
		for (i = 0; i < 128; i += 16) {
			mon_printf("  %04x: ", i);
			for (j = 0; j < 16; j++)
				mon_printf("%02x ", cd_buf[i + j]);
			mon_puts(" |");
			for (j = 0; j < 16; j++) {
				int c = cd_buf[i + j];

				mon_putc(c >= 0x20 && c < 0x7f ? c : '.');
			}
			mon_puts("|\n");
		}

		/*
		 * Sector 16 of any ISO9660 disc is the primary volume
		 * descriptor and begins 01 "CD001". Finding it is proof the
		 * whole path works, not just that bytes moved.
		 */
		if (cd_buf[1] == 'C' && cd_buf[2] == 'D' && cd_buf[3] == '0' &&
		    cd_buf[4] == '0' && cd_buf[5] == '1')
			mon_puts("\n  \"CD001\" - this is a real ISO9660 volume "
				 "descriptor. The path works.\n");
		return;
	}

	mon_printf("unknown: cd %s\n", argv[1]);
}

static void cmd_card_format(int argc, char **argv)
{
	/* must match bu_first_block_t in drivers/block/bu.h */
	static unsigned char blk[MC_SECTOR_SIZE];
	unsigned long slot, seq = 0;
	unsigned short saved_imask;
	int ok = 1, end, i;

	if (argc < 4 || strcmp(argv[3], "yes") != 0) {
		mon_puts("usage: card format <slot> yes [seq]\n"
			 "  slot: 0-3 = port 1 tap A-D, 4-7 = port 2 tap A-D\n"
			 "  seq : position in the joined set (default 0)\n"
			 "\n"
			 "  THIS ERASES THE CARD. Block 0 is its directory;\n"
			 "  any saves on it are gone. The literal word 'yes'\n"
			 "  is required so this cannot happen by accident.\n");
		return;
	}

	slot = mon_hex(argv[2], &ok);
	if (!ok || slot > 7) {
		mon_puts("bad slot (0-7)\n");
		return;
	}
	if (argc > 4)
		seq = mon_hex(argv[4], &ok);

	/*
	 * Build the header. Fields are the four __u32 of bu_first_block_t:
	 *   id     0x1234        what bu_read_first_block() looks for
	 *   size   1024          card size in 128-byte blocks (128 KB)
	 *                        bu.c reports (size >> 3) - 1 = 127 Kbytes
	 *   serial                unique-ish, so cards can be told apart
	 *   number seq           position for CONFIG_PSX_LARGE_CARD, which
	 *                        requires them found in order 0,1,2,...
	 */
	for (i = 0; i < MC_SECTOR_SIZE; i++)
		blk[i] = 0;

	*(unsigned long *)&blk[0]  = 0x1234;
	*(unsigned long *)&blk[4]  = 1024;
	*(unsigned long *)&blk[8]  = 0xb1acc000UL + (slot << 8) + seq;
	*(unsigned long *)&blk[12] = seq;

	mon_printf("formatting slot %lu as card #%lu - erasing block 0\n",
		   slot, seq);

	/* bu.c has an ACK interrupt handler registered; keep it out of our
	 * polled transfer, the same way kloader masks IRQs during uploads. */
	saved_imask = *(volatile unsigned short *)0xbf801074;
	*(volatile unsigned short *)0xbf801074 = 0;

	end = mc_write_sector((int)slot, 0, blk);

	*(volatile unsigned short *)0xbf801074 = saved_imask;

	if (end < 0) {
		mon_puts("  FAILED: no response (card not present?)\n");
		return;
	}
	mon_printf("  head: %02x %02x %02x %02x %02x %02x  (xx FLAG 5a 5d ..)\n",
		   mc_head[0], mc_head[1], mc_head[2],
		   mc_head[3], mc_head[4], mc_head[5]);
	mon_printf("  tail: %02x %02x %02x %02x %02x %02x %02x %02x\n",
		   mc_tail[0], mc_tail[1], mc_tail[2], mc_tail[3],
		   mc_tail[4], mc_tail[5], mc_tail[6], mc_tail[7]);
	mon_printf("  end byte %02x = %s\n", end,
		   end == 0x47 ? "'G' good" :
		   end == 0x4e ? "'N' bad checksum" :
		   end == 0xff ? "bad sector" : "unexpected");

	/* Read back regardless of the end byte: the block is what matters,
	 * and the status decode has already been wrong once. */
	{
		static unsigned char rb[MC_SECTOR_SIZE];

		saved_imask = *(volatile unsigned short *)0xbf801074;
		*(volatile unsigned short *)0xbf801074 = 0;
		i = mc_read_sector((int)slot, 0, rb);
		*(volatile unsigned short *)0xbf801074 = saved_imask;

		if (!i) {
			mon_puts("  verify: read back FAILED\n");
			return;
		}
		mon_printf("  verify: id=%08lx size=%lu serial=%08lx number=%lu\n",
			   *(unsigned long *)&rb[0], *(unsigned long *)&rb[4],
			   *(unsigned long *)&rb[8], *(unsigned long *)&rb[12]);
		mon_puts(*(unsigned long *)&rb[0] == 0x1234 ?
			 "  OK — reboot and the driver should claim this card\n" :
			 "  MISMATCH — header did not stick\n");
	}
}

/*
 * Read a block from any block device through the block layer.
 *
 * Written to answer one question: after "initrd copied to ram0", is the data
 * actually readable from /dev/ram0? rd.c notes that "the read path doesn't
 * work correctly on uClinux", so a mount can fail even when the copy reported
 * success. `blk 1 0 1` dumps the ext2 superblock block — magic 53 EF at
 * offset 0x38 means the filesystem is there and readable.
 *
 *   blk <major> <minor> <block>     e.g. blk 1 0 1   -> /dev/ram0 block 1
 *                                        blk d0 0 0  -> /dev/bul block 0
 */
/*
 * blkw - write a block through the Linux block layer, then read it back.
 *
 * bu.c has had a write path since it was written and it has never once run.
 * kloader writes memory cards - that is how its settings persist - so the SIO0
 * protocol is proven; what is unproven is do_bu_request() handling CURRENT->cmd
 * == WRITE. Everything that wants to save a file on this machine is waiting on
 * that one question, and the monitor is the right place to ask it: no
 * filesystem, no page cache games, no other user of the bus.
 *
 * Destructive. Requires the word "yes", and refuses block 0, which carries the
 * magic bu.c uses to recognise a formatted card.
 */
static void cmd_blkw(int argc, char **argv)
{
	struct buffer_head *bh;
	unsigned long major, minor, block, size = 1024;
	kdev_t dev;
	int ok = 1, i, bad;
	unsigned char pat;

	if (argc < 5 || strcmp(argv[argc - 1], "yes")) {
		mon_puts("usage: blkw <major> <minor> <block> [size] yes   (hex)\n"
			 "  WRITES to the device and DESTROYS that block.\n"
			 "  d0 0 100 ... yes   /dev/bul block 256\n"
			 "  Block 0 is refused: it holds the card magic.\n");
		return;
	}

	major = mon_hex(argv[1], &ok);
	minor = mon_hex(argv[2], &ok);
	block = mon_hex(argv[3], &ok);
	if (argc > 5)
		size = mon_hex(argv[4], &ok);
	if (!ok) {
		mon_puts("bad argument\n");
		return;
	}
	if (block == 0) {
		mon_puts("refusing block 0 - it carries the card magic\n");
		return;
	}

	dev = MKDEV(major, minor);
	pat = (unsigned char)(0xA5 ^ (block & 0xff));

	mon_printf("write test: dev %lu:%lu block %lu, %lu bytes, pattern %02x\n",
		   major, minor, block, size, pat);

	/* 1. read it first - a write path that cannot read is not testable */
	bh = bread(dev, block, (int)size);
	if (!bh) {
		mon_puts("  READ FAILED before writing - stopping\n");
		return;
	}
	mon_printf("  read ok, first bytes %02x %02x %02x %02x\n",
		   (unsigned char)bh->b_data[0], (unsigned char)bh->b_data[1],
		   (unsigned char)bh->b_data[2], (unsigned char)bh->b_data[3]);

	/* 2. fill and write */
	for (i = 0; i < (int)size; i++)
		bh->b_data[i] = (char)(pat + (i & 0x0f));

	mark_buffer_uptodate(bh, 1);
	mark_buffer_dirty(bh);
	ll_rw_block(WRITE, 1, &bh);
	wait_on_buffer(bh);

	if (!buffer_uptodate(bh)) {
		mon_puts("  WRITE FAILED (buffer not uptodate after ll_rw_block)\n");
		brelse(bh);
		return;
	}
	brelse(bh);
	mon_puts("  write submitted and completed\n");

	/* 3. drop the cache and read it back off the media */
	invalidate_buffers(dev);

	bh = bread(dev, block, (int)size);
	if (!bh) {
		mon_puts("  READ BACK FAILED\n");
		return;
	}

	bad = 0;
	for (i = 0; i < (int)size; i++)
		if ((unsigned char)bh->b_data[i] != (unsigned char)(pat + (i & 0x0f)))
			bad++;

	if (bad == 0)
		mon_printf("  VERIFIED: %lu bytes read back identical\n", size);
	else
		mon_printf("  MISMATCH: %d of %lu bytes differ\n", bad, size);

	mon_printf("  first bytes now %02x %02x %02x %02x\n",
		   (unsigned char)bh->b_data[0], (unsigned char)bh->b_data[1],
		   (unsigned char)bh->b_data[2], (unsigned char)bh->b_data[3]);
	brelse(bh);
}

static void cmd_blk(int argc, char **argv)
{
	struct buffer_head *bh;
	unsigned long major, minor, block;
	int ok = 1, i, j;

	unsigned long size = 1024;

	if (argc < 4) {
		mon_puts("usage: blk <major> <minor> <block> [size]   (hex)\n"
			 "  1 0 1        /dev/ram0 block 1 (ext2 superblock)\n"
			 "  d0 0 0       /dev/bul block 0\n"
			 "  d1 0 10 800  /dev/psxcd sector 16 - the CD's ISO9660\n"
			 "               descriptor, through the block layer\n"
			 "  (psxcd needs size 800: its hardsect_size is 2048 and\n"
			 "   bread below that fails)\n");
		return;
	}

	major = mon_hex(argv[1], &ok);
	minor = mon_hex(argv[2], &ok);
	block = mon_hex(argv[3], &ok);
	if (argc > 4)
		size = mon_hex(argv[4], &ok);
	if (!ok) {
		mon_puts("bad argument\n");
		return;
	}

	/*
	 * Print what the block layer thinks this device is BEFORE touching it.
	 * A read that hangs without the driver's request function ever being
	 * called means the block layer declined to dispatch, and these three
	 * tables are what it decides from. A zero in blk_size is the classic
	 * cause: the device is registered but has no size, so every request is
	 * "beyond the end of the device".
	 */
	{
		extern int *blk_size[];
		extern int *blksize_size[];
		extern int *hardsect_size[];

		mon_printf("dev %lu:%lu  blk_size=%d KB  blksize=%d  hardsect=%d\n",
			   major, minor,
			   blk_size[major]      ? blk_size[major][minor]      : -1,
			   blksize_size[major]  ? blksize_size[major][minor]  : -1,
			   hardsect_size[major] ? hardsect_size[major][minor] : -1);
	}

	mon_printf("bread(major %lu, minor %lu, block %lu, %lu)...\n",
		   major, minor, block, size);

	bh = bread(MKDEV(major, minor), block, (int)size);
	if (!bh) {
		mon_puts("  READ FAILED (bread returned NULL)\n");
		return;
	}

	for (i = 0; i < 64; i += 16) {
		mon_printf("%04x: ", i);
		for (j = 0; j < 16; j++)
			mon_printf("%02x ", (unsigned char)bh->b_data[i + j]);
		mon_puts(" |");
		for (j = 0; j < 16; j++) {
			int c = (unsigned char)bh->b_data[i + j];

			mon_putc(c >= 0x20 && c < 0x7f ? c : '.');
		}
		mon_puts("|\n");
	}

	/* ext2 superblock magic sits at offset 0x38 of the block */
	mon_printf("ext2 magic at 0x38: %02x %02x  %s\n",
		   (unsigned char)bh->b_data[0x38],
		   (unsigned char)bh->b_data[0x39],
		   ((unsigned char)bh->b_data[0x38] == 0x53 &&
		    (unsigned char)bh->b_data[0x39] == 0xef) ?
		   "= EF53, a filesystem is here" : "not ext2");

	brelse(bh);
}

/*
 * Round-trip test for a block device: write a signature, read it back.
 *
 * rd.c keeps ramdisk contents in the buffer cache — getblk() per block,
 * pinned with mark_buffer_protected(). A read that finds no matching cached
 * buffer silently returns a fresh one full of whatever was in that memory,
 * which is how "initrd copied to ram0" can be followed by a mount failure and
 * a dump full of slab data.
 *
 * This writes a known pattern through the same path the driver uses and reads
 * it back, so we can tell "the write never stored anything" from "the read
 * cannot find it".
 *
 *   blktest <major> <minor> <block>
 */
static void cmd_blktest(int argc, char **argv)
{
	struct buffer_head *bh;
	unsigned long major, minor, block;
	int ok = 1, i, bad = 0;

	if (argc < 4) {
		mon_puts("usage: blktest <major> <minor> <block>  (hex)\n"
			 "  writes a signature to that block, reads it back\n");
		return;
	}

	major = mon_hex(argv[1], &ok);
	minor = mon_hex(argv[2], &ok);
	block = mon_hex(argv[3], &ok);
	if (!ok) {
		mon_puts("bad argument\n");
		return;
	}

	mon_printf("writing signature to %lu:%lu block %lu\n",
		   major, minor, block);

	bh = getblk(MKDEV(major, minor), block, 1024);
	if (!bh) {
		mon_puts("  getblk FAILED\n");
		return;
	}
	for (i = 0; i < 1024; i++)
		bh->b_data[i] = (char)(0xb0 + (i & 0x0f));
	bh->b_data[0] = 'B'; bh->b_data[1] = 'R';
	bh->b_data[2] = 'M'; bh->b_data[3] = 'N';

	mark_buffer_uptodate(bh, 1);
	mark_buffer_dirty(bh);
	mark_buffer_protected(bh);
	brelse(bh);

	mon_puts("  written, now reading it back\n");

	bh = bread(MKDEV(major, minor), block, 1024);
	if (!bh) {
		mon_puts("  bread FAILED\n");
		return;
	}

	mon_printf("  first 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
		   (unsigned char)bh->b_data[0], (unsigned char)bh->b_data[1],
		   (unsigned char)bh->b_data[2], (unsigned char)bh->b_data[3],
		   (unsigned char)bh->b_data[4], (unsigned char)bh->b_data[5],
		   (unsigned char)bh->b_data[6], (unsigned char)bh->b_data[7]);

	if (bh->b_data[0] != 'B' || bh->b_data[1] != 'R' ||
	    bh->b_data[2] != 'M' || bh->b_data[3] != 'N')
		bad = 1;
	for (i = 16; i < 1024; i++)
		if ((unsigned char)bh->b_data[i] != (0xb0 + (i & 0x0f)))
			bad = 1;

	mon_puts(bad ? "  MISMATCH - the block device does not round-trip\n"
		     : "  OK - write and read agree, the device works\n");
	brelse(bh);
}

static void cmd_reboot(void)
{
	typedef void (*entry_t)(void);

	mon_puts("jumping to BIOS reset vector 0xbfc00000 ...\n");
	((entry_t)0xbfc00000)();
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

void brmon_main(void)
{
	char *argv[MON_ARGS_MAX];
	int argc;

	mon_serial_init();

	mon_puts("\n\n"
		 "  ####  BLACKROO MONITOR  ####\n"
		 "  Linux " UTS_RELEASE " on PlayStation (MIPS R3000A)\n"
		 "  Type 'help' for commands, 'cont' to resume booting.\n\n");

	for (;;) {
		mon_puts("blackroo> ");
		mon_getline();

		argc = mon_split(mon_line, argv);
		if (argc == 0)
			continue;

		if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?"))
			cmd_help();
		else if (!strcmp(argv[0], "md") || !strcmp(argv[0], "dump"))
			cmd_md(argc, argv);
		else if (!strcmp(argv[0], "peek"))
			cmd_peek(argc, argv);
		else if (!strcmp(argv[0], "poke"))
			cmd_poke(argc, argv);
		else if (!strcmp(argv[0], "fill"))
			cmd_fill(argc, argv);
		else if (!strcmp(argv[0], "ram"))
			cmd_ram(argc, argv);
		else if (!strcmp(argv[0], "hw"))
			cmd_hw();
		else if (!strcmp(argv[0], "cpu"))
			cmd_cpu();
		else if (!strcmp(argv[0], "mem"))
			cmd_mem();
		else if (!strcmp(argv[0], "card"))
			cmd_card(argc, argv);
		else if (!strcmp(argv[0], "sio0"))
			cmd_sio0(argc, argv);
		else if (!strcmp(argv[0], "kbd"))
			cmd_kbd(argc, argv);
		else if (!strcmp(argv[0], "cd"))
			cmd_cd(argc, argv);
		else if (!strcmp(argv[0], "blk"))
			cmd_blk(argc, argv);
		else if (!strcmp(argv[0], "blktest"))
			cmd_blktest(argc, argv);
		else if (!strcmp(argv[0], "budefer")) {
			extern unsigned long bu_defers, bu_kbd_grabs;
			mon_printf("bu: %lu requests deferred, %lu keyboard bus grabs\n",
				   bu_defers, bu_kbd_grabs);
		}
		else if (!strcmp(argv[0], "blkw"))
			cmd_blkw(argc, argv);
		else if (!strcmp(argv[0], "tty")) {
			if (argc > 1 && !strcmp(argv[1], "off"))
				mon_tty_echo = 0;
			else if (argc > 1 && !strcmp(argv[1], "on"))
				mon_tty_echo = 1;
			mon_printf("tty echo %s\n", mon_tty_echo ? "on" : "off");
		}
		else if (!strcmp(argv[0], "reboot"))
			cmd_reboot();
		else if (!strcmp(argv[0], "cont") || !strcmp(argv[0], "exit"))
			break;
		else
			mon_printf("unknown command: %s (try 'help')\n", argv[0]);
	}

	mon_puts("leaving monitor\n");
}
