/*
 * psxkbd.c - a keyboard for the PlayStation, on the SIO0 controller bus.
 *
 * Speaks the Lightspan protocol (SIO0 device id 0x96), which is the closest
 * thing the PS1 ever had to a standard keyboard and what a BlueRetro adapter
 * emulates from a Bluetooth keyboard. Proven on hardware with BRMON's `kbd`
 * command before this driver existed - see docs/27-KEYBOARD-BRINGUP.md and
 * docs/captures/2026-08-21-keyboard-scancodes.txt.
 *
 *   TX:  01 42 00 00 00 00 00 00 00 00 00 00 00 00 06
 *   RX: HiZ 96 5A nn  d  d  d  d  d  d  d  d  d  d  d
 *
 *   0x96 = keyboard device id      0x5A = ready marker
 *   nn   = count of valid scancode bytes, 0x00..0x0B, or 0xFF = no keyboard
 *   d    = raw PS/2 Scan Code Set 2, up to 11 bytes per frame
 *
 * Eleven bytes exist because the adapter is a pass-through, not a translator:
 * a PS/2 keyboard already emits Set 2, and a fast typist produces several
 * make and break codes between two polls.
 *
 * Design notes
 * ------------
 * *Polled from a timer.* There is no keyboard interrupt to hook - SIO0's ACK
 * interrupt belongs to the memory card driver. A timer is also what lets this
 * share the bus safely, because it can simply skip a turn.
 *
 * *Emits keycodes, not scancodes.* CONFIG_PC_KEYB is off for this port: the
 * 8042 that pckbd_translate() assumes does not exist here. Rather than
 * translate Set 2 -> Set 1 -> keycode through it, the table below maps Set 2
 * straight to Linux keycodes and kbd_translate() is a pass-through. One table
 * instead of two, and the one that is left can be checked against a real
 * keyboard with BRMON.
 *
 * *Shares SIO0 with bu.c.* The cards and the keyboard differ only by the
 * address byte, so both drivers use the same bus. psx_sio0_trylock() in bu.c
 * is the non-blocking arbiter; losing the race costs one 20 ms sample.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/kbd_kern.h>
#include <linux/kbd_ll.h>	/* handle_scancode() */

#include <asm/io.h>
#include <asm/keyboard.h>

/*
 * The SIO0 arbiter, in drivers/block/bu.c - declared here rather than in a
 * header because these two drivers are the only users of the bus and the
 * pairing is the point. See the comment above psx_sio0_trylock() in bu.c.
 */
extern int psx_sio0_trylock(void);
extern void psx_sio0_unlock(void);

/*
 * SIO0, through KSEG1 so the CPU write queue cannot reorder register access.
 * psx-spx, Controllers and Memory Cards:
 *   0x1f801040 JOY_TX_DATA / JOY_RX_DATA
 *   0x1f801044 JOY_STAT   bit0 TX ready, bit1 RX not empty, bit7 /ACK level
 *   0x1f801048 JOY_MODE   0x000D = 8 bit, MUL1
 *   0x1f80104a JOY_CTRL   bit0 TXEN, bit1 /JOYn, bit2 RXEN, bit13 slot
 *   0x1f80104e JOY_BAUD   0x0088 -> ~250 kHz
 */
#define JOY_DATA  (*(volatile unsigned char  *)0xbf801040)
#define JOY_STAT  (*(volatile unsigned long  *)0xbf801044)
#define JOY_MODE  (*(volatile unsigned short *)0xbf801048)
#define JOY_CTRL  (*(volatile unsigned short *)0xbf80104a)
#define JOY_BAUD  (*(volatile unsigned short *)0xbf80104e)

#define KBD_FRAME_LEN	15
#define KBD_ID		0x96
#define KBD_READY	0x5a
#define KBD_NO_DEV	0xff

