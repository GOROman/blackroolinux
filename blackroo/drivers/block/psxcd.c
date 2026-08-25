/*
 * psxcd.c - the PlayStation CD-ROM as a Linux block device.
 *
 * 700 MB the console has always been able to read, against the ~1 MB of RAM
 * this machine has left after the kernel. docs/24-CDROM-DRIVER-RESEARCH.md is
 * the design; every hardware question it marked VERIFY has since been answered
 * on real hardware through BRMON's `cd` command, and the answers are recorded
 * in docs/captures/2026-08-25-cdrom-first-sector.txt.
 *
 * What is settled and relied on here:
 *
 *   - The init and read command sequences work verbatim as documented.
 *   - DMA3 reads are byte-identical to PIO.
 *   - DMA needs **no cache invalidate**: the R3000A has no writeback data
 *     cache, and cached and uncached views of a buffer were compared byte for
 *     byte on hardware.
 *   - DMA needs **no address translation**: this kernel is linked in KUSEG, so
 *     a buffer's own address already is its physical address. Mask it to 24
 *     bits and hand it to MADR.
 *
 * Stage 1, deliberately: this reads **synchronously, polling**, one sector at a
 * time, from inside the request function. That is slower than it has to be and
 * it is not the shape docs/24 §5.6 describes - an interrupt-driven state
 * machine that keeps the drive streaming. It is written this way because it is
 * the version that can be *shown to be correct*: the same code path BRMON has
 * already proven, with a block device wrapped round it. Stage 2 is the
 * interrupt machine, and it should not be attempted until this one mounts a
 * filesystem.
 *
 * The one thing that would make stage 1 unbearable is avoided: `Pause` is only
 * issued on a discontinuity, never after every request. A Pause costs ~32 ms at
 * 2x and PSn00bSDK reports the controller ignores commands for roughly a
 * second afterwards, so pausing per request would be slower than the drive.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>

#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/delay.h>

#define MAJOR_NR	PSXCD_MAJOR
#define DEVICE_NAME	"psxcd"
#define DEVICE_REQUEST	do_psxcd_request
#define DEVICE_NR(dev)	(MINOR(dev))
#define DEVICE_NO_RANDOM
#include <linux/blk.h>

/* ------------------------------------------------------------------ */
/* Hardware layer                                                      */
/* ------------------------------------------------------------------ */

/*
 * KSEG1 throughout, matching head.S, brmon.c and PSn00bSDK. docs/24 §1.7 notes
 * the tree is inconsistent about this; being explicit costs nothing.
 *
 * Never do a 32-bit load from a CD port: auto-increment is off, so a word read
 * returns one byte four times over - and enabling auto-increment to "fix" that
 * breaks DMA, which relies on the bus splitting each word into four byte reads.
 */
#define CD0		(*(volatile unsigned char *)0xbf801800)
#define CD1		(*(volatile unsigned char *)0xbf801801)
#define CD2		(*(volatile unsigned char *)0xbf801802)
#define CD3		(*(volatile unsigned char *)0xbf801803)

#define CD_BUS_CFG	(*(volatile unsigned long *)0xbf801018)
#define CD_COM_DELAY	(*(volatile unsigned long *)0xbf801020)
#define IRQ_STAT	(*(volatile unsigned long *)0xbf801070)
#define IRQ_MASK	(*(volatile unsigned long *)0xbf801074)

#define D3_MADR		(*(volatile unsigned long *)0xbf8010b0)
#define D3_BCR		(*(volatile unsigned long *)0xbf8010b4)
#define D3_CHCR		(*(volatile unsigned long *)0xbf8010b8)
#define DMA_DPCR	(*(volatile unsigned long *)0xbf8010f0)

#define CHCR_BUSY	0x01000000
#define CHCR_CD_READ	0x11000000

#define HSTS_RSLRRDY	0x20
#define HSTS_DRQSTS	0x40
#define HSTS_BUSYSTS	0x80

#define IRQ_CD		0x0004

