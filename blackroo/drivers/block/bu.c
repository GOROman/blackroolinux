/*
** bu - PlayStation memory card block driver.
*/

#include <linux/config.h>

/*
 * BLACKROO 2026-08-21: turn on the driver's own diagnostics.
 *
 * bu_check() and friends print the exact state, hardware status and byte at
 * which a probe gives up — all of it behind #ifdef DEBUG and therefore
 * invisible. Detection currently finds cards on multitap sub-ports A and C
 * but not B, while the monitor's polled probe talks to B perfectly (it
 * answers 5A 5D and accepted a format), so the fault is in this driver's
 * sequence rather than the hardware. These printks say where.
 */
/* #define DEBUG 1 */   /* the printks that accidentally fixed the timing */

#ifdef CONFIG_PSX_LARGE_CARD
#define MAJOR_NR	BU_LARGE_MAJOR
#else
#define MAJOR_NR   BU_MAJOR
#endif

#include <linux/major.h>
#include <linux/malloc.h>
#include <linux/vmalloc.h>
#include <linux/blk.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/hdreg.h>
#include <linux/blkpg.h>

#include <asm/io.h>
#include <asm/delay.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/ps/interrupts.h>

#include "bu.h"

#define TRUE                  (1)
#define FALSE                 (0)

#define SECTOR_SZ_SHIFT       (9)

#define TIMEOUT_VALUE         (100)
#define CATCH_TIMEOUT         (100)
#define N_CHECKS              (10)

#undef CALCULATE_FLOOR
#define MYSTERIOUS_DELAY
#define MYSTERIOUS_DELAY_VALUE	1000

static int bu_seen[BU_MINORS];      /* card numbers already claimed */
static int bu_hardsects[BU_MINORS];
static int bu_blocksizes[BU_MINORS];
static int bu_sizes[BU_MINORS];
static volatile int bu_total = 0;
static bu_device_t bu_devices[BU_MINORS];
static int bu_current = -1;                // current card number
static volatile int bu_state = BU_NONE;   // current transfer state
static volatile int bu_lock = FALSE;      // driver locked flag
static volatile int bu_probing = FALSE;   // initial card sweep in progress
static volatile int bu_step = 0;          // current request step
static volatile int bu_open = FALSE;
static volatile int bu_try = 0;
static volatile int bu_continue = FALSE; // request function called from
                                            //interrupt handler or timeout handler
static struct timer_list bu_timer;
static bu_request_t bu_curr_request = {0, 0, 0};
static bu_t bu_curr = {&bu_curr_request};
static DECLARE_WAIT_QUEUE_HEAD (bu_wait);

static int bu_rd_routine (bu_t *bu);
int psx_sio0_trylock (void);
static void do_bu_request (request_queue_t * q);
void psx_sio0_unlock (void);
#define BU_DELAY(a) udelay(a)

/*
 * SIO0 bus sharing with the keyboard driver (drivers/char/psxkbd.c).
 *
 * The memory cards and a Lightspan keyboard live on the same SIO0 bus and are
 * told apart only by the address byte (0x81 card, 0x01 keyboard). Both drivers
 * therefore have to take turns on the hardware.
 *
 * bu_catch() cannot be reused for this: it sleeps, and the keyboard is polled
 * from a timer. These are the non-blocking counterpart, guarding the same
 * bu_lock so the two drivers are genuinely mutually exclusive.
 *
 * cli() around the test-and-set is not decoration. bu.c drives the cards from
 * the ACK interrupt, and an interrupt landing between the test and the set is
 * exactly the lost-wakeup shape that made card detection work in emulators and
 * fail on real hardware.
 *
 * A caller that loses the race must simply skip its turn. Dropping a keyboard
 * poll costs one 20 ms sample; interfering with a card transfer costs the
 * filesystem.
 */
unsigned long bu_kbd_grabs = 0;   /* diagnostic: keyboard poll acquisitions */

int psx_sio0_trylock (void) {
   int flags, got = 0;

   save_flags (flags);
   cli ();
   if (!bu_lock && !bu_probing) {
      bu_kbd_grabs++;
      bu_lock = TRUE;
      got = 1;
   }
   restore_flags (flags);

   return got;
}

void psx_sio0_unlock (void) {
   bu_lock = FALSE;
   /* bu_catch() sleeps on this when it finds the bus busy. */
   wake_up (&bu_wait);
}

static int bu_catch (int card, int checks, int timeout) {
   int i, flags;
   
   for (i = 0; i < checks; i++) {
	   save_flags(flags);
	   cli();
      if (bu_lock) {
         restore_flags (flags);
#ifdef DEBUG
   printk (KERN_INFO "bu_catch: card %d try lock goto sleep\n", card+1);   
#endif   
         sleep_on_timeout (&bu_wait, timeout);
#ifdef DEBUG
   printk (KERN_INFO "bu_catch: card %d try lock\n", card+1);   
#endif   
      }
      else {
         bu_lock = TRUE;
         restore_flags (flags);
#ifdef DEBUG
   printk (KERN_INFO "bu_catch: card %d catched\n", card+1);   
#endif   
         return 1;
      }
   }
#ifdef DEBUG
   printk (KERN_ERR "bu_catch: can't catch card %d\n", card+1);
#endif   
   return 0;
}