/*
 * Bounded spins. This runs in a timer, so nothing here may wait forever - and
 * on a 33 MHz R3000 "bounded" still has to mean "short".
 *
 * KBD_ACK_SPINS was 20000, which is about 6 ms of polling a register. The last
 * byte of a frame never gets an acknowledgement, so every poll burned the full
 * timeout: roughly a third of the CPU, in stalls far longer than SIO1's 8-byte
 * receive FIFO holds at 115200 (~700 us). The visible symptom was serial input
 * losing characters - a command typed into the monitor arrived a letter short.
 *
 * A device acknowledges within ~100 us, so this is generous even now.
 */
#define KBD_TX_SPINS	4000
#define KBD_RX_SPINS	4000
#define KBD_ACK_SPINS	2000

/*
 * Poll rate. Every tick at HZ=100, so 100 Hz.
 *
 * The adapter reports at most one event per frame and does not queue: an event
 * that happens between two polls is simply lost, which is why a fast "ll" could
 * arrive as one keypress. Sampling twice as often halves that window, and it
 * costs little because a frame now stops as soon as nn says there is nothing
 * to collect - the common case by a wide margin (244 idle frames to 237 busy
 * ones even while typing).
 *
 * When nothing answers, back off hard. An absent keyboard would otherwise
 * spend a slice of every tick clocking fifteen bytes at 250 kHz into a bus
 * with nothing on it, for the whole uptime of a 33 MHz machine.
 */
#define KBD_POLL_TICKS	1
#define KBD_IDLE_TICKS	(HZ)
#define KBD_QUIET_MAX	50

static struct timer_list psxkbd_timer;
static int psxkbd_slot;			/* 0 = port 1, 1 = port 2 */
static unsigned char psxkbd_addr = 0x01;/* 0x01 direct, 0x01+floor behind a tap */
static int psxkbd_scan;			/* which slot/addr pair to try next */
static int psxkbd_quiet;		/* consecutive polls with no keyboard */
static int psxkbd_present;
static int psxkbd_announced;

/* Set 2 prefix state, carried across frames: a make code can be split. */
static int psxkbd_ext;
static int psxkbd_brk;

/*
 * There is deliberately no frame de-duplication here, and that is worth
 * recording because an earlier version had it and it was wrong.
 *
 * A trace of real frames settled what this protocol is:
 *
 *   nn=0:            244 times   nothing happened
 *   nn=1: 59         237 times   right shift make
 *   nn=2: f0 59       26 times   right shift break
 *   nn=1: 44 / f0 44  13 each    'o' down / up
 *
 * Make and break codes, one event per frame, nn=0 when idle - an *event*
 * stream, not a report of which keys are currently held. Two identical
 * consecutive frames are therefore two genuine keypresses, and suppressing
 * the second ate real input: typing "hello" produced "helo".
 *
 * The unbroken column of shift-makes that prompted the de-duplication was
 * ordinary typematic autorepeat (237 makes against 26 releases - a held key),
 * which the console layer already handles. What made it look pathological was
 * the debug printk, not the events.
 */

/*
 * Bring-up tracing: print every key as it is decoded. On by default while the
 * driver is new, because until brsh reads /dev/console there is nothing else
 * to prove that keystrokes are arriving - they land in tty1's input buffer
 * with no reader.
 *
 * Poke it to 1 from BRMON to turn it on without rebuilding; the address is in
 * System.map as `psxkbd_debug`.
 */
static int psxkbd_debug;	/* off: the driver is proven, tracing is opt-in */

/*
 * PS/2 Scan Code Set 2 -> Linux keycode.
 *
 * Paired arrays rather than a sparse initialiser, because EGCS 2.91 is the
 * compiler here. The letter, digit and punctuation entries were verified
 * against a real keyboard through BRMON before this driver was written.
 */