#define CDC_NOP		0x01
#define CDC_SETLOC	0x02
#define CDC_READN	0x06
#define CDC_PAUSE	0x09
#define CDC_INIT	0x0a
#define CDC_DEMUTE	0x0c
#define CDC_SETMODE	0x0e

#define PSXCD_SECTOR	2048

static unsigned long psxcd_saved_mask;

int  psxcd_hw_init(void);
int  psxcd_hw_read(unsigned long lba, void *buf);
int  psxcd_hw_cmd(int cmd, const unsigned char *par, int npar,
		  unsigned char *res, int *nres);
void psxcd_hw_stop(void);

/* Streaming state: ReadN runs until stopped, so remember where it is. */
static int psxcd_streaming;
static unsigned long psxcd_next_lba;
static int psxcd_present;

static void cd_take_irq (void)
{
   psxcd_saved_mask = IRQ_MASK;
   IRQ_MASK = psxcd_saved_mask & ~IRQ_CD;
}

static void cd_release_irq (void)
{
   IRQ_STAT = ~IRQ_CD;
   IRQ_MASK = psxcd_saved_mask;
}

static int cd_wait_busy (void)
{
   long spins;

   for (spins = 0; spins < 200000; spins++)
      if (!(CD0 & HSTS_BUSYSTS))
         return 1;

   return 0;
}

static int cd_int (void)
{
   CD0 = 1;
   return CD3 & 7;
}

/* MIPS interrupt first, then the controller. [docs/24 §2.4] */
static void cd_ack (void)
{
   IRQ_STAT = ~IRQ_CD;
   CD0 = 1;
   CD3 = 0x1f;
}

static int cd_wait_int (long spins)
{
   long i;

   for (i = 0; i < spins; i++) {
      int t = cd_int ();

      if (t)
         return t;
   }

   return -1;
}

/* Loop on RSLRRDY with a cap: reading past the end of the result FIFO pads
 * with zeroes and then silently repeats the response. [docs/24 §1.3] */
static int cd_result (unsigned char *buf, int max)
{
   int n = 0;

   while ((CD0 & HSTS_RSLRRDY) && n < max)
      buf[n++] = CD1;

   return n;
}

static int cd_do (int cmd, const unsigned char *par, int npar,
                  unsigned char *res, int *nres, long spins)
{
   int t, i;

   if (!cd_wait_busy ())
      return -1;

   CD0 = 0;
   for (i = 0; i < npar; i++)
      CD2 = par[i];
   CD1 = (unsigned char)cmd;

   t = cd_wait_int (spins);
   if (t < 0)
      return -1;

   *nres = cd_result (res, 16);
   cd_ack ();

   return t;
}

int psxcd_hw_cmd (int cmd, const unsigned char *par, int npar,
                  unsigned char *res, int *nres)
{
   int t;

   cd_take_irq ();
   t = cd_do (cmd, par, npar, res, nres, 400000);
   cd_release_irq ();

   return t;
}

int psxcd_hw_init (void)
{
   unsigned char res[16];
   int nres, t, i;

   cd_take_irq ();

   CD_BUS_CFG = 0x00020943;	/* 8-bit bus, auto-increment OFF */
   CD_COM_DELAY = 0x0000132c;	/* the BIOS default is gone by now */

   DMA_DPCR |= 0x00008000;	/* DMA3 master enable */
   D3_CHCR = 0;

   CD0 = 0;
   CD3 = 0x00;
   CD0 = 1;
   CD3 = 0x1f;			/* acknowledge every flag */
   CD2 = 0x1f;			/* enable every interrupt source */

   for (i = 0; i < 2; i++) {
      t = cd_do (CDC_NOP, 0, 0, res, &nres, 400000);
      if (t < 0) {
         cd_release_irq ();
         return 0;
      }
   }

   /* Init sets mode = 20h, so Setmode has to come after it, never before. */
   t = cd_do (CDC_INIT, 0, 0, res, &nres, 2000000);
   if (t == 3)
      if (cd_wait_int (4000000) > 0) {
         cd_result (res, 16);
         cd_ack ();
      }

   cd_do (CDC_DEMUTE, 0, 0, res, &nres, 400000);

   {
      unsigned char mode = 0x80;	/* 2x, 2048-byte data-only sectors */

      cd_do (CDC_SETMODE, &mode, 1, res, &nres, 400000);
   }

   cd_release_irq ();

   psxcd_streaming = 0;
   psxcd_present = 1;

   return 1;
}