static void bu_interrupt (int irq, void * dev_id, struct pt_regs * regs) {
   int status;
   unsigned long flags;

   if (bu_current < 0) {
#ifdef DEBUG
      printk (KERN_ERR "bu_interrupt: no current device\n");
#endif
      return;
   }

   if (bu_open) {   
      bu_state = BU_READY;
      wake_up (&bu_wait);
   }
   else {
		status = bu_rd_routine (&bu_curr);

 		if (status < 0) {
         del_timer (&bu_timer);
#ifdef DEBUG
         printk ("bu_interrupt: operation filed\n");
#endif
         spin_lock_irqsave (&io_request_lock, flags);
         bu_continue = TRUE;
         do_bu_request (NULL);
         bu_continue = FALSE;
         spin_unlock_irqrestore (&io_request_lock, flags);
         return;
		}
      		
	   if (bu_curr.stop) {
         del_timer (&bu_timer);
         if (CURRENT->cmd == READ) {
            memcpy (CURRENT->buffer+(bu_step << BU_BLK_SHIFT), bu_curr_request.buffer, BU_BLK_SIZE);
         }
         bu_step++;
         bu_try = 0;
         spin_lock_irqsave (&io_request_lock, flags);
         bu_continue = TRUE;
         do_bu_request (NULL);
         bu_continue = FALSE;
         spin_unlock_irqrestore (&io_request_lock, flags);
         return;
      }
      else {
         mod_timer (&bu_timer, jiffies+bu_devices[bu_curr_request.card].timeout);
         return;
      }
   }
}

static void bu_timeout (unsigned long card) {
   unsigned long flags;

   /*
    * BLACKROO 2026-08-26: always release the sleeper, first, whatever else
    * this function decides to do.
    *
    * bu_ready() sits in `while (bu_state == BU_WAIT) sleep_on(&bu_wait)`, and
    * this timer is the ONLY thing that can rescue it when the card's
    * acknowledge interrupt does not arrive. Two of the three paths below used
    * to return without touching bu_state or waking the queue - the
    * bu_current < 0 path, and the !bu_open path - so a sleeper waited for a
    * wakeup that had already decided not to come. Not a race: a guaranteed
    * hang, and it took the whole machine with it because bread() never
    * returned.
    *
    * The symptom was a plain `blk d0 0 0` in the monitor stopping dead at
    * "bread(major 208, minor 0, block 0, 1024)..." with the kernel still
    * running around it.
    */
   bu_state = BU_TIMEOUT;
   wake_up (&bu_wait);

   if (bu_current < 0) {
#ifdef DEBUG
      printk (KERN_ERR "bu_timeout: no current device\n");
#endif
      return;
   }

   if (bu_open) {
      /* already woken above */
   }
   else {
      del_timer (&bu_timer);
#ifdef DEBUG
      printk ("bu_timeout: operation timeout\n");
#endif
      spin_lock_irqsave (&io_request_lock, flags);
      bu_continue = TRUE;
      do_bu_request (NULL);
      bu_continue = FALSE;
      spin_unlock_irqrestore (&io_request_lock, flags);
      return;
   }
}

static int bu_ready (int card) {
   unsigned long flags;

   /*
    * BLACKROO 2026-08-21: close the lost-wakeup race.
    *
    * This used to set bu_state, arm the timer, test bu_state and then
    * sleep_on() with interrupts enabled throughout — the original author
    * even left a "may be we lose interrupt ?" comment on the test. The
    * acknowledge IRQ can fire between the test and the sleep: bu_interrupt()
    * sets BU_READY and wakes a queue nobody is waiting on yet, the wakeup is
    * lost, and we sleep until the timer expires. The caller then reports
    * "card in slot N not found".
    *
    * On real hardware a memory card pulls /ACK roughly 100us after each
    * byte, so it very often wins that race; emulators deliver the interrupt
    * later and more coarsely and usually do not. That is exactly the
    * "detection works in DuckStation, fails on the console" behaviour
    * recorded in April.
    *
    * Verified with the monitor's polled probe on 2026-08-21: the card in
    * slot 0 answers correctly (FLAG 08, ID 5A 5D, ACK on every byte), so the
    * hardware and the byte sequence were never the problem.
    *
    * Disabling interrupts around the test makes it atomic; sleep_on() ->
    * schedule() re-enables them while we are actually asleep, so the IRQ can
    * still arrive and wake us.
    */
   save_flags (flags);
   cli ();

   bu_state = BU_WAIT;

   init_timer (&bu_timer);
   bu_timer.function = bu_timeout;
   bu_timer.data = NULL;
   bu_timer.expires = jiffies+bu_devices[card].timeout;
   add_timer (&bu_timer);

   /*
    * sleep_on_timeout, not sleep_on: a missed wakeup should cost a
    * timeout, not the machine. bu_timeout() above is now careful to always
    * wake us, but this driver has already produced one lost-wakeup bug
    * (see the comment at the top of this function) and an unbounded sleep
    * in a block driver's read path is how that became a hang.
    */
   while (bu_state == BU_WAIT) {
      if (!sleep_on_timeout (&bu_wait, bu_devices[card].timeout + HZ)) {
         bu_state = BU_TIMEOUT;
         break;
      }
   }

   restore_flags (flags);

   del_timer (&bu_timer);
   if (bu_state == BU_READY) return 0;
   else return -1;
}

static void bu_hw_init (void) {
	outw (0x40, BU_CONTROL);
	outb (0x88, BU_REGE);
	outb (0x0d, BU_REG8);
	outw (0x00, BU_CONTROL);
	BU_DELAY (10);
	outw (0x02, BU_CONTROL);
	BU_DELAY (10);
	outw (0x2002, BU_CONTROL);
	BU_DELAY (10);
	outw (0x00, BU_CONTROL);
}

static int bu_sw_init (bu_t *bu) {

	bu->cnt = 0;
	bu->stop = 0;

#ifdef CALCULATE_FLOOR
	bu->bu_request->floor = (bu->bu_request->block >> BU_FLOOR_SHIFT) & BU_FLOOR_MASK;  
	bu->bu_request->block = bu->bu_request->block & BU_BLOCK_MASK;
#endif

	bu->cs = ((bu->bu_request->block) & 0xff) ^ (((bu->bu_request->block) >> 8) & 0xff);
   if (bu->bu_request->mode == 'R')
		bu->state = 0;
	else if (bu->bu_request->mode == 'W')
		bu->state = 0x10;
   else
 		return -1; // unknown mode

	return 0;
}