static const unsigned char psxkbd_set2[][2] = {
	/* letters */
	{0x1c, 30},{0x32, 48},{0x21, 46},{0x23, 32},{0x24, 18},{0x2b, 33},
	{0x34, 34},{0x33, 35},{0x43, 23},{0x3b, 36},{0x42, 37},{0x4b, 38},
	{0x3a, 50},{0x31, 49},{0x44, 24},{0x4d, 25},{0x15, 16},{0x2d, 19},
	{0x1b, 31},{0x2c, 20},{0x3c, 22},{0x2a, 47},{0x1d, 17},{0x22, 45},
	{0x35, 21},{0x1a, 44},
	/* digits */
	{0x45, 11},{0x16,  2},{0x1e,  3},{0x26,  4},{0x25,  5},{0x2e,  6},
	{0x36,  7},{0x3d,  8},{0x3e,  9},{0x46, 10},
	/* punctuation */
	{0x0e, 41},{0x4e, 12},{0x55, 13},{0x5d, 43},{0x54, 26},{0x5b, 27},
	{0x4c, 39},{0x52, 40},{0x41, 51},{0x49, 52},{0x4a, 53},
	/* control keys */
	{0x66, 14},{0x0d, 15},{0x5a, 28},{0x29, 57},{0x76,  1},{0x58, 58},
	{0x12, 42},{0x59, 54},{0x14, 29},{0x11, 56},
	/* function keys */
	{0x05, 59},{0x06, 60},{0x04, 61},{0x0c, 62},{0x03, 63},{0x0b, 64},
	{0x83, 65},{0x0a, 66},{0x01, 67},{0x09, 68},{0x78, 87},{0x07, 88},
	{0x7e, 70},{0x77, 69},
	/* keypad */
	{0x7c, 55},{0x7b, 74},{0x79, 78},{0x71, 83},{0x70, 82},{0x69, 79},
	{0x72, 80},{0x7a, 81},{0x6b, 75},{0x73, 76},{0x74, 77},{0x6c, 71},
	{0x75, 72},{0x7d, 73},
	{0x00, 0}
};

/* Set 2 with an 0xE0 prefix -> Linux keycode. */
static const unsigned char psxkbd_set2_e0[][2] = {
	{0x14,  97},	/* right ctrl  */
	{0x11, 100},	/* right alt   */
	{0x1f, 125},	/* left meta   */
	{0x27, 126},	/* right meta  */
	{0x2f, 127},	/* menu        */
	{0x70, 110},	/* insert      */
	{0x71, 111},	/* delete      */
	{0x6c, 102},	/* home        */
	{0x69, 107},	/* end         */
	{0x7d, 104},	/* page up     */
	{0x7a, 109},	/* page down   */
	{0x75, 103},	/* up          */
	{0x72, 108},	/* down        */
	{0x6b, 105},	/* left        */
	{0x74, 106},	/* right       */
	{0x4a,  98},	/* keypad /    */
	{0x5a,  96},	/* keypad enter*/
	{0x00, 0}
};

static int psxkbd_keycode(int code, int ext)
{
	const unsigned char (*t)[2] = ext ? psxkbd_set2_e0 : psxkbd_set2;
	int i;

	for (i = 0; t[i][1]; i++)
		if (t[i][0] == code)
			return t[i][1];

	return 0;
}

/*
 * Clock one byte out and back. Returns the byte, or -1 on timeout.
 *
 * want_ack is 0 for the final byte of a frame, where no acknowledgement is
 * coming and waiting for one is pure waste.
 */
static int psxkbd_xfer(unsigned char out, int want_ack)
{
	long spins;
	int rx;

	for (spins = 0; !(JOY_STAT & 0x1); spins++)
		if (spins > KBD_TX_SPINS)
			return -1;

	JOY_DATA = out;

	for (spins = 0; !(JOY_STAT & 0x2); spins++)
		if (spins > KBD_RX_SPINS)
			return -1;

	rx = JOY_DATA;

	/*
	 * Wait for the device to pull /ACK. This is the pacing: clocking the
	 * next byte before the far end is ready makes it drop the transfer,
	 * which is how the memory card writes failed for a day.
	 */
	if (want_ack) {
		for (spins = 0; spins < KBD_ACK_SPINS; spins++)
			if (JOY_STAT & 0x80)
				break;
	}

	return rx & 0xff;
}