/* Stop the drive streaming. Expensive - only on a discontinuity. */
/*
 * How long the controller is useless after a Pause.
 *
 * docs/24 quotes PSn00bSDK: "the drive controller will not process any command
 * properly for some time after a CdlPause command, so an external timer (the
 * vblank counter) and manual polling are required to defer the next attempt",
 * with a 60-vblank constant - about 1.2 s at PAL 50 Hz.
 *
 * This driver's header comment cites that cooldown as the reason never to
 * Pause per request, and then did not wait for it on the occasions when it
 * DOES Pause. The symptom is exactly what the first CD-root boot produced:
 *
 *     psxcd: found ROOT.IMG at LBA 827, 4096 KB      <- lookup fine
 *     psxcd: lba 827: expected INT1, got INT3        <- first seek after it
 *     EXT2-fs: unable to read superblock
 *
 * Setloc and ReadN both "succeeded" with INT3 because the controller was
 * still answering the Pause, and the INT3 the read loop then saw was that
 * backlog rather than a data-ready INT1.
 */
#define PSXCD_PAUSE_COOLDOWN_US	1300000		/* 1.3 s, > 60 vblanks PAL */

void psxcd_hw_stop (void)
{
   unsigned char res[16];
   int nres, t, i;

   if (!psxcd_streaming)
      return;

   cd_take_irq ();

   /*
    * Drain BEFORE pausing.
    *
    * ReadN runs until it is stopped, and stage 1 is synchronous - so between
    * the last read and this Pause the drive has gone right on delivering
    * sectors, raising an INT1 each time, with nobody to consume them. All of
    * init and the whole first mount attempt happen in that window.
    *
    * If one of those INT1s is still queued, `cd_do(CDC_PAUSE)` returns 1
    * instead of 3, the `t == 3` branch below is skipped, and Pause's own INT2
    * is never drained. It then surfaces as the answer to a later command -
    * which on hardware looked like:
    *
    *     psxcd: ReadN -> INT2
    *
    * a command that appears to fail while actually being handed somebody
    * else's reply.
    */
   while (cd_int ()) {
      cd_result (res, 16);
      cd_ack ();
   }

   t = cd_do (CDC_PAUSE, 0, 0, res, &nres, 400000);
   if (t == 3) {
      cd_wait_int (4000000);
      /* Drain the second response too. Reading past the end of the result
       * FIFO pads with zeroes and then silently repeats [docs/24 §1.3], so
       * bytes left here surface as a wrong answer to the NEXT command. */
      cd_result (res, 16);
      cd_ack ();
   } else if (t > 0) {
      printk (KERN_WARNING DEVICE_NAME
              ": Pause answered INT%d - draining and continuing\n", t);
      cd_result (res, 16);
      cd_ack ();
      /* Whatever Pause's real responses were, they are still to come. */
      if (cd_wait_int (4000000) > 0) {
         cd_result (res, 16);
         cd_ack ();
      }
   }
   cd_release_irq ();

   psxcd_streaming = 0;

   /* udelay's argument is bounded, so spend it in slices. */
   for (i = 0; i < PSXCD_PAUSE_COOLDOWN_US / 1000; i++)
      udelay (1000);

   /* Anything the controller raised while it settled is stale by definition. */
   cd_take_irq ();
   while (cd_int ()) {
      cd_result (res, 16);
      cd_ack ();
   }
   cd_release_irq ();
}

static void cd_lba_to_msf (unsigned long lba, unsigned char *msf)
{
   unsigned long t = lba + 150;		/* two-second lead-in */
   unsigned long m = t / (75 * 60);
   unsigned long s = (t / 75) % 60;
   unsigned long f = t % 75;

   msf[0] = (unsigned char)(((m / 10) << 4) | (m % 10));
   msf[1] = (unsigned char)(((s / 10) << 4) | (s % 10));
   msf[2] = (unsigned char)(((f / 10) << 4) | (f % 10));
}