static int bu_rd_state0 (bu_t *bu) {

	/* bit 13 of JOY_CTRL selects the physical port; the card address byte
	 * 0x81+floor selects the multitap sub-port behind it. */
	outw ((BU_PORT (bu->bu_request->card) << 13) | 0x1003, BU_CONTROL);

	/*
	 * BLACKROO 2026-08-21: let the select settle before addressing.
	 *
	 * Without this, detection found cards on some multitap sub-ports and not
	 * others (A and C yes, B no) even though the monitor's polled probe
	 * talked to every one of them and formatted them successfully. The
	 * difference was timing: the polled code waits ~2ms after asserting
	 * /JOYn, this did not.
	 *
	 * It was confirmed by accident — enabling the driver's DEBUG printks
	 * made detection start working, because the print latency supplied the
	 * delay. That is not something to rely on: with DEBUG off the timing
	 * vanishes and so do the cards.
	 */
	BU_DELAY (2000);

	bu->byte = inb (BU_DATA);
	outb( 0x81 + bu->bu_request->floor, BU_DATA );
#if 0	
	if (bu->bu_request->floor > 7)
		outb (0x81-bu->bu_request->floor, BU_DATA);
	else
		outb (0x81+bu->bu_request->floor, BU_DATA);
#endif
	return 1;
}

static int bu_rd_state1 (bu_t *bu) {
	bu->byte = inb (BU_DATA);
	outb (bu->bu_request->mode, BU_DATA);
	return 1;
}

static int bu_rd_state2(bu_t *bu) {
	bu->byte = inb (BU_DATA);
	outb (0, BU_DATA);
   bu->hw_state = bu->byte;
	return 1;
}

static int bu_rd_state3 (bu_t *bu) {
	bu->byte = inb (BU_DATA);
	outb (0, BU_DATA);
   if (bu->byte == 0x5a) return 1;
	return -1;
}

static int bu_rd_state4 (bu_t *bu) {
	bu->byte = inb (BU_DATA);
	outb (((bu->bu_request->block) >> 8) & 0xff, BU_DATA);
   if (bu->byte == 0x5d) return 1;
	return -1;
}

static int bu_rd_state5 (bu_t *bu) {
	bu->byte = inb (BU_DATA);
	outb ((bu->bu_request->block)&0xff, BU_DATA);
   return 1;
}

static int bu_rd_state6 (bu_t *bu) {
//  outw (0x1003, BU_CONTROL);
	bu->byte = inb (BU_DATA); // high byte of block address
	outb(0, BU_DATA);
   return 1;
}

static int bu_rd_state7 (bu_t *bu) {
//  outw (0x803, BU_CONTROL);
	bu->byte = inb (BU_DATA);
	outb (0, BU_DATA);
   if (bu->byte == 0x5c) return 1;
	return -1;
}

static int bu_rd_state8 (bu_t *bu)
{
	bu->byte = inb (BU_DATA);
	outb (0, BU_DATA);
   if (bu->byte == 0x5d) return 1;
	return -1;
}

static int bu_rd_state9 (bu_t *bu) {
//  outw (0x803, BU_CONTROL);
	bu->byte = inb (BU_DATA);
	outb (0, BU_DATA);
   if (bu->byte == ((bu->bu_request->block >> 8) & 0xff)) return 1;
	return -1;
}

static int bu_rd_state10 (bu_t *bu) {
	bu->byte = inb (BU_DATA);
	outb (0, BU_DATA);
   if (bu->byte == (bu->bu_request->block & 0xff)) return 1;
	return -1;
}

static int bu_rd_state12 (bu_t *bu) {
	int timeout = 0;

	bu->byte = inb (BU_DATA);
	outb (0, BU_DATA);
	if (bu->bu_request->mode == 'R')
	   if (bu->byte != bu->cs) return -1;

   while ((inw (BU_STATUS) & 7) != 7)
		if (timeout++ > 10000) return -1;

	bu->byte = inb (BU_DATA);
	outb (0, BU_DATA);
   if (bu->byte == 0x47) {
		bu->stop = 1;
		return 1;
	}
	return -1;
}

static int bu_wr_state7 (bu_t *bu) {
//	inb (BU_DATA);
	inb (BU_DATA);
	outb (bu->cs, BU_DATA);
	return 1;
}

static int bu_rd_data (bu_t *bu) {
	__u8 byte = inb (BU_DATA);
	bu->bu_request->buffer[bu->cnt] = byte;
	bu->cs ^= byte;
	outb (0x00, BU_DATA);
	bu->cnt++;
	return ((bu->cnt == BU_BLK_SIZE) ? 1 : 0);
}

static int bu_wr_data (bu_t *bu) {
	__u8 byte = bu->bu_request->buffer[bu->cnt]; 
	bu->cs ^= byte;
	inb (BU_DATA);
	outb (byte, BU_DATA);
	bu->cnt++;
	return ((bu->cnt == BU_BLK_SIZE) ? 1 : 0);
}

int ((*bu_rd_states[])(bu_t*)) = {
	bu_rd_state0, bu_rd_state1, bu_rd_state2, bu_rd_state3, bu_rd_state4,
	bu_rd_state5, bu_rd_state6, bu_rd_state7, bu_rd_state8, bu_rd_state9,
	bu_rd_state10, bu_rd_data, bu_rd_state12, NULL,					NULL,
	NULL,
	bu_rd_state0, bu_rd_state1, bu_rd_state2, bu_rd_state3, bu_rd_state4,
	bu_rd_state5, bu_wr_data, bu_wr_state7,	bu_rd_state6,  	bu_rd_state7,
	bu_rd_state12
};	

