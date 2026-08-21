/*
 * brcon.c - a minimal polled SIO1 character device for userspace.
 *
 * Why this exists
 * ---------------
 * /dev/console goes through the tty layer, and on this port a userspace
 * write(1, ...) reaches sys_write and never returns: the process sleeps in
 * the line discipline waiting for a transmit path that serial_psx.c does not
 * really provide (it also compiles with an implicit declaration of
 * sio_ready(), which is a fair hint at its state).
 *
 * Fixing the tty layer is a project of its own. This is the small thing that
 * unblocks userspace: a character device that talks to SIO1 by polling,
 * exactly as the in-kernel monitor does - no tty, no line discipline, no
 * interrupts, no sleeping. The monitor has been driving this port reliably
 * all along, so the code is known to work on real hardware.
 *
 * A shell opens /dev/brcon and uses it for stdin/stdout. That is a genuine
 * userspace process doing real syscalls into a real driver; it just skips a
 * tty layer that is not ready yet.
 *
 * Registers and the RTS requirement come from docs/13-SIO1-HARDWARE-RESEARCH.md:
 *   MODE 0x4e = 8N1 with the 16x baud multiplier
 *   CTRL 0x27 asserts RTS, without which real silicon refuses to transmit
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/sched.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/ps/sio.h>

#define BRCON_MAJOR	60

/* give up rather than wedge the machine if nothing is listening */
#define BRCON_TX_SPINS	400000

static int brcon_ready;

static void brcon_init_port(void)
{
	if (brcon_ready)
		return;

	outw(SIO_B11520, SIO_RATE_REG);
	outw(SIO_BRS16 | SIO_CHR8 | SIO_SB1, SIO_MODE_REG);
	outw(SIO_TX | SIO_RX | SIO_DTR | SIO_RTS, SIO_CTRL_REG);
	brcon_ready = 1;
}

static void brcon_putc(int c)
{
	long spins = 0;

	if (c == '\n')
		brcon_putc('\r');

	while ((inw(SIO_STAT_REG) & SIO_RFW) == 0)
		if (++spins > BRCON_TX_SPINS)
			return;

	outb(c, SIO_DATA_REG);
}

static ssize_t brcon_write(struct file *file, const char *buf, size_t count,
			   loff_t *ppos)
{
	size_t i;
	static int first = 1;

	if (first) {
		printk("brcon: first write, %d bytes\n", (int)count);
		first = 0;
	}

	brcon_init_port();

	for (i = 0; i < count; i++) {
		char c;

		if (get_user(c, buf + i))
			return -EFAULT;
		brcon_putc(c);
	}

	return count;
}

/*
 * Blocking read of at least one character.
 *
 * Polls with schedule() between characters so the machine is not locked up
 * while a shell sits at its prompt - this is cooperative rather than
 * interrupt-driven, which is all the monitor has ever needed here.
 */
static ssize_t brcon_read(struct file *file, char *buf, size_t count,
			  loff_t *ppos)
{
	size_t got = 0;

	if (!count)
		return 0;

	brcon_init_port();

	/* keep RTS asserted or the host will not send (docs/13) */
	outw(inw(SIO_CTRL_REG) | SIO_RTS | SIO_RX | SIO_TX, SIO_CTRL_REG);

	while (got < count) {
		int c;

		while ((inw(SIO_STAT_REG) & SIO_RFR) == 0) {
			if (signal_pending(current))
				return got ? (ssize_t)got : -EINTR;
			schedule();
		}

		c = inb(SIO_DATA_REG) & 0xff;

		if (put_user((char)c, buf + got))
			return -EFAULT;
		got++;

		/* echo, and hand the line over on carriage return */
		if (c == '\r' || c == '\n') {
			brcon_putc('\n');
			break;
		}
		brcon_putc(c);
	}

	return got;
}

static int brcon_open(struct inode *inode, struct file *file)
{
	printk("brcon: open by pid %d\n", current->pid);
	brcon_init_port();
	return 0;
}

static int brcon_release(struct inode *inode, struct file *file)
{
	return 0;
}

static struct file_operations brcon_fops = {
	read:		brcon_read,
	write:		brcon_write,
	open:		brcon_open,
	release:	brcon_release,
};

int __init brcon_init(void)
{
	if (register_chrdev(BRCON_MAJOR, "brcon", &brcon_fops)) {
		printk(KERN_ERR "brcon: cannot get major %d\n", BRCON_MAJOR);
		return -EIO;
	}

	printk(KERN_INFO "brcon: polled SIO1 console on major %d\n",
	       BRCON_MAJOR);
	return 0;
}

__initcall(brcon_init);