/*
 * Read one 2048-byte sector into buf.
 *
 * Streaming is kept running between calls, so a sequential read costs one
 * INT1 and one DMA. A seek costs a Pause and a fresh Setloc/ReadN.
 *
 * Always Setloc before ReadN. A bare ReadN after a Pause "resumes at the most
 * recently received sector" [docs/24 §2.3] - it hands back a sector you already
 * had, which in a block device is silent data corruption.
 */
int psxcd_hw_read (unsigned long lba, void *buf)
{
   unsigned char msf[3], res[16];
   int nres, t, i;

   cd_take_irq ();

   if (!psxcd_streaming || lba != psxcd_next_lba) {
      if (psxcd_streaming) {
         cd_release_irq ();
         psxcd_hw_stop ();
         cd_take_irq ();
      }

      cd_lba_to_msf (lba, msf);

      t = cd_do (CDC_SETLOC, msf, 3, res, &nres, 400000);
      if (t != 3) {
         printk (KERN_ERR DEVICE_NAME ": Setloc lba %lu -> INT%d\n", lba, t);
         cd_release_irq ();
         return 0;
      }

      t = cd_do (CDC_READN, 0, 0, res, &nres, 400000);
      if (t != 3) {
         printk (KERN_ERR DEVICE_NAME ": ReadN -> INT%d\n", t);
         cd_release_irq ();
         return 0;
      }

      psxcd_streaming = 1;
   }

   /* The seek happens inside ReadN, so the first sector of a run can be far
    * later than the steady-state 6.6 ms. */
   t = cd_wait_int (8000000);
   if (t != 1) {
      printk (KERN_ERR DEVICE_NAME ": lba %lu: expected INT1, got INT%d\n",
              lba, t);
      psxcd_streaming = 0;
      cd_release_irq ();
      return 0;
   }

   cd_result (res, 16);

   /* Request the data before acknowledging, and keep the BIOS's dummy
    * accesses - both the BIOS and PSn00bSDK do this on real hardware and
    * nobody knows whether they are load-bearing. [docs/24 §4.6] */
   CD0 = 0; (void)CD0;
   CD3 = 0; (void)CD3;
   CD0 = 0;
   CD3 = 0x80;

   for (i = 0; i < 200000; i++)
      if (CD0 & HSTS_DRQSTS)
         break;

   if (!(CD0 & HSTS_DRQSTS)) {
      printk (KERN_ERR DEVICE_NAME ": lba %lu: no data\n", lba);
      cd_ack ();
      psxcd_streaming = 0;
      cd_release_irq ();
      return 0;
   }

   /* No translation and no invalidate: see the header comment. */
   D3_MADR = ((unsigned long)buf) & 0x00ffffff;
   D3_BCR = 0x00010200;			/* 1 block x 512 words */
   D3_CHCR = CHCR_CD_READ;

   for (i = 0; i < 2000000; i++)
      if (!(D3_CHCR & CHCR_BUSY))
         break;

   if (D3_CHCR & CHCR_BUSY) {
      printk (KERN_ERR DEVICE_NAME ": lba %lu: DMA stalled\n", lba);
      cd_ack ();
      psxcd_streaming = 0;
      cd_release_irq ();
      return 0;
   }

   cd_ack ();
   cd_release_irq ();

   psxcd_next_lba = lba + 1;

   return 1;
}

/* ------------------------------------------------------------------ */
/* Block device                                                        */
/* ------------------------------------------------------------------ */

/*
 * Where the ext2 image starts on the disc.
 *
 * The image is a plain file inside the ISO9660 filesystem and this tree has no
 * isofs, so the driver finds it itself: read the volume descriptor, walk the
 * root directory, take the extent. docs/24 §5.5 option B.
 *
 * Option A - being told the LBA on the command line - is still here and still
 * wins if given, because it is the escape hatch when a disc is laid out in a
 * way the lookup does not expect. But it cannot be the normal path: the LBA
 * changes every time the disc is rebuilt, so baking it into the kernel means a
 * kernel rebuild per disc rebuild, and disc contents are about to be iterated
 * on constantly.
 */