/*
 * Clock one Lightspan frame. Returns the number of bytes received.
 * The caller must already hold the SIO0 bus.
 */
static int psxkbd_frame(unsigned char *rx)
{
	unsigned char tx[KBD_FRAME_LEN] = {
		0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06
	};
	int i, got, want;

	tx[0] = psxkbd_addr;

	JOY_CTRL = 0;
	JOY_MODE = 0x000d;
	JOY_BAUD = 0x0088;
	JOY_CTRL = 0x0003 | (psxkbd_slot ? 0x2000 : 0);	/* TXEN | /JOYn | slot */

	/* The device needs a moment after select before it will answer. */
	udelay(100);

	/*
	 * Stop as soon as the frame has nothing more to say. The reply lags the
	 * sends, so nn sits two bytes after wherever 0x96 0x5A turns up rather
	 * than at a fixed offset; once it is in hand, only nn payload bytes are
	 * worth clocking. An idle frame ends after five bytes instead of fifteen.
	 */
	want = KBD_FRAME_LEN;

	for (i = 0; i < want; i++) {
		got = psxkbd_xfer(tx[i], i < want - 1);
		if (got < 0)
			break;
		rx[i] = (unsigned char)got;

		if (want == KBD_FRAME_LEN && i >= 2 &&
		    rx[i - 2] == KBD_ID && rx[i - 1] == KBD_READY) {
			int nn = rx[i];

			if (nn != KBD_NO_DEV) {
				want = i + 1 + nn;
				if (want > KBD_FRAME_LEN)
					want = KBD_FRAME_LEN;
			}
		}
	}

	JOY_CTRL = 0;		/* leave the bus deselected for bu.c */

	return i;
}

/*
 * Decode one frame and feed the console. Returns 1 if a keyboard answered.
 *
 * Anchors on finding 0x96 rather than trusting a fixed offset, because SIO0
 * replies lag one transfer behind the sends.
 */
static int psxkbd_decode(const unsigned char *rx, int n)
{
	int i, at = -1, num, code, keycode;

	/*
	 * Demand 0x96 *followed by* the 0x5A ready marker.
	 *
	 * Matching on 0x96 alone is not enough: when something else drives the
	 * bus at the same time the reply is garbage, and garbage containing a
	 * stray 0x96 was enough to make this report a keyboard and decode 0xff
	 * bytes as keystrokes. Two bytes in the right order is a far weaker
	 * coincidence.
	 */
	for (i = 0; i + 1 < n; i++)
		if (rx[i] == KBD_ID && rx[i + 1] == KBD_READY) {
			at = i;
			break;
		}

	if (at < 0 || at + 2 >= n)
		return 0;

	num = rx[at + 2];
	if (num == KBD_NO_DEV)
		return 1;		/* adapter there, no keyboard attached */

	if (num > n - (at + 3))
		num = n - (at + 3);

	/*
	 * Raw frame tracing. Whether this protocol delivers *events* (make and
	 * break codes as they happen) or *state* (the set of keys held right
	 * now) decides how the driver should behave, and the two want opposite
	 * fixes - so print what actually arrives rather than guess.
	 */
	if (psxkbd_debug) {
		printk("psxkbd: frame nn=%d:", num);
		for (i = 0; i < num; i++)
			printk(" %02x", rx[at + 3 + i]);
		printk("\n");
	}

	for (i = 0; i < num; i++) {
		code = rx[at + 3 + i];

		/* 0x00 is padding, 0xff is an idle/absent byte - neither is a key. */
		if (code == 0x00 || code == 0xff)
			continue;
		if (code == 0xe0) {
			psxkbd_ext = 1;
			continue;
		}
		if (code == 0xf0) {
			psxkbd_brk = 1;
			continue;
		}

		keycode = psxkbd_keycode(code, psxkbd_ext);

		if (keycode)
			handle_scancode((unsigned char)keycode, !psxkbd_brk);

		psxkbd_ext = 0;
		psxkbd_brk = 0;
	}

	return 1;
}