int bu_rd_routine (bu_t *bu) {
	int i;
	int status;
   
#ifdef MYSTERIOUS_DELAY
	for(i = 0; i < MYSTERIOUS_DELAY_VALUE; i++);
#endif	
	outw (inw (BU_CONTROL) | 0x13 | (((bu->bu_request->card) & 1) << 13), BU_CONTROL);
	status = (bu_rd_states[bu->state]) (bu);
	//outw (inw (BU_CONTROL) | 0x10, BU_CONTROL);
	if (status<0) return -1; //error
	bu->state += status;
#if 0
	if (bu->stop == 0)
	; // end of packet transmission
#endif
	return 0; //ok
}

/*
 * Re-run the queue after the keyboard lets go of SIO0.
 *
 * do_bu_request() below returns without doing anything if bu_lock is held -
 * psxkbd polls the same controller bus and takes that lock 100 times a second.
 * Returning is correct (the request function must not sleep waiting for a
 * bus), but returning ALONE is not: the request stays on the queue and nothing
 * ever calls the request function again, so bread() waits for a completion
 * that will never come. The console hangs, in the monitor, on a plain read.
 *
 * That is why `card rd 0` worked in August 2026 and a block read stopped
 * working later: the keyboard driver did not exist yet. The lock was doing its
 * job; the caller was dropping the request on the floor.
 *
 * So: if the bus is busy, arm a one-jiffy timer and try again. psxkbd holds
 * the lock for one poll, so a retry a tick later almost always wins.
 */
static struct timer_list bu_retry_timer;
static int bu_retry_armed = FALSE;
unsigned long bu_defers = 0;   /* diagnostic: see the defer branch. Not static - BRMON reads it. */

static void bu_retry (unsigned long unused)
{
   unsigned long flags;

   bu_retry_armed = FALSE;

   /* The block layer calls the request function with io_request_lock held. */
   spin_lock_irqsave (&io_request_lock, flags);
   do_bu_request (BLK_DEFAULT_QUEUE (MAJOR_NR));
   spin_unlock_irqrestore (&io_request_lock, flags);
}

static void bu_arm_retry (void)
{
   if (bu_retry_armed)
      return;
   bu_retry_armed = TRUE;
   bu_retry_timer.expires = jiffies + 1;
   add_timer (&bu_retry_timer);
}