static unsigned long psxcd_base_lba;
static unsigned long psxcd_len_kb = 16 * 1024;	/* until told otherwise */
static int psxcd_base_forced;			/* psxcd_base= was given */

/* The image's name in the ISO9660 root directory. Uppercase, 8.3, and the
 * ";1" version suffix is matched separately so it need not be typed. */
static char psxcd_file_name[32] = "ROOT.IMG";

static int __init psxcd_base_setup (char *s)
{
   psxcd_base_lba = simple_strtoul (s, NULL, 0);
   psxcd_base_forced = 1;
   return 1;
}

static int __init psxcd_size_setup (char *s)
{
   psxcd_len_kb = simple_strtoul (s, NULL, 0);
   return 1;
}

/* -1 = decide from ROOT_DEV, 0 = never probe, 1 = always probe */
static int psxcd_probe = -1;

static int __init psxcd_probe_setup (char *s)
{
   psxcd_probe = (int)simple_strtoul (s, NULL, 0);
   return 1;
}

static int __init psxcd_file_setup (char *s)
{
   int i;

   for (i = 0; i < (int)sizeof (psxcd_file_name) - 1 && s[i]; i++)
      psxcd_file_name[i] = s[i];
   psxcd_file_name[i] = '\0';
   return 1;
}

__setup ("psxcd_base=", psxcd_base_setup);
__setup ("psxcd_size=", psxcd_size_setup);
__setup ("psxcd_file=", psxcd_file_setup);
__setup ("psxcd_probe=", psxcd_probe_setup);

/* ------------------------------------------------------------------ */
/* ISO9660 lookup - docs/24 §5.5 option B                              */
/* ------------------------------------------------------------------ */

/*
 * Enough ISO9660 to find one file in the root directory. Not a filesystem:
 * no subdirectories, no Joliet, no Rock Ridge, no multi-extent. The disc is
 * built by us and the image sits in the root, so anything more is code that
 * cannot be tested here.
 *
 * Layout, from ECMA-119:
 *   Volume descriptor (LBA 16): type byte at 0 (1 = primary), "CD001" at 1,
 *   and the root directory RECORD - not a pointer, the record itself -
 *   embedded at offset 156.
 *
 *   Directory record:
 *     0   length of this record; 0 means no more records in this sector
 *     2   extent LBA, both-endian (little-endian half at +2)
 *     10  data length, both-endian (little-endian half at +10)
 *     25  file flags; bit 1 (0x02) marks a directory
 *     32  length of the file identifier
 *     33  the identifier itself, e.g. "ROOT.IMG;1"
 */
#define ISO_PVD_LBA		16
#define ISO_ROOT_RECORD		156
#define ISO_REC_LEN		0
#define ISO_REC_EXTENT		2
#define ISO_REC_SIZE		10
#define ISO_REC_FLAGS		25
#define ISO_REC_NAMELEN		32
#define ISO_REC_NAME		33
#define ISO_FLAG_DIR		0x02

/* 64 sectors = 128 KB of directory records, several thousand files. Far more
 * than this disc will ever hold, and small enough that a malformed length
 * cannot turn the boot into a silent multi-hour seek. */
#define ISO_MAX_ROOT_BYTES	(64 * PSXCD_SECTOR)

/*
 * Both-endian fields sit at odd byte offsets inside a record, so a 32-bit load
 * would be unaligned - and on MIPS that traps rather than fixing itself up.
 * Assemble the little-endian half a byte at a time.
 */
static unsigned long __init iso_le32 (const unsigned char *p)
{
   return ((unsigned long)p[0])
        | ((unsigned long)p[1] << 8)
        | ((unsigned long)p[2] << 16)
        | ((unsigned long)p[3] << 24);
}

/*
 * Compare an ISO9660 identifier against a wanted name.
 *
 * ISO9660 stores "ROOT.IMG;1" - uppercase, with a version suffix. The suffix
 * is optional in what the caller asks for, and the comparison is
 * case-insensitive so a lowercase psxcd_file= still works.
 */