static void psxkbd_tick(unsigned long unused)
{
	unsigned char rx[KBD_FRAME_LEN];
	int n, alive = 0;

	/*
	 * With no keyboard, walk both ports and all four multitap floors.
	 *
	 * An adapter can sit behind a tap, where it answers 0x01+floor rather
	 * than 0x01 - a BlueRetro in tap port B is address 0x02. Scanning costs
	 * nothing at the idle poll rate and saves the user having to know, or
	 * having to tell us, where they plugged it in.
	 */
	if (!psxkbd_present) {
		psxkbd_slot = (psxkbd_scan >> 2) & 1;
		psxkbd_addr = 0x01 + (psxkbd_scan & 3);
		psxkbd_scan = (psxkbd_scan + 1) & 7;
	}

	/*
	 * bu.c owns SIO0 whenever a card transfer is in flight. Skip this
	 * sample rather than corrupt it; at 50 Hz nobody will notice.
	 */
	if (psx_sio0_trylock()) {
		n = psxkbd_frame(rx);
		psx_sio0_unlock();

		if (n > 0)
			alive = psxkbd_decode(rx, n);
	} else {
		/* Bus busy is not the same as no keyboard - do not back off. */
		alive = psxkbd_present;
	}

	if (alive) {
		psxkbd_quiet = 0;
		if (!psxkbd_present) {
			psxkbd_present = 1;
			printk(KERN_INFO "psxkbd: keyboard on port %d, "
			       "address %02x%s\n", psxkbd_slot + 1,
			       psxkbd_addr,
			       psxkbd_addr > 1 ? " (multitap floor)" : "");
			psxkbd_announced = 1;
		}
	} else if (psxkbd_quiet < KBD_QUIET_MAX) {
		psxkbd_quiet++;
	} else {
		psxkbd_present = 0;
		/* A half-typed sequence cannot be completed now. */
		psxkbd_ext = 0;
		psxkbd_brk = 0;
	}

	psxkbd_timer.expires = jiffies +
		(psxkbd_present ? KBD_POLL_TICKS : KBD_IDLE_TICKS);
	add_timer(&psxkbd_timer);
}

/* ------------------------------------------------------------------ */
/* The arch keyboard hooks drivers/char/keyboard.c expects              */
/* ------------------------------------------------------------------ */

/*
 * kbd_translate() is a pass-through: psxkbd_decode() has already produced a
 * Linux keycode, so there is nothing left to translate. This is the whole
 * reason CONFIG_PC_KEYB is off - pckbd_translate() would expect Set 1 from an
 * 8042 this machine does not have.
 */
int kbd_translate(unsigned char scancode, unsigned char *keycode, char raw_mode)
{
	*keycode = scancode;
	return 1;
}

int kbd_setkeycode(unsigned int scancode, unsigned int keycode)
{
	return -EINVAL;		/* the table is fixed */
}

int kbd_getkeycode(unsigned int scancode)
{
	return -EINVAL;
}

/*
 * A release with no matching press. It happens after a frame is dropped for
 * the bus, so treat it as an ordinary release rather than a stuck key.
 */
char kbd_unexpected_up(unsigned char keycode)
{
	return 0200;
}

void kbd_leds(unsigned char leds)
{
	/* The Lightspan protocol is one-way; there are no LEDs to set. */
}

void __init kbd_init_hw(void)
{
	psxkbd_slot = 0;
	psxkbd_addr = 0x01;
	psxkbd_quiet = KBD_QUIET_MAX;	/* start scanning, slowly */

	init_timer(&psxkbd_timer);
	psxkbd_timer.function = psxkbd_tick;
	psxkbd_timer.data = 0;
	psxkbd_timer.expires = jiffies + KBD_IDLE_TICKS;
	add_timer(&psxkbd_timer);

	printk(KERN_INFO "psxkbd: Lightspan keyboard driver, scanning SIO0 "
	       "(both ports, multitap floors 01-04)\n");
}