static void do_bu_request (request_queue_t * q) {

   static u_long start, len;
	static int card;
   int status = 0;
#ifdef CONFIG_PSX_LARGE_CARD
	int j;
#endif


   if (!bu_continue) {
start: 
		if (bu_lock) {
			/* SIO0 is busy (psxkbd). Come back a tick later rather than
			 * abandoning the request - see bu_arm_retry() above. */
			/*
			 * Diagnostic, 2026-08-27: a userspace mount(2) of this
			 * device hangs where the kernel's own root mount
			 * succeeds. One hypothesis is that psxkbd's 100 Hz poll
			 * takes bu_lock often enough to starve this retry once a
			 * keyboard has locked on. Count the deferrals: a number
			 * that climbs without bound proves it, and silence
			 * disproves it.
			 */
			bu_defers++;
			if (bu_defers == 1 || bu_defers == 10 ||
			    bu_defers == 100 || bu_defers == 1000 ||
			    bu_defers == 10000 || (bu_defers % 50000) == 0)
				printk (KERN_ERR DEVICE_NAME
					": request deferred x%lu (bu_lock held)\n",
					bu_defers);
			bu_arm_retry ();
			return;
		}
      
      while (TRUE)
      {
	      INIT_REQUEST;
      
         card = DEVICE_NR (CURRENT->rq_dev);
#ifdef CONFIG_PSX_LARGE_CARD
         if (card > 0) {
#else
         if (card >= BU_MINORS) {
#endif
	         printk (KERN_ERR DEVICE_NAME ": request for unknown device: %d\n", card+1);
	         end_request (FALSE);
	         continue;
	      }
      
         start = CURRENT->sector << (SECTOR_SZ_SHIFT-BU_BLK_SHIFT);
         len = CURRENT->current_nr_sectors << (SECTOR_SZ_SHIFT-BU_BLK_SHIFT);
#ifndef CONFIG_PSX_LARGE_CARD
		   bu_total = bu_devices[card].first_block.size-BU_FIRST_BLOCKS;
#endif

#ifdef DEBUG
	      printk (KERN_INFO DEVICE_NAME ": request for %d card, start=%ld(%ld), len=%ld(%ld), total=%d\n",
		      card+1, CURRENT->sector, start, CURRENT->current_nr_sectors, len, bu_total);
#endif

	      if ((start+len) > bu_total) {
	         printk (KERN_ERR DEVICE_NAME ": bad access: block=%ld, count=%ld\n",
		         CURRENT->sector,
		         CURRENT->current_nr_sectors);
	         end_request (FALSE);
	         continue;
	      }

	      if (CURRENT->cmd == READ) {
            bu_curr_request.mode = 'R';
         }
         else if (CURRENT->cmd == WRITE) {
            bu_curr_request.mode = 'W';
         }
         else {
	         printk (KERN_ERR DEVICE_NAME ": bad command: %d\n", CURRENT->cmd);
	         end_request (FALSE);
	         continue;
	      }

#ifndef CONFIG_PSX_LARGE_CARD
         bu_curr_request.card = card;
#endif
	      
         bu_lock = TRUE;
         bu_current = card;
         bu_step = 0;
         bu_try = 0;
         break;
      }
   }
   
   if (bu_step < len) {
      if (bu_try >= N_CHECKS) {
	      printk (KERN_ERR DEVICE_NAME ": block operation for card %d filed: err=0x%x,"
            "hw_status=0x%x,byte=0x%x\n", bu_curr_request.card+1,
            bu_curr.state, bu_curr.hw_state, (int)bu_curr.byte);
	      end_request (FALSE);
		   bu_current = -1;
		   bu_lock = FALSE;
			if (! QUEUE_EMPTY)
				goto start;
			else return;
      }
      else bu_try++;
      
      // fill low-level request data
      if (CURRENT->cmd == WRITE) {
         memcpy (bu_curr_request.buffer, CURRENT->buffer+(bu_step << BU_BLK_SHIFT), BU_BLK_SIZE);
      }
      bu_curr_request.block = start+bu_step;
#ifdef CONFIG_PSX_LARGE_CARD
		for (j = 0; j < BU_MINORS; j++) {
			if (bu_devices[j].first_block.size > 0) {
				bu_curr_request.block += BU_FIRST_BLOCKS;
				if (bu_curr_request.block < bu_devices[j].first_block.size) break;
				bu_curr_request.block -= bu_devices[j].first_block.size;
			}
		}
      if (j >= BU_MINORS) {
      	printk (KERN_ERR DEVICE_NAME ": bad card number found: %d\n",
      		j+1);
         end_request (FALSE);
			bu_current = -1;   
			bu_lock = FALSE;
			if (! QUEUE_EMPTY)
				goto start;
			else return;
      }
		bu_curr_request.card = j;
#else
		bu_curr_request.block += BU_FIRST_BLOCKS;
#endif
      bu_curr_request.floor = BU_FLOOR (bu_curr_request.card);

#ifdef DEBUG
	   printk (KERN_INFO DEVICE_NAME ": block operation card=%d, op=%c, block=%d\n",
		   bu_curr_request.card+1, bu_curr_request.mode, bu_curr_request.block);
#endif

      // init one block operation
      bu_state = BU_NONE;
         
      if ((status = bu_sw_init (&bu_curr)) < 0) {
	      printk (KERN_ERR DEVICE_NAME ": block operation for card %d filed: err=0x%x,"
            "status=0x%x,hw_status=0x%x,byte=0x%x\n", bu_curr_request.card+1, status,
            bu_curr.state, bu_curr.hw_state, (int)bu_curr.byte);
	      end_request (FALSE);
		   bu_current = -1;
		   bu_lock = FALSE;
			if (! QUEUE_EMPTY)
				goto start;
			else return;
      }
      
      bu_hw_init ();

	   status = bu_rd_routine (&bu_curr);

 	  if (status < 0) {
	      printk (KERN_ERR DEVICE_NAME ": block operation for card %d filed: err=0x%x,"
            "status=0x%x,hw_status=0x%x,byte=0x%x\n", bu_curr_request.card+1, status,
            bu_curr.state, bu_curr.hw_state, (int)bu_curr.byte);
	      end_request (FALSE);
		   bu_current = -1;
		   bu_lock = FALSE;
			if (! QUEUE_EMPTY)
				goto start;
			else return;
      }
   
   	init_timer (&bu_timer);
   	bu_timer.function = bu_timeout;
   	bu_timer.data = NULL;
   	bu_timer.expires = jiffies+bu_devices[bu_curr_request.card].timeout;
   	add_timer (&bu_timer);
      
      return;
   }
   else {
#ifdef DEBUG
	   printk (KERN_INFO DEVICE_NAME ": request for %d card done\n", card+1);
#endif
	   end_request (TRUE);
		bu_current = -1;
		bu_lock = FALSE;
		if (! QUEUE_EMPTY)
			goto start;
		else return;
   }
}

static int bu_check (bu_t * bu) {
	int status;
   int try = 0;

try:
   if (try >= N_CHECKS) {
      outw (0, BU_CONTROL);	/* release the bus on the way out too */
      BU_DELAY (100);
   	return 0;
   }
   else try++;

   if (bu_sw_init (bu) < 0) return 0;
   
   bu_hw_init ();
   
	do
	{
      if ((bu->state & 0xf) != 0) {
         if (bu_ready (bu->bu_request->card) < 0) {
#ifdef DEBUG
          printk (KERN_ERR DEVICE_NAME ": check operation for card %d timeout: st=0x%x,"
               "status=0x%x,hw_status=0x%x,byte=0x%x,control=0x%x,count=%d\n", 
              bu->bu_request->card+1, inw (BU_STATUS),
              bu->state, bu->hw_state, (int)bu->byte, inw (BU_CONTROL), bu->cnt);
#endif
            goto try;
	      }
      }

	   status = bu_rd_routine (bu);

 	   if (status < 0) {
#ifdef DEBUG
       printk (KERN_ERR DEVICE_NAME ": check operation for card %d filed: err=0x%x,"
            "status=0x%x,hw_status=0x%x,byte=0x%x\n", bu->bu_request->card+1, status,
            bu->state, bu->hw_state, (int)bu->byte);
#endif
         goto try;
	   }		
	} while (!bu->stop);

   /*
    * BLACKROO 2026-08-21: release the bus.
    *
    * bu_rd_state0() asserts /JOYn (JOY_CTRL bit 1) to select the card and
    * nothing ever deasserts it, so a *successful* transfer leaves the port
    * still selecting that card. The next slot's probe then starts against a
    * bus that is already held, and fails; its failure path happens to leave
    * things clean, so the probe after that succeeds. Hence the giveaway
    * pattern of every other slot failing: found, not found, found, not found
    * — with four cards that had all just been formatted successfully.
    *
    * A register dump taken early in the session showed exactly this and it
    * went unnoticed: JOY_CTRL = 0x3003, /JOYn still asserted, long after the
    * driver had finished with the card.
    */
   outw (0, BU_CONTROL);
   BU_DELAY (100);

   return 1;
}

static int bu_read_first_block (int card) {
	bu_t bu = {0};
   bu_request_t bu_request = {0, 0, 0};
   union {
      __u8  fill[128];
      bu_first_block_t block;
   } first_block;

	bu_request.block = 0x0;
	bu_request.mode = 'R';
	bu_request.card = card;
   bu_request.floor = BU_FLOOR (card);
	bu.bu_request = &bu_request;
	if (!bu_check (&bu)) {
		// the block is unreadable, no blocks on the floor 
#ifdef DEBUG
      printk (KERN_ERR DEVICE_NAME ": can't read %d card first block\n", card+1);
#endif
		return 0;
	}
	
	memcpy (first_block.fill, bu_request.buffer, BU_BLK_SIZE);
	
	if (first_block.block.id != BU_ID) {
		// bad card id - card wasn't write properly
#ifdef DEBUG
      printk (KERN_ERR DEVICE_NAME ": bad card id: 0x%x\n", first_block.block.id);
#endif
		return 0;
	}
	
	bu_devices[card].first_block.id = first_block.block.id;
	bu_devices[card].first_block.size = first_block.block.size;
	bu_devices[card].first_block.serial = first_block.block.serial;
	bu_devices[card].first_block.number = first_block.block.number;
	
	return 1;
}

static int bu_do_open (struct inode *inode, struct file *filp) {
   int card = DEVICE_NR (inode->i_rdev);

#ifdef DEBUG   
	printk ("try to open %d card (curr=%d, lock=%d)\n", card+1, bu_current, bu_lock);
#endif

#ifdef CONFIG_PSX_LARGE_CARD
	if (card > 0) return -ENODEV;
#else
   if (card >= BU_MINORS) return -ENODEV;
#endif
	
   if (!bu_devices[card].usage) {
      check_disk_change (inode->i_rdev);
      if (bu_sizes[card] == 0) {
         return -ENXIO;
      }
   }
   bu_devices[card].usage++;
      
   MOD_INC_USE_COUNT;

#ifdef DEBUG   
	printk ("%d card opened\n", card+1);
#endif
	
   return 0;
}

static int bu_release (struct inode *inode, struct file *filp) {
   int card = DEVICE_NR (inode->i_rdev);

#ifdef DEBUG   
	printk ("try to release %d card (curr=%d, lock=%d)\n", card+1, bu_current, bu_lock);
#endif

   bu_devices[card].usage--;
   
   MOD_DEC_USE_COUNT;

   return 0;
}

static int bu_check_change (kdev_t dev) {

#ifdef DEBUG   
	printk ("try to check card change\n");
#endif

#ifdef CONFIG_PSX_LARGE_CARD
   if (MINOR (dev) > 0) return 0;
#else
   if (MINOR (dev) >= BU_MINORS) return 0;
#endif
   
   return 1;
}

/* Serials accepted so far in the sweep in progress. A repeat means the slot
 * is echoing a card that lives somewhere else - see bu_revalidate(). */
static unsigned long bu_serials[BU_MINORS];
static int bu_nserials;

static int bu_serial_seen (unsigned long serial, int slot)
{
   int k;

   for (k = 0; k < bu_nserials; k++)
      if (bu_serials[k] == serial)
         return 1;

   if (bu_nserials < BU_MINORS)
      bu_serials[bu_nserials++] = serial;
   return 0;
}

static int bu_revalidate (kdev_t dev) {
   int card = DEVICE_NR (dev);
   int i, bu_size = 0;
#ifdef CONFIG_PSX_LARGE_CARD
	int n = 0;
	/* which card numbers have been claimed in this sweep - see the
	 * range/duplicate check below, and the same array in bu_init() */
	char bu_seen[BU_MINORS];

	for (i = 0; i < BU_MINORS; i++)
		bu_seen[i] = 0;
	bu_nserials = 0;
#endif

#ifdef DEBUG   
	printk ("try to revalidate %d card (curr=%d, lock=%d)\n", card+1, bu_current, bu_lock);
#endif

   if (!bu_catch (card, N_CHECKS, CATCH_TIMEOUT)) {
#ifdef DEBUG   
      printk (KERN_ERR DEVICE_NAME ": can't lock card %d: device busy\n", card);
#endif
      return -EBUSY;
   }
   bu_current = card;
   bu_state = BU_NONE;
   bu_open = TRUE;
   
   bu_total = 0;

   /*
    * BLACKROO 2026-08-26: this loop re-probes every slot, exactly as the boot
    * sweep in bu_init() does - and it was missing both of the things that
    * sweep was fixed to do in August.
    *
    *   1. bu_probing keeps psxkbd off SIO0. Without it the keyboard's 100 Hz
    *      poll interleaves with the probe and corrupts the first block, which
    *      reads back as a different card.
    *   2. A multitap needs a gap between transactions. Probing back to back
    *      made detection alternate slot by slot on known-good cards.
    *
    * The symptom here was subtler than "not found": an EMPTY slot came back
    * as a present card carrying the previous slot's number, so a correctly
    * numbered 0,1,2 set was rejected with "Bad card sequence - found 1
    * instead 0" and the whole device refused to open. Renumbering the cards
    * did not help, because the numbers were never the problem.
    */
   bu_probing = TRUE;

#ifdef CONFIG_PSX_LARGE_CARD
	for (i = 0, n = 0; i < BU_MINORS; i++) {
      BU_DELAY (5000);
#else
	{
		i = card;
#endif
		if (!bu_read_first_block (i)) {    
         printk (KERN_INFO DEVICE_NAME ": card in slot %d not found\n", i+1);
         bu_sizes[i] = 0;
      }
      else {
#ifdef CONFIG_PSX_LARGE_CARD
			/*
			 * BLACKROO 2026-08-26: the same fix bu_init() got in August,
			 * which this copy of the sweep never received.
			 *
			 * Comparing against a running counter means one odd slot
			 * poisons the whole device. On this machine a multitap slot
			 * with no card in it hands back a NEIGHBOUR'S first block -
			 * magic and all - so slot 1 (port 1 tap A, where a DualShock
			 * lives and no card does) read as a card numbered 1, and a
			 * perfectly good 0,1,2 set was rejected with "found 1
			 * instead 0".
			 *
			 * Judge each card on its own recorded number: in range, and
			 * not already claimed. A slot that fails that is simply not
			 * a card - skip it, do not condemn the device.
			 */
			/*
			 * BLACKROO 2026-08-27: reject by SERIAL, not just by number.
			 *
			 * An empty multitap slot does not read as empty - it hands
			 * back a neighbouring card's first block, magic and all.
			 * Slot 1 (port 1 tap A, a DualShock and no card) reads as a
			 * 127 KB card every sweep. Checking the card NUMBER cannot
			 * see that, because the phantom carries a perfectly valid
			 * one; it just belongs to somebody else.
			 *
			 * Every card gets a unique serial when it is formatted, so a
			 * repeat is proof the slot is echoing another card rather
			 * than holding one.
			 */
			if (bu_devices[i].first_block.number >= BU_MINORS ||
			    bu_seen[bu_devices[i].first_block.number] ||
			    bu_serial_seen (bu_devices[i].first_block.serial, i)) {
	   		printk (KERN_INFO DEVICE_NAME ": slot %d ignored - number %d serial %08x already claimed\n",
	   			i + 1, bu_devices[i].first_block.number,
	   			bu_devices[i].first_block.serial);
            bu_sizes[i] = 0;
			}
			else {
			   bu_seen[bu_devices[i].first_block.number] = 1;
#endif	
      	bu_sizes[i] = (bu_devices[i].first_block.size >> (10-BU_BLK_SHIFT))-1;
			bu_size += bu_sizes[i];
			bu_total += bu_devices[i].first_block.size-BU_FIRST_BLOCKS;
         printk (KERN_INFO DEVICE_NAME ": %d Kbytes card found in slot %d\n", 
         	bu_sizes[i], i+1);
#ifdef CONFIG_PSX_LARGE_CARD
			}
         n++;
#endif	
      }   
	}
      
#ifdef CONFIG_PSX_LARGE_CARD
	bu_sizes[0] = bu_size;
#endif	

   bu_current = -1;   
   bu_open = FALSE;
   bu_lock = FALSE;

   /* Give SIO0 back to the keyboard. Every exit from here must clear this,
    * or psxkbd is locked off the bus for the rest of the boot. */
   bu_probing = FALSE;
   wake_up (&bu_wait);
	
	if (bu_size == 0) {
      return -ENODEV;
   }
   
   return 0;
}

static int bu_ioctl (struct inode * inode, struct file * filp, unsigned int cmd, unsigned long arg) {
	int err;
	long size;
	struct hd_geometry geo;

#ifdef DEBUG
	printk ("bu_ioctl: cmd=%d\n", cmd);
#endif
	
	switch (cmd) {
		case BLKGETSIZE:
			if (!arg) return -EINVAL;
			err = !access_ok (VERIFY_WRITE, arg, sizeof (long));
			if (err) return -EFAULT;
			size = bu_sizes[MINOR (inode->i_rdev)]*1024/bu_hardsects[MINOR (inode->i_rdev)];
			if (copy_to_user ((long *)arg, &size, sizeof (long))) return -EFAULT;
			return 0;
			
		case BLKRRPART:
			return -ENOTTY;
			
		case HDIO_GETGEO:
			err = !access_ok (VERIFY_WRITE, arg, sizeof (geo));
			if (err) return -EFAULT;
			size = bu_sizes[MINOR (inode->i_rdev)]*1024/bu_hardsects[MINOR (inode->i_rdev)];
			geo.cylinders = (size & ~0x3f) >> 6;
			geo.heads = 4;
			geo.sectors = 16;
			geo.start = 4;
			if (copy_to_user ((void *)arg, &geo, sizeof (geo))) return -EFAULT;
			return 0;
			
		default:
			return blk_ioctl (inode->i_rdev, cmd, arg);
	}
	
	return -ENOTTY;
}

static struct block_device_operations bu_fops =
{
	open:		bu_do_open,
	release:	bu_release,
	ioctl:	  bu_ioctl,
   check_media_change: bu_check_change,
   revalidate: bu_revalidate,
};

int __init bu_init (void) {
   int i, bu_size = 0, n = 0;

#ifdef CONFIG_PSX_LARGE_CARD
   if (register_blkdev (MAJOR_NR, "bul", &bu_fops)) {
#else
   if (register_blkdev (MAJOR_NR, "bu", &bu_fops)) {
#endif
	   printk (KERN_ERR DEVICE_NAME ": Unable to get major %d\n",
	      MAJOR_NR);
	   return -EBUSY;
   }
   
   for (i = 0; i < BU_MINORS; i++) {
      bu_hardsects[i] = BU_HARDSECSIZE;
      bu_blocksizes[i] = BU_BSIZE;
      bu_sizes[i] = 0;
      bu_devices[i].timeout = TIMEOUT_VALUE;
   }

   init_timer (&bu_retry_timer);
   bu_retry_timer.function = bu_retry;
   bu_retry_timer.data = 0;

   blk_init_queue (BLK_DEFAULT_QUEUE (MAJOR_NR), DEVICE_REQUEST);
   blk_size[MAJOR_NR] = bu_sizes;
   blksize_size[MAJOR_NR] = bu_blocksizes;
   hardsect_size[MAJOR_NR] = bu_hardsects;
   read_ahead[MAJOR_NR] = BU_RAHEAD;

   i = request_irq (CONTROLLER, bu_interrupt, SA_INTERRUPT, DEVICE_NAME, NULL);
   if (i < 0) {
      printk (KERN_ERR DEVICE_NAME ": can't get irq %d\n", CONTROLLER);

      if (unregister_blkdev (MAJOR_NR, DEVICE_NAME) != 0)
         printk (KERN_ERR DEVICE_NAME ": unregister of device failed\n");
      
      blk_cleanup_queue (BLK_DEFAULT_QUEUE(MAJOR_NR));
      
      blk_size[MAJOR_NR] = NULL;
      blksize_size[MAJOR_NR] = NULL;
      hardsect_size[MAJOR_NR] = NULL;
      read_ahead[MAJOR_NR] = 0;
         
      return i;
   }
   
   /*
    * BLACKROO 2026-08-21: keep other SIO0 users off the bus for the sweep.
    *
    * The keyboard driver (drivers/char/psxkbd.c) polls SIO0 from a timer
    * started during console init, which is *before* this initcall runs. Left
    * to interleave, it corrupts detection - and a corrupted first block reads
    * as "card not found", so the card is silently dropped for the whole boot.
    * That is how /dev/bul went from 381 KB to 254 KB.
    *
    * This must NOT be bu_lock. The sweep below calls bu_catch(), which takes
    * bu_lock itself; holding it here makes every slot fail with "can't catch
    * card" and the driver comes up with nothing. bu_probing is a separate flag
    * that psx_sio0_trylock() honours, so outsiders stay out while bu.c's own
    * locking keeps working.
    */
   bu_probing = TRUE;

   // check of card existence
   for (i = 0, bu_total = 0, n = 0; i < BU_MINORS; i++) {
      /*
       * BLACKROO 2026-08-21: let the bus settle between slots.
       *
       * Probing back to back, detection succeeded on floors 0 and 2 and
       * failed on 1 and 3 — a perfect alternation, with all four cards known
       * good (each had just been formatted and read back through the same
       * multitap). A tap needs a gap between transactions; without one every
       * second probe lands while it is still busy.
       */
      BU_DELAY (5000);

      printk (KERN_INFO DEVICE_NAME ": detecting card in slot %d ...\n", i+1);
      if (!bu_catch (i, N_CHECKS, CATCH_TIMEOUT)) {
         printk (KERN_ERR DEVICE_NAME ": can't catch card in slot %d\n", i+1);
         continue;
      }
      bu_current = i;
      bu_state = BU_NONE;
      bu_open = TRUE;
		if (!bu_read_first_block (i)) {    
         printk (KERN_INFO DEVICE_NAME ": card in slot %d not found\n", i+1);
         bu_sizes[i] = 0;
      }
      else {
#ifdef CONFIG_PSX_LARGE_CARD
			/*
			 * BLACKROO 2026-08-21: judge the card on its own recorded
			 * number, not on how many cards happen to have been found
			 * so far.
			 *
			 * This used to compare against a running counter, so a
			 * single failed slot poisoned every card after it: with
			 * cards numbered 0..3 and slot 2 failing, slot 3 was read
			 * perfectly and then rejected with "found 2 instead 1".
			 * One flaky probe cost three cards.
			 *
			 * A card is accepted if its number is within range and has
			 * not already been claimed.
			 */
			if (bu_devices[i].first_block.number >= BU_MINORS ||
			    bu_seen[bu_devices[i].first_block.number]) {
	   		printk (KERN_ERR DEVICE_NAME ": Bad card sequence - found %d (range/duplicate)\n",
	   			bu_devices[i].first_block.number);
			}
			else {
			   bu_seen[bu_devices[i].first_block.number] = 1;
#endif	
      	bu_sizes[i] = (bu_devices[i].first_block.size >> (10-BU_BLK_SHIFT))-1;
			bu_size += bu_sizes[i];
			bu_total += bu_devices[i].first_block.size-BU_FIRST_BLOCKS;
         printk (KERN_INFO DEVICE_NAME ": %d Kbytes card found in slot %d\n", 
         	bu_sizes[i], i+1);
#ifdef CONFIG_PSX_LARGE_CARD
			}
#endif	
         n++;
      }
      bu_current = -1;
      bu_open = FALSE;
      bu_lock = FALSE;
   }

#ifdef CONFIG_PSX_LARGE_CARD
	bu_sizes[0] = bu_size;

   bu_probing = FALSE;		/* the keyboard may have the bus again */
   wake_up (&bu_wait);

   printk (KERN_INFO DEVICE_NAME ": driver initialized: %d cards joined, total size = %d Kbytes\n",
      BU_MINORS, bu_size);
#else
   printk (KERN_INFO DEVICE_NAME ": driver for %d cards initialized\n",
      BU_MINORS);
#endif

   return 0;
}

#if defined(MODULE)
int init_module (void) {
   int error;

   error = bu_init ();
   if (error == 0)
   {
      printk (KERN_INFO DEVICE_NAME ": loaded as module\n");
   }

   return error;
}

void cleanup_module (void)
{
   int i;
   
#ifdef CONFIG_PSX_LARGE_CARD
   fsync_dev (MKDEV (MAJOR_NR, 0));
#else
   for (i = 0; i < BU_MINORS; i++)
      fsync_dev (MKDEV (MAJOR_NR, i));
#endif

   free_irq (CONTROLLER, NULL);

   if (unregister_blkdev (MAJOR_NR, DEVICE_NAME) != 0)
      printk (KERN_ERR DEVICE_NAME ": unregister of device failed\n");

   blk_cleanup_queue (BLK_DEFAULT_QUEUE(MAJOR_NR));
      
   blk_size[MAJOR_NR] = NULL;
   blksize_size[MAJOR_NR] = NULL;
   hardsect_size[MAJOR_NR] = NULL;
   read_ahead[MAJOR_NR] = 0;

   return;
} 
#endif