static int __init iso_name_eq (const unsigned char *id, int idlen,
                               const char *want)
{
   int i;

   /* drop the ";1" version suffix if present */
   for (i = 0; i < idlen; i++)
      if (id[i] == ';') {
         idlen = i;
         break;
      }

   for (i = 0; i < idlen; i++) {
      int a = id[i], b = want[i];

      if (!b)
         return 0;
      if (a >= 'a' && a <= 'z') a -= 32;
      if (b >= 'a' && b <= 'z') b -= 32;
      if (a != b)
         return 0;
   }

   return want[idlen] == '\0';
}

/* One sector of scratch. __initdata: this is all over before init memory is
 * freed, and 2 KB is not something this machine can spare permanently. */
static unsigned char psxcd_probe_buf[PSXCD_SECTOR] __initdata
       __attribute__ ((aligned (4)));

/*
 * Find psxcd_file_name in the disc's root directory.
 *
 * Sets psxcd_base_lba and psxcd_len_kb and returns 1 on success. On any
 * failure it returns 0 having changed nothing, so the caller can fall back to
 * whatever the command line said.
 */
static int __init psxcd_iso_lookup (void)
{
   unsigned long root_lba, root_len, sector;
   unsigned char *b = psxcd_probe_buf;

   if (!psxcd_hw_read (ISO_PVD_LBA, b)) {
      printk (KERN_ERR DEVICE_NAME ": cannot read the volume descriptor\n");
      return 0;
   }

   if (b[0] != 1 || b[1] != 'C' || b[2] != 'D' || b[3] != '0' ||
       b[4] != '0' || b[5] != '1') {
      printk (KERN_ERR DEVICE_NAME
              ": no ISO9660 primary volume descriptor at LBA %d\n",
              ISO_PVD_LBA);
      return 0;
   }

   root_lba = iso_le32 (b + ISO_ROOT_RECORD + ISO_REC_EXTENT);
   root_len = iso_le32 (b + ISO_ROOT_RECORD + ISO_REC_SIZE);

   if (!root_lba || !root_len) {
      printk (KERN_ERR DEVICE_NAME ": root directory record is empty\n");
      return 0;
   }

   /*
    * Bound the scan.
    *
    * root_len comes off the disc, so a scratched or malformed one can say
    * 4 GB. Every sector of it would be a synchronous polled read of ~6.6 ms
    * at best, from inside an initcall - the machine would look hung, at boot,
    * with no way to tell why. A root directory of more than this is not a
    * disc this driver is meant to read.
    */
   if (root_len > ISO_MAX_ROOT_BYTES) {
      printk (KERN_ERR DEVICE_NAME
              ": root directory claims %lu bytes - refusing to scan\n",
              root_len);
      return 0;
   }

   /* Walk the root directory a sector at a time. Records never straddle a
    * sector boundary in ISO9660 - a short sector is zero-padded instead - so
    * each sector can be scanned on its own. */
   for (sector = 0; sector * PSXCD_SECTOR < root_len; sector++) {
      int off = 0;

      if (!psxcd_hw_read (root_lba + sector, b)) {
         printk (KERN_ERR DEVICE_NAME ": root directory read failed\n");
         return 0;
      }

      while (off < PSXCD_SECTOR) {
         unsigned char *rec = b + off;
         int reclen = rec[ISO_REC_LEN];
         int namelen;

         if (reclen == 0)
            break;			/* padding to end of sector */
         if (off + reclen > PSXCD_SECTOR)
            break;			/* malformed - do not walk off the end */

         namelen = rec[ISO_REC_NAMELEN];

         if (!(rec[ISO_REC_FLAGS] & ISO_FLAG_DIR) &&
             namelen > 0 && ISO_REC_NAME + namelen <= reclen &&
             iso_name_eq (rec + ISO_REC_NAME, namelen, psxcd_file_name)) {
            unsigned long lba = iso_le32 (rec + ISO_REC_EXTENT);
            unsigned long len = iso_le32 (rec + ISO_REC_SIZE);

            if (!lba || len < PSXCD_SECTOR) {
               printk (KERN_ERR DEVICE_NAME ": %s is empty or too small\n",
                       psxcd_file_name);
               return 0;
            }

            psxcd_base_lba = lba;
            psxcd_len_kb = len >> 10;

            printk (KERN_INFO DEVICE_NAME ": found %s at LBA %lu, %lu KB\n",
                    psxcd_file_name, psxcd_base_lba, psxcd_len_kb);
            return 1;
         }

         off += reclen;
      }
   }

   /*
    * Not found. Say what IS there.
    *
    * "not found" alone cannot distinguish a disc without the file from a
    * directory walk that is reading rubbish - and those need completely
    * different fixes. Listing the names turns the failure into evidence that
    * the PVD read, the extent arithmetic and the record walk all worked.
    * This is __init, so it costs nothing once the machine is up.
    */
   printk (KERN_ERR DEVICE_NAME ": %s not found. The root directory holds:\n",
           psxcd_file_name);

   for (sector = 0; sector * PSXCD_SECTOR < root_len; sector++) {
      int off = 0;

      if (!psxcd_hw_read (root_lba + sector, b))
         break;

      while (off < PSXCD_SECTOR) {
         unsigned char *rec = b + off;
         int reclen = rec[ISO_REC_LEN];
         int namelen, i;
         char nm[40];

         if (reclen == 0 || off + reclen > PSXCD_SECTOR)
            break;

         namelen = rec[ISO_REC_NAMELEN];
         if (namelen > 0 && ISO_REC_NAME + namelen <= reclen) {
            if (namelen > (int)sizeof (nm) - 1)
               namelen = sizeof (nm) - 1;
            for (i = 0; i < namelen; i++) {
               unsigned char c = rec[ISO_REC_NAME + i];

               /* "." and ".." are stored as a single 0x00 / 0x01 byte */
               nm[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            }
            nm[namelen] = '\0';
            printk (KERN_ERR DEVICE_NAME ":   %-16s %s LBA %lu, %lu bytes\n",
                    nm,
                    (rec[ISO_REC_FLAGS] & ISO_FLAG_DIR) ? "dir " : "file",
                    iso_le32 (rec + ISO_REC_EXTENT),
                    iso_le32 (rec + ISO_REC_SIZE));
         }

         off += reclen;
      }
   }

   return 0;
}

static int psxcd_sizes[1];
static int psxcd_blocksizes[1];
static int psxcd_hardsects[1];

/*
 * The block layer always speaks 512-byte sectors regardless of hardsect_size,
 * so shift by 4 to get 2048-byte units.
 */
static void do_psxcd_request (request_queue_t *q)
{
   while (1) {
      unsigned long lba, n;
      char *buf;
      int ok = 1;

      INIT_REQUEST;

      if (CURRENT->cmd != READ) {
         printk (KERN_ERR DEVICE_NAME ": write attempted; read-only\n");
         end_request (0);
         continue;
      }

      if ((CURRENT->sector & 3) || (CURRENT->current_nr_sectors & 3)) {
         printk (KERN_ERR DEVICE_NAME ": unaligned request %lu+%lu\n",
                 CURRENT->sector, CURRENT->current_nr_sectors);
         end_request (0);
         continue;
      }

      lba = psxcd_base_lba + (CURRENT->sector >> 2);
      n = CURRENT->current_nr_sectors >> 2;
      buf = CURRENT->buffer;

      while (n--) {
         if (!psxcd_hw_read (lba, buf)) {
            ok = 0;
            break;
         }
         lba++;
         buf += PSXCD_SECTOR;
      }

      end_request (ok);
   }
}

static int psxcd_open (struct inode *inode, struct file *filp)
{
   if (!psxcd_present)
      return -ENXIO;
   if (filp && (filp->f_mode & 2))
      return -EROFS;		/* read-only, and say so early */

   MOD_INC_USE_COUNT;
   return 0;
}

static int psxcd_release (struct inode *inode, struct file *filp)
{
   MOD_DEC_USE_COUNT;
   return 0;
}

static int psxcd_ioctl (struct inode *inode, struct file *filp,
                        unsigned int cmd, unsigned long arg)
{
   switch (cmd) {
      case BLKGETSIZE:
         return put_user (psxcd_len_kb * 2, (unsigned long *)arg);
      case BLKFLSBUF:
         if (!capable (CAP_SYS_ADMIN))
            return -EACCES;
         fsync_dev (inode->i_rdev);
         invalidate_buffers (inode->i_rdev);
         return 0;
      default:
         return -EINVAL;
   }
}

static struct block_device_operations psxcd_fops = {
   open:	psxcd_open,
   release:	psxcd_release,
   ioctl:	psxcd_ioctl,
};

int __init psxcd_init (void)
{
   if (register_blkdev (MAJOR_NR, DEVICE_NAME, &psxcd_fops)) {
      printk (KERN_ERR DEVICE_NAME ": cannot get major %d\n", MAJOR_NR);
      return -EBUSY;
   }

   blk_init_queue (BLK_DEFAULT_QUEUE (MAJOR_NR), DEVICE_REQUEST);

   /*
    * 2048 is the drive's native unit with Setmode bit 5 clear, and setting it
    * means the block layer only ever issues 2048-aligned requests - no
    * read-modify-write, no partial sectors, no bounce buffer.
    *
    * It also forces the image: ext2 refuses a filesystem whose blocksize is
    * below the hardware sector size, so the disc image must be built with
    * mke2fs -b 2048. [docs/24 §5.3]
    */
   psxcd_hardsects[0] = PSXCD_SECTOR;
   psxcd_blocksizes[0] = PSXCD_SECTOR;
   psxcd_sizes[0] = psxcd_len_kb;

   hardsect_size[MAJOR_NR] = psxcd_hardsects;
   blksize_size[MAJOR_NR] = psxcd_blocksizes;
   blk_size[MAJOR_NR] = psxcd_sizes;
   read_ahead[MAJOR_NR] = 16;

   if (!psxcd_hw_init ()) {
      printk (KERN_INFO DEVICE_NAME ": no drive responding\n");
      /* Leave the device registered: a disc may be inserted later, and
       * failing the initcall would take the whole driver out. */
      return 0;
   }

   /*
    * Find the image, unless the command line already said where it is.
    *
    * A failed lookup is not fatal: psxcd_base_lba keeps whatever it had, the
    * device stays registered, and the disc can still be read raw through
    * BRMON. Panicking here would take out a machine that boots perfectly well
    * from the ramdisk.
    */
   /*
    * Only walk the disc if this machine is actually about to boot from it.
    *
    * The lookup is a sequence of synchronous polled CD reads run from inside
    * an initcall. Every loop in it is bounded, but "bounded" is a claim about
    * code, and the cost of that claim being wrong is a console that hangs
    * before it reaches userspace - on EVERY boot from this disc, including
    * the ramdisk one that has nothing to do with the CD. So: no root=/dev/psxcd,
    * no probe. `psxcd_probe=1` forces it for debugging, `psxcd_probe=0`
    * disables it outright.
    */
   if (psxcd_base_forced)
      printk (KERN_INFO DEVICE_NAME
              ": using psxcd_base=%lu from the command line\n",
              psxcd_base_lba);
   else if (psxcd_probe == 0 ||
            (psxcd_probe < 0 && MAJOR (ROOT_DEV) != MAJOR_NR))
      printk (KERN_INFO DEVICE_NAME
              ": not the root device - skipping the ISO9660 probe\n");
   else if (!psxcd_iso_lookup ())
      printk (KERN_WARNING DEVICE_NAME
              ": falling back to LBA %lu - root=/dev/psxcd will not mount\n",
              psxcd_base_lba);

   psxcd_sizes[0] = psxcd_len_kb;	/* the lookup may have changed it */

   printk (KERN_INFO DEVICE_NAME
           ": PlayStation CD-ROM, image at LBA %lu, %lu KB, read-only\n",
           psxcd_base_lba, psxcd_len_kb);

   return 0;
}

__initcall (psxcd_init);
