# A Linux 2.4 block driver for the PlayStation 1 CD-ROM — research reference

> Written 2026-08-21. Research only: **no code in this repository was modified
> to produce this document.**
>
> Target: PAL SCPH-750x, BIOS `System ROM Version 4.1 12/16/97 E`, 2 MB RAM,
> R3000A, no MMU. Kernel tree at `blackroo/` has **no** `drivers/cdrom/` and
> **no** `fs/isofs` (verified this session by directory listing).
>
> Constraint that shapes everything below: **the driver may not call the BIOS.**
> `blackroo/arch/mipsnommu/ps/prom/memory.c:prom_free_prom_memory()` hands the
> low memory below `_ftext` to the page allocator, so BIOS scratch state is gone
> by the time any driver runs. Every access here is a direct register access.

## How to read this document

Each factual claim carries its source. Sources are one of:

- **[SPX]** psx-spx, fetched as the repository's own markdown (not the rendered
  site) — see §9 for exact URLs. This is the primary hardware reference.
- **[PSN]** PSn00bSDK `libpsn00b/psxcd` source, fetched from GitHub master.
  Known-working code on this exact hardware class.
- **[TREE]** a file in this repository, read this session.
- **[MEAS]** measured this session from a file in `output/`.
- **[CROSS]** cross-implementation survey: DuckStation, PCSX-Redux (emulator and
  its bare-metal `src/mips/` code), PSXSDK, psyqo, and — most importantly —
  **real-hardware logs from JaCzekanski's `ps1-tests`**. Full URLs in §9.

Anything not established by one of those is marked **VERIFY** and must be
tested before it is relied on. This project has been burned repeatedly by
plausible-but-wrong assumptions (see `GUARDRAILS.md`), so the marks are not
decoration.

---

## Bring-up result — 2026-08-25

**The command path and PIO reads are proven on hardware.** A polled probe in
BRMON (`cd` in `arch/mipsnommu/ps/brmon.c`) reads real sectors first time:
LBA 16 returns this project's own ISO9660 volume descriptor. Capture:
`docs/captures/2026-08-25-cdrom-first-sector.txt`.

Resolved against the VERIFY items below:

| Question | Answer |
|---|---|
| Does the documented init sequence work? | Yes, verbatim — including `Init`'s two-stage INT3→INT2 |
| LBA→MSF BCD with the 150-frame lead-in? | Correct; LBA 16 → MSF `00:02:16`, returning sector 16 |
| 8-bit PIO reads of `RDDATA`? | Work |
| BFRD order (request data, *then* ack)? | Works, dummy BIOS accesses kept |
| `COM_DELAY` — `1325h` or `132Ch`? | Found in place: **`1325h`**. We write `132Ch`; both serviceable at 2x/PIO |
| Drive firmware | `98 06 10 c3` = 1998-06-10 rev C3 |

**DMA is proven too** (same day): `cd cmp` reads a sector by PIO and by DMA and
compares all 2048 bytes - identical, at two different LBAs - and `cd dma` reads
arbitrary sectors. `MADR = addr & 0xffffff`, `BCR = 0x00010200`,
`CHCR = 0x11000000`, poll bit 24.

| Question | Answer |
|---|---|
| DMA3 burst read (§4.3)? | Works; byte-identical to PIO |
| Does DMA into a cached buffer need an invalidate (§1.7)? | **No.** Cached and uncached views of the buffer agree byte for byte |
| Address translation for `MADR`? | **None needed** — see below |

**Correction to §1.7's framing.** That section discusses KSEG0 vs KSEG1, but
**this kernel is linked in KUSEG**: its symbols are plain physical addresses
(`0015758c`), not `8015758c`. `mem` in the monitor shows the same — "kernel
text starts 000900". Two consequences:

- The uncached alias of a kernel buffer is `(addr & 0x1fffffff) | 0xa0000000`.
  The obvious `addr | 0x20000000` yields a KUSEG address hundreds of megabytes
  into a 2 MB machine and **hangs the console outright**, with no output.
- `MADR` wants a physical address and a kernel buffer's address already *is*
  one, so there is nothing to translate. §4.5's "no bounce buffer needed"
  conclusion holds, for this reason rather than the one it gives.

Still open: 16-bit reads, and everything in §5 (the block driver itself).

---

## 0. The decisions, up front

Everything below argues for these. If you read nothing else:

| Decision | Value |
|---|---|
| Register access | 8-bit only, through **KSEG1** `0xBF801800`-`0xBF801803`. Never 32-bit. |
| Read mode | `Setmode 0x80` — 2x speed, **2048-byte** data-only sectors, bit 4 clear |
| Transfer | **DMA channel 3**, `CHCR = 0x11000000`, `BCR = 0x00010200`, straight into `CURRENT->buffer` |
| IRQ | Linux irq **10** (`CDROM`), hardware `I_STAT` bit 2. `do_IRQ` acks `I_STAT`; the handler acks the device with `HCLRCTL = 0x1F` |
| Block layer | `request_fn` via `blk_init_queue`, major **209**, one minor, read-only |
| `hardsect_size` | **2048** — which forces the disc image to be `mke2fs -b 2048` |
| Image location | fixed LBA passed on the kernel command line (`psxcd_base=`) |
| Streaming | keep `ReadN` running; `Pause` only on a discontinuity — it costs ~30 ms plus a ~1 s controller cooldown |
| Prior art | none exists in any Linux/BSD PS1 port; port from PSn00bSDK or PCSX-Redux bare-metal |

---

## 1. Hardware register reference

### 1.1 The four ports and the bank mechanism

The CPU talks to the CD-ROM **decoder** chip (a CXD1199 variant), not to the
drive itself. The decoder provides mailboxes to the drive's HC05
microcontroller. Four 8-bit ports at `0x1F801800`-`0x1F801803`, bank-switched
into four banks of four registers. [SPX cdromdrive.md]

Bank ("index") is selected by writing the low 2 bits of `0x1F801800`, and read
back from the low 2 bits of the same address.

**On read:**

| Bank | `1F801800` | `1F801801` | `1F801802` | `1F801803` |
|---|---|---|---|---|
| 0, 2 | `HSTS` | `RESULT` | `RDDATA` | `HINTMSK` |
| 1, 3 | `HSTS` | `RESULT` | `RDDATA` | `HINTSTS` |

**On write:**

| Bank | `1F801800` | `1F801801` | `1F801802` | `1F801803` |
|---|---|---|---|---|
| 0 | `ADDRESS` | `COMMAND` | `PARAMETER` | `HCHPCTL` |
| 1 | `ADDRESS` | `WRDATA` | `HINTMSK` | `HCLRCTL` |
| 2 | `ADDRESS` | `CI` | `ATV0` (L→L) | `ATV1` (L→R) |
| 3 | `ADDRESS` | `ATV2` (R→R) | `ATV3` (R→L) | `ADPCTL` |

[SPX cdromdrive.md, "CDROM Controller I/O Ports"]

Note that `HSTS`, `RESULT` and `RDDATA` are readable from **every** bank — only
`1F801803` on read, and `1F801801`/`1F801802`/`1F801803` on write, actually
depend on the bank. A driver therefore only needs to set the bank before
touching `1F801803` and before writing commands/parameters.

### 1.2 `1F801800` read — `HSTS` (status)

```
  0-1 RA       Current register bank (R/W)
  2   ADPBUSY  ADPCM busy            (1 = playing XA-ADPCM)
  3   PRMEMPT  Parameter empty       (1 = parameter FIFO empty)
  4   PRMWRDY  Parameter write ready (1 = parameter FIFO not full)
  5   RSLRRDY  Result read ready     (1 = result FIFO not empty)
  6   DRQSTS   Data request          (1 = RDDATA reads / WRDATA writes pending)
  7   BUSYSTS  Busy status           (1 = HC05 busy acknowledging command)
```

[SPX cdromdrive.md]

`RSLRRDY` (bit 5) is the loop condition for draining the response FIFO;
`DRQSTS` (bit 6) says the data FIFO has a sector in it; `BUSYSTS` (bit 7) must
be **0** before a new command byte is written.

### 1.3 Command and parameter FIFOs

- `1F801802` write, bank 0 = `PARAMETER`. **16 bytes deep.** `PRMWRDY` clears
  when full. psx-spx explicitly flags that the CXD1199 datasheet's claim of 8
  bytes is wrong, because the longest command takes 13 parameter bytes.
  [SPX cdromdrive.md]
- `1F801801` write, bank 0 = `COMMAND`. Writing the command byte makes the HC05
  drain the parameter FIFO, process the command, push return values into the
  result FIFO, and fire INT3 (or INT5 on error). [SPX]
- `1F801801` read (any bank) = `RESULT`. **16 bytes.** `RSLRRDY` clears after
  the last byte of the response is read. Reading past the end pads with `00h`
  to 16 bytes and then **wraps to the first response byte** — it never signals
  "empty" by returning garbage, it silently repeats. [SPX]

  Practical consequence: never read the result FIFO a fixed number of times.
  Loop on `RSLRRDY`, with a hard cap. PSn00bSDK caps at `MAX_RESULT_SIZE` = 32.
  [PSN common.c]

### 1.4 `1F801803` — interrupt registers

Read, banks 1 and 3 — `HINTSTS`:

```
  0-2 INTSTS  interrupt "type" from the HC05 (see below)
  3   BFEMPT  sound-map XA buffer empty
  4   BFWRDY  sound-map XA buffer write ready
  5-7 -       reserved (always read as 1)
```

Bits 0-2 are **not** three independent flags. The HC05 uses them as a single
3-bit *type* value: [SPX]

```
  INT0  NoIntr       no interrupt pending
  INT1  DataReady    new sector available (ReadN/ReadS), or Play report packet
  INT2  Complete     command finished (second response, only some commands)
  INT3  Acknowledge  command received and acknowledged (all commands)
  INT4  DataEnd      end of disc, or end of track with auto-pause
  INT5  DiskError    command error, read error, licence error, or lid opened
```

Read banks 0 and 2 / write bank 1 at `1F801802` — `HINTMSK`:

```
  0-2 ENINT     enable IRQ on the respective INTSTS bits
  3   ENBFEMPT  enable IRQ on BFEMPT
  4   ENBFWRDY  enable IRQ on BFWRDY
  5-7 -         reserved (write 0, read 1)
```

"The CD-ROM drive fires an interrupt whenever `(HINTMSK & HINTSTS)` is
non-zero. This register is typically set to `1Fh`." [SPX]

Write bank 1 at `1F801803` — `HCLRCTL` (acknowledge):

```
  0-2 CLRINT     acknowledge HC05 interrupt flags (1 = clear)
  3   CLRBFEMPT  acknowledge BFEMPT
  4   CLRBFWRDY  acknowledge BFWRDY
  5   SMADPCLR   clear sound-map XA buffer
  6   CLRPRM     clear parameter FIFO
  7   CHPRST     reset decoder chip
```

"Normally one should write `07h` to reset the HC05 interrupt flags, or `1Fh` to
acknowledge all IRQs. **After acknowledge, the result FIFO is drained and if
there's been a pending command, then that command gets sent to the
controller.**" [SPX]

That last sentence is load-bearing: **acknowledging destroys the result FIFO.**
Read the response *before* the ack, or accept that it is gone. PSn00bSDK does
the opposite — it acks first, then reads `CD_REG(1)` — and inserts a 50-iteration
empty-loop delay between the two, with the "correct" drain loop commented out:

```c
    CD_REG(0) = 1;
    CD_REG(3) = 0x1f; // Acknowledge all IRQs
    CD_REG(3) = 0x40; // Reset parameter buffer

    //while (CD_REG(0) & (1 << 5))
        //CD_REG(1);
    for (int i = 0; i < 50; i++)
        __asm__ volatile("");
    ...
    uint8_t first_byte = CD_REG(1);
```
[PSN common.c, `_cd_irq_handler`]

**VERIFY:** this is a direct conflict between the reference and known-working
code. PSn00bSDK evidently gets valid response bytes after a `1Fh` ack plus a
short delay, which suggests the drain is not instantaneous. The safe design for
a first driver is **read the response first, then ack** — do not copy the
PSn00bSDK ordering without testing it, and do not assume psx-spx's phrasing
means the FIFO is empty the cycle after the write.

Write bank 0 at `1F801803` — `HCHPCTL` (data request):

```
  0-4 -     reserved (0)
  5   SMEN  sound map enable
  6   BFWR  request sector buffer write (prepare WRDATA)
  7   BFRD  request sector buffer read  (prepare RDDATA)   <- the one we need
```
[SPX]

### 1.5 `1F801802` read — `RDDATA` (data FIFO)

"After ReadS/ReadN commands have generated INT1, software must set the BFRD
flag, then wait until DRQSTS is set, [then] the datablock (disk sector) can be
read from this register." [SPX]

Sizes and padding:

> "The PSX hardware allows to read 800h-byte or 924h-byte sectors, indexed as
> [000h..7FFh] or [000h..923h]; when trying to read further bytes, then the PSX
> will repeat the byte at index [800h-8] or [924h-4] as padding value."
> [SPX]

Access widths:

> "RDDATA can be accessed with 8bit or 16bit reads (ie. to read a 2048-byte
> sector, one can use 2048 load-byte opcodes, or 1024 load halfword opcodes,
> or, more conventionally, a 512 word DMA transfer; the actual CDROM databus is
> only 8bits wide, so the CPU's bus interface handles splitting the reads)."
> [SPX]

Note what that does **not** say: it does not endorse 32-bit CPU loads from
`RDDATA`. 8-bit and 16-bit are named; the 32-bit case is described only for
DMA. See §5.3 — there is a related bus-config trap.

### 1.6 The bus configuration register — `1F801018`

`1F801018` is the CD-ROM Delay/Size register, "usually `00020843h` or
`00020943h`". [SPX memorycontrol.md]

```
  0-3   Write Delay        (00h..0Fh = 1..16 cycles)
  4-7   Read Delay         (00h..0Fh = 1..16 cycles)
  8     Recovery Period    (uses COM0 timings)
  9     Hold Period        (uses COM1 timings)
  10    Floating Period    (uses COM2 timings)
  11    Pre-strobe Period  (uses COM3 timings)
  12    Data Bus-width     (0=8bit, 1=16bit)
  13    Auto Increment     (0=No, 1=Yes)
  16-20 Number of address bits (window = 1 << N bytes)
```

PSn00bSDK writes `BUS_CD_CFG = 0x00020943` in `CdInit()`. [PSN common.c]
Decoded against the table above: write delay 3 (4 cycles), read delay 4
(5 cycles), recovery on, pre-strobe on, 8-bit bus, **auto-increment off**,
window `1<<2` = 4 bytes. The BIOS value observed in the psx-spx ReadN trace is
the same `00020943h`, alongside `COM_DELAY = 0000132Ch` at `1F801020`. [SPX
cdromdrive.md, ReadN section]

The critical bit is bit 13:

> "Performing a 32-bit read from 1F801800h will return the HSTS register's value
> repeated four times, as the 'auto increment' flag in the BIU configuration
> register for the CD-ROM (at 1F801018h) is disabled by default. Enabling it will
> restore the correct behavior but **will also break CD-ROM DMA reads**, which
> rely on the bus interface splitting each 32-bit word transfer into four
> sequential byte reads from RDDATA." [SPX cdromdrive.md, "Misc"]

So: leave auto-increment off, and **never do a 32-bit load from any CD-ROM
port** — you get one byte smeared four times, not four bytes.

### 1.7 Where these addresses live in this kernel tree

`blackroo/include/asm-mipsnommu/ps/hwregs.h` already defines the offsets:
`CDREG0_PORT`..`CDREG3_PORT` = `0x1800`..`0x1803`, `DMA_MADR3_PORT` = `0x10b0`,
`DMA_BCR3_PORT` = `0x10b4`, `DMA_CHCR3_PORT` = `0x10b8`, `DMA_DPCR_PORT` =
`0x10f0`, `DMA_DICR_PORT` = `0x10f4`. [TREE]

**Trap in that header:** it also defines `MDEC_CONTROL_PORT 0x10f0` and
`MDEC_STATUS_PORT 0x10f4`, which collide with DPCR/DICR. The MDEC is really at
`0x1820`/`0x1824`. Do not use the MDEC defines. [TREE, cross-checked against
SPX iomap.md]

**Addressing mode — resolve this before writing a line of driver code.** The
tree is internally inconsistent:

- `arch/mipsnommu/ps/setup.c:57` sets `mips_io_port_base = PSX_HW_REG_BASE =
  0x1f800000`, so `inb(0x1800)` dereferences virtual `0x1f801800` — a **KUSEG**
  address. [TREE]
- `arch/mipsnommu/kernel/head.S` and `arch/mipsnommu/ps/brmon.c` instead use
  literal `0xbf80xxxx` — **KSEG1**, uncached — with head.S carrying the comment
  "Accessed through KSEG1 (0xbf80...) so nothing is cached." [TREE]
- PSn00bSDK uses `IOBASE 0xbf800000` throughout. [PSN hwregs_c.h]

**VERIFY:** the KUSEG path apparently works today (the existing `bu.c` memory
card driver uses `inb`/`outb` and does talk to hardware), which is consistent
with the PS1's R3000A having **no data cache at all** — its 1 KB "D-cache" is
wired as the scratchpad at `0x1F800000`, so data loads/stores are never cached
regardless of segment. But this is exactly the kind of assumption that has cost
this project days before. **Use explicit KSEG1 pointers (`0xbf801800`…) in the
new driver**, matching `head.S`, `brmon.c` and PSn00bSDK. It costs nothing and
removes the question.

Second-order consequence of the same fact: if the PS1 truly has no writeback
data cache, **DMA buffers need no cache flush/invalidate**. That is a large
simplification versus generic MIPS DMA. Mark it **VERIFY** and prove it with a
one-sector DMA read into a KSEG0 buffer read back through KSEG0.

---

## 2. The command protocol

### 2.1 Command mechanics

1. Wait for `BUSYSTS` (`HSTS` bit 7) = 0.
2. Wait for `HINTSTS & 7` = 0 — **no interrupt pending**.
3. Select bank 0.
4. Write parameter bytes to `1F801802`.
5. Write the command byte to `1F801801`.

Step 2 is not optional. psx-spx is emphatic in three separate places:

> "If there are any pending cdrom interrupts from a previous command, for
> example, an INT3/Acknowledge, these should be cleared before sending a new
> command. If a new command is sent early anyway, the behavior becomes
> unpredictable. For one, BUSYSTS will stay set from the last command. In
> addition to this, the new command will simplily sit in the command register
> unhandled and can be easily overwritten by new commands that are sent."
> [SPX, COMMAND register]

> "Trying to send a new command in the Busy-phase causes malfunction (the older
> command seems to get lost, the newer command executes and returns its results
> and triggers an interrupt, but, thereafter, the controller seems to hang). So,
> always at least wait until BUSYSTS goes off before sending a command."
> [SPX, BUSYSTS flag]

> The HC05 mainloop executes a command only when "Main CPU has sent a command,
> AND, there is no INT pending". [SPX, First Response]

PSn00bSDK's `CdCommandF()` implements exactly steps 1/3/4/5, waiting on
`CD_REG(0) & 0x80` (BUSYSTS) twice and resetting the parameter FIFO in between,
but it does **not** wait on `HINTSTS`:

```c
    while (CD_REG(0) & 0x80) ;          /* BUSYSTS */
    CD_REG(0) = 1;
    CD_REG(3) = 0x40;                   /* CLRPRM: reset parameter FIFO */
    for (int i = 0; i < 50; i++) ;      /* short settling delay */
    while (CD_REG(0) & (1 << 7)) ;      /* BUSYSTS again */
    CD_REG(0) = 0;
    for (; length > 0; length--) CD_REG(2) = *(_param++);
    CD_REG(0) = 0;
    CD_REG(1) = (uint8_t) cmd;
```
[PSN common.c, `CdCommandF`]

It gets away with it because its own IRQ handler acks before any new command is
issued. A Linux driver with a request queue must be explicit about it.

### 2.2 Commands the driver needs

| Op | Name | Params | Responses |
|---|---|---|---|
| `01h` | `Nop` / Getstat | — | INT3(stat). Also resets the shell-open flag |
| `02h` | `Setloc` | amm, ass, asect — **BCD** | INT3(stat) |
| `06h` | `ReadN` | — | INT3(stat), then INT1(stat)+data repeatedly, forever, until Pause |
| `1Bh` | `ReadS` | — | as ReadN, no retry |
| `08h` | `Stop` | — | INT3(stat) → INT2(stat) |
| `09h` | `Pause` | — | INT3(stat) → INT2(stat) |
| `0Ah` | `Init` | — | INT3(stat) (late) → INT2(stat). Sets mode=`20h`, motor on, aborts all commands |
| `0Ch` | `Demute` | — | INT3(stat) |
| `0Eh` | `Setmode` | mode | INT3(stat) |
| `13h` | `GetTN` | — | INT3(stat, first, last) — BCD |
| `14h` | `GetTD` | track (BCD) | INT3(stat, mm, ss) — BCD, **no frame byte** |
| `15h` | `SeekL` | — | INT3(stat) → INT2(stat). Data-mode seek |
| `10h` | `GetlocL` | — | INT3(amm,ass,asect,mode,file,chan,sm,ci) — **no stat byte** |
| `19h` | `Test` | sub | INT3(...) — `19h,20h` gives drive firmware date/version |

[SPX command summary; cross-checked against PSN psxcd.h `CdlCommand` enum, which
agrees on every opcode]

`Setmode` bits [SPX]:

```
  7  Speed        0 = 1x, 1 = 2x
  6  XA-ADPCM     send XA sectors to the SPU
  5  Sector Size  0 = 800h (2048, data only), 1 = 924h (2340, whole sector minus sync)
  4  Ignore       ignores sector size AND the exact Setloc position — do not use
  3  XA-Filter    only process XA sectors matching Setfilter
  2  Report       report interrupts during audio Play
  1  AutoPause    auto-pause at end of track
  0  CDDA         allow reading CD-DA sectors, ignore missing EDC
```

For this driver: **`Setmode 0x80`** — double speed, 2048-byte data-only sectors,
everything else off. Bit 4 in particular must stay clear: it makes the drive
return "data from the Setloc position minus 0..3 sectors" [SPX], which is a
silent data-corruption bug in a block device.

`Setloc` encoding [SPX + PSN misc.c]:

```c
    lba += 150;                          /* 2-second lead-in */
    amm   = itob(lba / (75 * 60));
    ass   = itob((lba / 75) % 60);
    asect = itob(lba % 75);
```

with `itob(i) = ((i/10*16) | (i%10))`. psx-spx adds a validation rule: "All
three parameters must be valid packed BCD, with `ass < 60h` and `asect < 75h`;
invalid or out-of-range values return INT5(stat,10h)."

**Confirmed against a real image [MEAS]:** `output/blackroo.bin` sector 16 has
header bytes `00 02 16 02` — MM=`00h`, SS=`02h`, SECT=`16h` BCD, mode 2. Decoding
back: `(0*60 + 2) * 75 + 16 - 150 = 16`. The +150 and the BCD are both correct.

### 2.3 The `Setloc, Read, Pause` contract

> "Setloc is memorizing the wanted target, and marks it as unprocessed, and has
> no other effect (it doesn't start reading or seeking, and doesn't interrupt or
> redirect any active reads). If Read is issued with an unprocessed Setloc, then
> the drive is automatically seeking the Setloc location (and marks Setloc as
> processed). If Read is issued without an unprocessed Setloc: If reading is
> already in progress then it just continues reading. **If Reading was Paused,
> then reading resumes at the most recently received sector (ie. returning that
> sector once another time).**" [SPX]

That last clause is a landmine for a block driver: a bare `ReadN` after a
`Pause` re-delivers a sector you already had. **Always issue a fresh `Setloc`
before every `ReadN`.**

### 2.4 Worked example: read `n` sectors starting at LBA `L`

All addresses KSEG1. `CD0`..`CD3` = `0xbf801800`..`0xbf801803`.

**Phase 0 — one-time init** (driver probe), per [SPX "To init the CD"] and
[PSN `CdInit`]:

```
    I_MASK  |= 0x0004                    ; enable CD IRQ (bit 2)
    [1F801018h] = 0x00020943             ; CD bus timing, auto-increment OFF
    [1F801020h] = 0x0000132C             ; COM_DELAY   (see note below)
    DPCR    |= 0x00008000                ; DMA3 master enable
    D3_CHCR  = 0                         ; make sure DMA3 is stopped

    CD0 = 0                              ; bank 0
    CD3 = 0x00                           ; HCHPCTL = 0 (clear any data request)
    CD0 = 1                              ; bank 1
    CD3 = 0x1F                           ; HCLRCTL: ack every IRQ flag
    CD2 = 0x1F                           ; HINTMSK: enable every IRQ source

    Nop ; wait INT3 ; ack
    Nop ; wait INT3 ; ack                ; psx-spx says send two Nops
    Init (0Ah) ; wait INT3 ; ack ; wait INT2 ; ack
    Demute (0Ch) ; wait INT3 ; ack
    Setmode 0x80 ; wait INT3 ; ack
```

`Init` sets `mode = 20h` [SPX], i.e. 1x speed and 924h sectors — so `Setmode`
must come *after* `Init`, never before.

**COM_DELAY value:** psx-spx's "To init the CD" recipe says `1325h`; the BIOS
trace on the same page says `132Ch`; PCSX-Redux's bare-metal code writes `132Ch`
before every CD DMA. [SPX; CROSS §8.8] Use **`132Ch`** — it is what both the
BIOS and hardware-tested bare-metal code actually write. PSn00bSDK does not set
it at all, relying on the BIOS default; **we cannot**, because
`prom_free_prom_memory()` has already destroyed BIOS state, so this must be set
explicitly.

**Phase 1 — position and start reading:**

```
    msf = lba_to_msf_bcd(L)              ; see 2.2

    wait BUSYSTS == 0 and (HINTSTS & 7) == 0
    CD0 = 0
    CD2 = msf.mm ; CD2 = msf.ss ; CD2 = msf.ff
    CD1 = 0x02                           ; Setloc
      -> IRQ, HINTSTS&7 == 3 (INT3)
         read stat from CD1
         I_STAT = ~0x0004                ; ack the MIPS IRQ first
         CD0 = 1 ; CD3 = 0x1F            ; then ack the CD controller

    wait BUSYSTS == 0 and (HINTSTS & 7) == 0
    CD0 = 0
    CD1 = 0x06                           ; ReadN
      -> IRQ, INT3, stat.  ack as above.
```

**Phase 2 — per sector, `n` times:**

```
      -> IRQ, HINTSTS&7 == 1 (INT1)
         read stat from CD1                ; drain result FIFO while RSLRRDY
         CD0 = 0
         CD3 = 0x80                        ; HCHPCTL: BFRD = 1, request data
         wait until (HSTS & 0x40)          ; DRQSTS
         D3_MADR = phys(buffer)
         D3_BCR  = 0x00010200              ; 1 block, 512 words = 2048 bytes
         D3_CHCR = 0x11000000              ; start, burst, device->RAM
         wait until (D3_CHCR & (1<<24)) == 0
         I_STAT = ~0x0004
         CD0 = 1 ; CD3 = 0x1F              ; ack INT1
         buffer += 2048
```

The seek happens implicitly inside `ReadN`; the first INT1 arrives after the
seek completes, so it can be far later than the steady-state 6.6 ms. Size the
first-sector timeout generously.

**Phase 3 — stop:**

```
    wait BUSYSTS == 0 and (HINTSTS & 7) == 0
    CD0 = 0
    CD1 = 0x09                           ; Pause
      -> INT3 (stat still has bit5 set), ack
      -> INT2 (stat with bit5 cleared),  ack
```

`ReadN` never stops on its own: "it's repeatedly sending stat,INT1 --> datablock,
that is continued even after a successful read has occured; use the Pause
command to terminate the repeated INT1 responses." [SPX]

### 2.5 Status byte and error bytes

```
  7 Play       6 Seek        5 Read        4 ShellOpen
  3 IdError    2 SeekError   1 MotorOn     0 Error
```

Only one of Play/Seek/Read is set at a time — during a seek, *only* bit 6 is
set, and bit 5 does not appear until the seek completes. [SPX] So "wait for read
to start" is `stat & 0x20`, not `!(stat & 0x40)`.

If bit 0 or bit 2 is set, INT5 is delivered instead of the normal response, and
a second response byte carries the error: [SPX]

```
  first response (stat.0 set):
    10h invalid parameter / invalid sub-function
    20h wrong number of parameters
    40h invalid command
    80h cannot respond yet (TOC not read, or no disc, or drive disconnected)
  second response (stat.2 set):
    04h seek failed
  unsolicited (stat.2 set):
    08h drive door became opened
```

Two of these matter enormously here:

- **`40h` on ReadN means the disc is not licensed for this drive.** "If you are
  reading an unlicensed disk without a modchip or first unlocking the drive,
  this command will first trigger INT5, without triggering INT3 or INT1. This
  INT5 is accompanied with the stat byte `0x3` and the following error byte
  `0x40`." Also: "discs whose region does not match the console region will also
  return error code 40h unless CDDA mode is enabled." [SPX] Our target is a
  **PAL** console booting a **modchipped CD-R** with a **PAL licence area**
  (`iso/LICENSEE.DAT`, GR-005), so this should not fire — but if the driver
  reports INT5/40h, the disc or the chip is the problem, not the code.
- **`08h` unsolicited INT5 = lid opened.** "When the shell is opened, INT5 is
  triggered regardless of whether a command was executing or not. When this
  happens, all bits except shell open and error are cleared in the status
  register." [SPX] The driver must handle an INT5 that belongs to no command.

---

## 3. Interrupt handling

### 3.1 What fires

The CD-ROM asserts **IRQ 2** of the PSX interrupt controller (`I_STAT`/`I_MASK`
at `1F801070`/`1F801074`), mask value `0x0004`. [SPX interrupts.md] The tree
already names it: `CDROM_MASK 0x004` in
`include/asm-mipsnommu/ps/interrupts.h`. [TREE]

The controller fires whenever `(HINTMSK & HINTSTS) != 0`. [SPX] With
`HINTMSK = 1Fh` that means any of INT1..INT5, plus the two sound-map flags we
do not use.

`HINTSTS & 7` gives the *type*, not a bitmask:

| Value | Name | When |
|---|---|---|
| 0 | NoIntr | nothing pending |
| 1 | DataReady | a sector (ReadN/ReadS) or a Play report is available |
| 2 | Complete | second response of Init/Pause/Stop/SeekL/SeekP/GetID/… |
| 3 | Acknowledge | first response — **every** command produces one |
| 4 | DataEnd | end of disc, or end of track with auto-pause |
| 5 | DiskError | command error, read error, licence error, **or lid opened** |

[SPX]

### 3.2 Queueing

> "The response interrupts are queued... if the 1st response is INT3, and the
> second INT5, then INT3 is delivered first, and INT5 is not delivered until
> INT3 is acknowledged (ie. the response interrupts are NOT ORed together to
> produce INT7 or so)." [SPX]

But it is not a real queue:

> "Instead of using a real queue, it's merely using some flags that do indicate
> which INT(s) need to be delivered. Basically, there seem to be two flags: One
> for Second Response (INT2), and one for Data/Report Response (INT1). **There
> is no flag for First Response (INT3)**; because that INT is generated
> immediately after executing a command." [SPX, "Responses"]

> "The flag mechanism means that the SUB-CPU cannot hold more than one
> undelivered INT1... accordingly, **the PSX can use only three of the available
> eight SRAM slots**: One for currently pending INT1, one for undelivered INT1,
> and one for currently/incompletely received sector." [SPX]

Consequence for the driver: there is no elasticity to trade against. Miss an
INT1 deadline and sectors are dropped **with no error flag anywhere**. See §7.

### 3.3 Acknowledging — the exact order

psx-spx's interrupt page states the general rule and the failure mode:

> "The correct acknowledge order is: First, acknowledge `I_STAT` … Then,
> acknowledge corresponding I/O port. When doing it vice-versa, **the hardware
> may miss further IRQs** (…there'll be no further edge, so `I_STAT` won't be
> ever set in future)." [SPX interrupts.md]

`I_STAT` bits are **edge-triggered** and are cleared by writing **0** to the bit
(1 = no change). So the ack is `I_STAT = ~0x0004`, not `I_STAT = 0x0004`. [SPX]

**Good news for this tree:** `arch/mipsnommu/ps/irq.c:do_IRQ()` already does
exactly this, before dispatching:

```c
    outw (~cpu_mask_tbl[irq], INT_ACKN_PORT);   /* I_STAT ack */
    mask_irq(irq);
    ... action->handler(irq, action->dev_id, regs);
    unmask_irq(irq);
```
[TREE]

So the driver's handler runs **after** `I_STAT` has been cleared, and only needs
to do the device-side ack:

```
    CD0 = 1;            /* bank 1 */
    CD3 = 0x1F;         /* HCLRCTL: clear ALL five flags — see below */
```

**Do not add an `I_STAT` write inside the CD handler** — `do_IRQ` has done it,
and doing it again after the device ack would be the documented wrong order.

**Ack with `1Fh`, not `07h`.** `07h` clears only INTSTS bits 0-2 and leaves
`BFEMPT`/`BFWRDY` (bits 3-4) set. Since the drive fires whenever
`(HINTMSK & HINTSTS) != 0` and we set `HINTMSK = 1Fh`, a stuck bit 3 or 4 would
hold the line asserted forever. This is not hypothetical: **both** PSXSDK
(`CDREG(3) = 7`) and the only existing PSXLinux CD-ROM attempt
(`CD_REGS[3] = 0x07`) ack with `07h` only. PSn00bSDK writes `1Fh`; PCSX-Redux's
bare-metal shell and psyqo clear `7` and `0x18` as two separate writes.
[CROSS §8.5] Either write `1Fh`, or set `HINTMSK = 07h` so bits 3-4 can never
assert — but do not mix `HINTMSK = 1Fh` with `HCLRCTL = 07h`.

**Loop until the cause reads zero.** psyqo's handler re-reads the cause after
acking and keeps going until `(cause & 15) == 0`, i.e. it explicitly handles more
than one interrupt per assertion of the line. [CROSS §6] Given that this tree's
`do_IRQ` dispatches one source per exception and then unmasks, a handler that
returns with a second CD interrupt already pending is relying on a fresh edge
that psx-spx says is edge-triggered. **Drain in a loop.**

### 3.4 Acknowledge drains the result FIFO

> "After acknowledge, the result FIFO is drained and if there's been a pending
> command, then that command gets send to the controller." [SPX]

**Read the response bytes before writing `HCLRCTL`.** PSn00bSDK does the reverse
plus a delay loop (§1.4) and apparently works; that is a discrepancy, not a
licence to copy. Recommended handler shape:

```c
    /* 1. read the type */
    write CD0 = 1;
    irq = CD3 & 7;

    /* 2. drain the result FIFO while RSLRRDY */
    n = 0;
    while ((CD0_read & 0x20) && n < 16) result[n++] = CD1_read;

    /* 3. if INT1, request the data block now (see 4.x) */

    /* 4. ack the controller */
    CD0 = 1; CD3 = 0x1F;
```

### 3.5 The two documented races

**(a) Unstable IRQ flags when polling.** [SPX, "Caution - Unstable IRQ Flag
polling"]

> "IRQ flag changes aren't synced with the MIPS CPU clock. If more than one bit
> gets set (and the CPU is reading at the same time) then the CPU does
> occassionally see only one of the newly bits:
> `0 ---> 1 ---> 3  ;0.1% glitch: occurs about once per thousands of INT3's`
> `0 ---> 4 ---> 5  ;1%   glitch: occurs about once per hundreds of INT5's`"

The stated workaround is to read `HINTSTS & 7` twice and use the second read.
psx-spx says the problem "applies only when manually polling the IRQ flags (an
actual IRQ handler will get triggered when the flags get nonzero, and the flags
will have stabilized once when the IRQ handler is reading them)", **except** for
"a combination of IRQ10h followed by IRQ3 [which] can also have unstable LSBs
within the IRQ handler". And: "**The problem occurs only on older consoles (like
LATE-PU-8), not on newer consoles (like PSone).**"

**VERIFY** for our target: SCPH-750x with a `12/16/97 E` BIOS is a PU-22/PU-23
class board [TREE docs/21], between "LATE-PU-8" and "PSone". psx-spx does not say
which side of the line it falls on. **Do the double read anyway** — it costs one
8-bit load and removes the question. Read the drive firmware date with
`Test 19h,20h` (`98h,06h,10h,C3h` = PU-22, `99h,02h,01h,C3h` = PU-23/PM-41) to
pin the board down. [SPX version table]

**(b) The lost wakeup.** Not from psx-spx — from this project's own history:

> "The acknowledge interrupt can arrive between the test and the sleep.
> `bu_interrupt()` sets `BU_READY` and wakes a queue nobody is on yet; the wakeup
> is lost… A card pulls `/ACK` about 100 µs after each byte, so **on real silicon
> it usually wins that race**. Emulators deliver the interrupt later and more
> coarsely, and usually do not. That is the entire 'works in DuckStation, fails
> on hardware' mystery." [TREE docs/22-WHAT-WENT-WRONG.md §7 Fault 1]

The CD-ROM is *slower* per event than a memory card (6.6 ms per sector at 2x
versus 100 µs per byte), so the race is less likely per event — but a `Nop` first
response takes only ~1.5 ms average and as little as ~0.55 ms (§7.1), which is
well inside the window of a sloppy `if (!done) sleep_on()`. **Use the same fix
`bu.c` got: make the test-and-sleep atomic under `cli()`, and loop rather than
test once.**

### 3.6 IRQ priority in this tree

`arch/mipsnommu/ps/int-handler.S` scans `cpu_mask_tbl` from index 0 upward and
dispatches the **first** match, one interrupt per exception entry. [TREE] The
index assignment in `include/asm-mipsnommu/ps/interrupts.h` is:

```
  0 TIMER2   1 CONTROLLER   2 PIO    3 DMA     4 TIMER0   5 TIMER1
  6 GPU      7 VBL          8 SIO    9 SPU    10 CDROM
```

**CDROM is index 10 — dead last, the lowest priority source in the machine.**
[TREE] Meanwhile `do_IRQ` masks the source it is servicing and unmasks it on the
way out, so a long-running SIO or VBlank handler delays the CD handler directly.

At 2x with 2048-byte sectors, the driver has **6.6 ms** between INT1s and only
about two usable buffer slots. **VERIFY** whether the serial console's SIO
handler (the monitor runs at 115200 with the kernel's own driver) can hold the
CPU long enough to drop sectors. If it can, options are: raise CDROM's priority
by renumbering the table, or fall back to 1x (13.3 ms budget) for the root
filesystem read, which is more than fast enough for a boot-time mount.

---

## 4. Sector formats and data transfer

### 4.1 What is actually on our discs

Raw layouts, exact offsets within the 2352-byte (930h) sector: [SPX cdromformat.md]

```
  Mode 1 (plain CD-ROM):
    000h 0Ch  Sync
    00Ch 4    Header (Minute, Second, Sector, Mode=01h)
    010h 800h Data (2048)
    810h 4    EDC
    814h 8    zero
    81Ch 114h ECC

  Mode 2 Form 1 (CD-XA):
    000h 0Ch  Sync
    00Ch 4    Header (Minute, Second, Sector, Mode=02h)
    010h 4    Sub-header (File, Channel, Submode AND DFh, Codinginfo)
    014h 4    copy of Sub-header
    018h 800h Data (2048)
    818h 4    EDC
    81Ch 114h ECC

  Mode 2 Form 2 (CD-XA):
    ... 018h 914h Data (2324)
        92Ch 4   EDC (or zero)     -- no ECC field at all
```

**Our discs are Mode 2 Form 1, verified [MEAS]:** `output/blackroo.bin` is
20,692,896 bytes = 8798 × 2352 exactly, and sector 16 reads

```
  sync 00 ff ff ff ff ff ff ff ff ff ff 00
  hdr  00 02 16 02                       <- mode 2
  sub  00 00 09 00 00 00 09 00           <- file 0, chan 0, submode 09h, dup
  data 01 "CD001" 01 00 "PLAYSTAT..."    <- the PVD, at offset 24 (018h)
```

Submode `09h` = Data (bit 3) + EOR (bit 0); **bit 5 (Form2) is clear**, so
Form 1, 2048 user bytes. [SPX subheader table] Sector 17 shows `89h` = the same
plus EOF (bit 7). This is exactly what `docs/19-BOOTABLE-CD.md` says mkpsxiso
produces, now confirmed byte-for-byte.

So with `Setmode` bit 5 = 0 the drive hands us **precisely the 2048 bytes of
user data**, header/subheader/EDC/ECC stripped. That is what we want: an ext2
image written as a file on this disc is a contiguous run of 2048-byte blocks
with no per-sector framing to strip in software.

### 4.2 The two delivery sizes

| Setmode bit 5 | FIFO length | Contents |
|---|---|---|
| 0 | `800h` = 2048 | user data only |
| 1 | `924h` = 2340 | everything from source offset `0Ch`: header(4) + subheader(4) + subheader copy(4) + data + EDC/ECC |

[SPX] With bit 5 = 1 on a Mode 2 Form 1 disc, user data starts at **delivered
offset 12 (0Ch)** — that is arithmetic from the two layouts above, and it is
corroborated by psx-spx's ReadN note about "the 2048 data bytes preceeded by a
12byte header". PSn00bSDK encodes the same split as word counts:
`_sector_size = (mode & CdlModeSize) ? 585 : 512` — 585 words = 2340 bytes,
512 words = 2048. [PSN cdread.c]

Reading past the end does not fault or return zeros; the hardware **repeats the
byte at index `[800h-8]` or `[924h-4]`** as padding. [SPX] A driver that
over-reads gets plausible-looking garbage, not an error.

**Recommendation: use `800h` (bit 5 = 0).** The whole-sector mode buys nothing
here — we are not doing software ECC, and the extra 292 bytes per sector is
14% more DMA traffic on a machine with 6.6 ms per sector.

### 4.3 DMA channel 3

Registers [SPX dmachannels.md; offsets already in TREE `ps/hwregs.h`]:

```
  1F8010B0h  D3_MADR   bits 0-23 = start address; not updated in SyncMode 0
  1F8010B4h  D3_BCR    SyncMode 0: bits 0-15 = word count, bits 16-31 = block count
  1F8010B8h  D3_CHCR   control
  1F8010F0h  DPCR      bits 12-14 DMA3 priority, bit 15 DMA3 master enable
  1F8010F4h  DICR      bit 19 DMA3 IRQ mask, bit 23 master enable, bit 27 DMA3 flag
```

`D3_CHCR` bits that matter: bit 0 direction (0 = device→RAM), bit 1 increment
(0 = +4), bits 9-10 SyncMode (0 = burst), bit 24 start/busy, bit 28 force start
without waiting for DREQ. "Bit 24 is automatically cleared upon COMPLETION of
the transfer." [SPX]

**The canonical value is `11000000h`** — psx-spx lists it under "Commonly used
DMA Control Register values" as `DMA3 CDROM 11000000h (normal)`, the BIOS trace
uses it, and PSn00bSDK writes exactly that. [SPX; PSN misc.c]

PSn00bSDK's whole sector-fetch routine:

```c
int CdGetSector(void *madr, int size) {
    DMA_MADR(DMA_CD) = (uint32_t) madr;
    DMA_BCR(DMA_CD)  = size | (1 << 16);      /* size = words, 1 block */
    DMA_CHCR(DMA_CD) = 0x11000000;
    while (DMA_CHCR(DMA_CD) & (1 << 24)) ;    /* spin until done */
    return 1;
}
```
[PSN misc.c]

so `D3_BCR = 0x00010200` for a 2048-byte sector (512 words, 1 block) — matching
the psx-spx BIOS trace exactly. There is also `CdGetSector2()`, identical but
with `CHCR = 0x11400100` (chopped, one word every 16 CPU cycles) and no spin —
for when the CPU must keep running during the transfer. [PSN misc.c]

Note that PSn00bSDK **does not** enable the DMA3 completion interrupt: it polls
`CHCR` bit 24, and `CdDataSync()` exposes that poll. [PSN misc.c] The BIOS
sequence in psx-spx does set `DICR` bits 19+23, but for a driver that already
has a per-sector CD IRQ there is no benefit in adding a second interrupt source.

Transfer cost: **24 clocks/word** with BIOS timings, 40 with the timings most
games set. [SPX] At 24 clk/word, 512 words = 12,288 cycles ≈ **0.36 ms** — about
5% of the 6.6 ms per-sector budget at 2x. Comfortable.

Also relevant: "**Any read access from RAM or I/O registers or filling more than
4 entries into the write queue will stall the CPU until the DMA is finished.**"
[SPX dmachannels.md] So the blocking `CdGetSector` spin is not really costing
anything the CPU could have used.

### 4.4 Manual FIFO reads

The alternative is a plain loop on `RDDATA` (`1F801802`):

> "RDDATA can be accessed with 8bit or 16bit reads (ie. to read a 2048-byte
> sector, one can use 2048 load-byte opcodes, or 1024 load halfword opcodes, or,
> more conventionally, a 512 word DMA transfer; the actual CDROM databus is only
> 8bits wide, so the CPU's bus interface handles splitting the reads)." [SPX]

**32-bit CPU loads are not endorsed and are dangerous here.** psx-spx names only
8-bit and 16-bit for the CPU, and separately documents that a 32-bit read of
`1F801800h` returns the same byte four times because auto-increment is off — and
that turning auto-increment on to fix that *breaks DMA*. [SPX "Misc"]
psx-spx's `unpredictablethings.md` compatibility table lists the CD-ROM ports as
**8-bit OK, 16-bit `?`, 32-bit `?`** — untested. **VERIFY** before using 16-bit
`lh` reads; **do not use 32-bit `lw` reads at all.**

Cost estimate: 2048 byte-loads against an 8-bit peripheral with a 5-cycle read
delay plus bus overhead. **VERIFY the real number by measurement** — the point
is that it is roughly an order of magnitude worse than DMA's 0.36 ms, and it
competes for the same 6.6 ms budget.

### 4.5 Which to use on a 2 MB no-MMU machine

**Use DMA.** The reasons are specific to this target, not general preference:

1. **Time budget.** 0.36 ms versus a PIO loop that plausibly costs several
   milliseconds. With only 2-3 usable sector slots in the drive's SRAM and no
   overrun flag, spending the budget on a copy loop is how sectors get silently
   dropped.
2. **No bounce buffer is needed.** With no MMU, `bh->b_data` is a plain physical
   address in the low 2 MB. DMA3's `MADR` is a 24-bit physical address, so the
   buffer-head's own memory can be the DMA target directly — no
   `virt_to_bus`, no IOMMU, no bounce. Convert with `PHYSADDR()`
   (i.e. mask off the KSEG0 bit), which the tree already uses in
   `prom/memory.c`. [TREE]
3. **Cache coherency is probably a non-issue.** The PS1's R3000A has a 4 KB
   I-cache and no data cache — its 1 KB "D-cache" is wired as the scratchpad at
   `0x1F800000`. If that is right, DMA into a KSEG0 buffer needs no invalidate.
   **VERIFY** with a one-sector test: DMA into a KSEG0 address, read it back
   through KSEG0, compare against the same sector read via KSEG1. If they differ,
   the assumption is wrong and every DMA buffer must be accessed through KSEG1.
4. **Memory.** With ~420 KB free after the kernel [TREE docs/23], a driver that
   allocated its own 2 KB bounce buffer per request would be affordable but
   pointless. DMA straight into the block layer's buffers costs zero extra RAM.

The one case for a PIO fallback is bring-up: a `brmon` command that reads one
sector by hand proves the command/IRQ path without also proving DMA. That is a
debugging aid, not the driver's data path.

### 4.6 The BFRD handshake and its timing subtlety

The documented order is: [SPX "CDROM Incoming Data / Buffer Overrun Timings"]

```
    Wait for INT1
    Send Data Request (BFRD = 1)
    Acknowledge INT1
    Copy Data to Main RAM (via I/O or DMA)
```

Note that the data request comes **before** the ack. psx-spx softens it:

> "it should be usually issued between receiving/acknowledging INT1 (however, it
> can be also issued shortly after the acknowledge; even if there are further
> sectors in the buffer, there seems to be a small delay between the acknowledge
> and the next interrupt, and Data Requests during that period are still treated
> to belong to the old interrupt)." [SPX]

PSn00bSDK follows the documented order, requesting data first, and preserves the
BIOS's peculiar dummy accesses:

```c
    if (irq == CdlDataReady) {
        // TODO: are the first 4 accesses really needed, or was this just
        // Sony's (dumb) way to flush the KUSEG write queue? We definitely
        // don't need to do that since we're using KSEG1.
        CD_REG(0) = 0;  CD_REG(0);
        CD_REG(3) = 0;  CD_REG(3);
        CD_REG(0) = 0;
        CD_REG(3) = 0x80;   // Request data
    }
    CD_REG(0) = 1;
    CD_REG(3) = 0x1f;       // Acknowledge all IRQs
```
[PSN common.c]

Those dummy reads/writes are the same ones in the psx-spx BIOS trace
(`[1F801800h]=00h / 00h=[1F801800h] / [1F801803h]=00h / 00h=[1F801803h] /
[1F801800h]=00h / [1F801803h]=80h`). Nobody knows if they are required.
**Keep them.** They cost six 8-bit bus cycles and they are what both the BIOS
and the only known-working open SDK do on real hardware.

---

## 5. Driver design for the Linux 2.4 block layer

### 5.1 Shape: `request_fn`, not `make_request_fn`

Use the classic 2.4 request-queue model, the same one `drivers/block/bu.c` uses:

```c
    register_blkdev(MAJOR_NR, "psxcd", &psxcd_fops);
    blk_init_queue(BLK_DEFAULT_QUEUE(MAJOR_NR), DEVICE_REQUEST);
    blk_size[MAJOR_NR]      = psxcd_sizes;      /* KB per minor   */
    blksize_size[MAJOR_NR]  = psxcd_blocksizes; /* 2048           */
    hardsect_size[MAJOR_NR] = psxcd_hardsects;  /* 2048           */
    read_ahead[MAJOR_NR]    = PSXCD_RAHEAD;
    request_irq(CDROM, psxcd_interrupt, SA_INTERRUPT, "psxcd", NULL);
```
[TREE bu.c:909-931 for the exact idiom this tree uses]

`make_request_fn` is the wrong tool here. It bypasses the elevator, and the
elevator is doing something we actively want: **merging adjacent requests into
long sequential runs.** The drive is a streaming device — one `Setloc` + `ReadN`
delivers sector N, N+1, N+2… at 6.6 ms each, while every restart costs a `Pause`
(~32 ms at 2x) plus a re-seek. Handing the driver 32 KB runs instead of 2 KB
singletons is the single biggest performance lever available.

`blk.h` needs an entry alongside the existing `BU_MAJOR`/`BU_LARGE_MAJOR` ones:

```c
    #elif (MAJOR_NR == PSXCD_MAJOR)
    #define DEVICE_NAME    "PSX CD-ROM"
    #define DEVICE_REQUEST do_psxcd_request
    #define DEVICE_NR(device) (MINOR(device))
```
[TREE include/linux/blk.h:330-343 shows the surrounding block]

### 5.2 Major number and `root=`

`include/linux/major.h` has `BU_MAJOR 207` and `BU_LARGE_MAJOR 208`; **209 is
free.** [TREE]

`init/main.c:name_to_kdev_t()` looks the name up in `root_dev_names[]`, and if it
is not there, falls through to `simple_strtoul(line, NULL, 16)` — **base 16**.
[TREE] There is no `psxcd` entry, so either add one, or pass the device
numerically:

```
    root=0xd100          # major 209 (0xd1), minor 0
```

**Incidental finding worth acting on:** the same code means `root=/dev/bul`, as
suggested in `docs/23-ROOT-FILESYSTEM-PLAN.md` §B2, does **not** resolve to the
memory-card device. `bul` is not in `root_dev_names[]`, so it is parsed as the
hex number `0xb` → major 0, minor 11. That plan needs `root=0xd000` (208 = 0xd0)
or a table entry. **VERIFY** by reading the boot log's ROOT_DEV.

### 5.3 Block size: 2048, and what that forces

`hardsect_size[209][0] = 2048` — the drive's native unit with `Setmode` bit 5
clear. `CURRENT->sector` and `CURRENT->current_nr_sectors` stay in 512-byte
units regardless (that is a block-layer convention, not a device property), so
the driver converts:

```c
    lba   = psxcd_base_lba + (CURRENT->sector >> 2);
    count = CURRENT->current_nr_sectors >> 2;
```

Setting `hardsect_size` to 2048 makes the block layer only ever issue requests
that are 2048-aligned and a multiple of 2048 — no read-modify-write, no partial
sectors, no bounce buffer.

**The consequence is a hard constraint on the disc image.** This tree's
`fs/ext2/super.c` reads:

```c
    hblock = get_hardblocksize(dev);
    if ((hblock != 0) && (sb->s_blocksize < hblock)) {
        printk("EXT2-fs: blocksize too small for device.\n");
        goto failed_mount;
    }
```
[TREE fs/ext2/super.c:485-491]

So **the ext2 image on the disc must be built with `-b 2048` or `-b 4096`.**
A 1024-byte-block image will be refused. Note that `docs/23-ROOT-FILESYSTEM-PLAN.md`
§B1 recommends `mke2fs -F -b 1024` — correct for `/dev/ram0`, wrong for this
device. Build the CD image with:

```bash
    mke2fs -F -b 2048 -d rootdir -t ext2 -I 128 -r 0 cdroot.img <blocks>
```

The alternative — `hardsect_size = 512` plus a driver-side 2048-byte sector
cache — buys 1024-byte-block compatibility at the cost of a cache, 4× request
count, and a whole class of alignment bugs. **Do not do it.** The image is
built by us; build it right.

`blksize_size[209][0] = 2048` as the initial soft blocksize; ext2 will call
`set_blocksize()` itself once it reads the superblock.

`blk_size[209][0]` is in **KB**. Exposing the whole disc: 8798 sectors × 2 KB =
17,596 KB for the current image. [MEAS]

`read_ahead[209]` is in 512-byte sectors. `bu.c` uses 2 (1 KB), which is right
for a 128-byte-sector memory card and far too small here. Start at **16**
(8 KB) and tune. **VERIFY** against free RAM — with ~420 KB free [TREE docs/23]
the buffer cache is not large, and aggressive read-ahead on a 2 MB machine can
cost more in eviction than it saves in seeks.

### 5.4 Read-only

Three places, all needed:

1. `psxcd_open()` returns `-EROFS` if `filp->f_mode & FMODE_WRITE`.
2. `do_psxcd_request()` calls `end_request(0)` for anything that is not
   `CURRENT->cmd == READ` — same shape as `bu.c`'s `"bad command"` arm.
   [TREE bu.c:500-508]
3. Mount read-only: `root=0xd100 ro`, and `MS_RDONLY` in the ext2 super.

There is no `Write` command on this hardware at all — the command set has no
sector-write opcode [SPX command summary] — so this is not a policy choice, it
is the device.

### 5.5 Where the ext2 image lives on the disc

The image is a file inside the ISO9660 filesystem, and the tree has no isofs, so
the driver needs its starting LBA from somewhere. Two options, in order:

**A. A `__setup()` parameter (do this first).** `mkpsxiso` can emit an LBA
listing at build time; bake the number into the kernel command line, which
`build.sh`/kloader already control:

```c
    static int __init psxcd_base_setup(char *s)
    { psxcd_base_lba = simple_strtoul(s, NULL, 0); return 1; }
    __setup("psxcd_base=", psxcd_base_setup);
```

The `__setup` machinery exists in this tree — `drivers/block/rd.c` uses it for
`ramdisk_start=` and friends. [TREE]

**VERIFY:** confirm the exact `mkpsxiso` flag that dumps LBAs for the version in
use (`iso/build-iso.sh` does not currently pass one), or read the LBA out of the
built `.bin` with a small host script that parses the root directory record.

**B. A ~60-line ISO9660 lookup in the driver (follow-up).** Read the PVD at
LBA 16, take the root directory extent, scan its records for the filename, take
`extent` and `size`. PSn00bSDK's `isofs.c` is a working model of exactly this
and is short. [PSN isofs.c] It survives disc rebuilds without a matching kernel
rebuild, which will matter once iteration starts.

Either way the driver exposes a device whose LBA 0 is the first sector of the
image, by adding `psxcd_base_lba` to every request. Do **not** expose the raw
disc and try to mount at an offset — 2.4 has no loop-offset for the root device.

### 5.6 The state machine

Modelled on `bu.c`, which drives its transfer entirely from the interrupt and
re-enters `do_bu_request()` from the handler. [TREE bu.c:138-186, 454-620]

```
  do_psxcd_request(q):
      INIT_REQUEST                         /* standard 2.4 macro */
      reject non-READ, reject out-of-range
      lba   = base + (CURRENT->sector >> 2)
      count = CURRENT->current_nr_sectors >> 2

      if (streaming && lba == next_expected_lba):
          /* drive is already delivering; just keep consuming INT1s */
          state = READING
      else:
          if (streaming) issue Pause, wait INT3+INT2       /* costly */
          issue Setloc(lba); on INT3 -> issue ReadN; state = READING

  psxcd_interrupt():
      irq = HINTSTS & 7   (read twice, use the second read)
      drain result FIFO
      switch (irq):
        INT3: expected first response; advance the command sub-state
        INT1: BFRD=1; wait DRQSTS; DMA 512 words to CURRENT->buffer
              ack; CURRENT->buffer += 2048; next_expected_lba++
              if (--count == 0) { end_request(1); do_psxcd_request(NULL); }
        INT2: second response of Pause/Init/SeekL; advance sub-state
        INT5: read the error byte; if lid-open, mark media changed and fail
              everything; else retry the run from the failed sector
        INT4: DataEnd — should not happen mid-image; treat as error
```

**Keep the drive streaming.** `ReadN` runs until stopped, and stopping is
expensive: a `Pause` second response takes ~32 ms at 2x, ~64 ms at 1x [SPX
timings], and PSn00bSDK warns that

> "the drive controller will not process any command properly for some time
> after a CdlPause command, so an external timer (the vblank counter) and manual
> polling are required to defer the next attempt" [PSN cdread.c header comment]

with a 60-vblank (~1.2 s at PAL 50 Hz) cooldown constant. So: only `Pause` on a
discontinuity or after an idle timer expires. Sectors that arrive while the
queue is empty are simply acked and discarded — that costs one ack per 6.6 ms,
which is nothing, and it keeps the head moving forward.

Remember the `Setloc`/`Read`/`Pause` contract: **after any `Pause`, always
`Setloc` before the next `ReadN`**, because a bare `ReadN` re-delivers the last
sector. [SPX §2.3 above]

#### CONFIRMED ON HARDWARE, 2026-08-26 — the cooldown is not advisory

The 60-vblank cooldown above was recorded here as a reason to *avoid* pausing.
It is more than that: **you must actually wait it out.** The first CD-root boot
failed exactly on this, and the failure is worth writing down because every
symptom points somewhere else:

```
psxcd: found ROOT.IMG at LBA 827, 4096 KB      <- ISO9660 lookup: correct
psxcd: PlayStation CD-ROM, image at LBA 827, 4096 KB, read-only
psxcd: lba 827: expected INT1, got INT3        <- first block-layer read
end_request: I/O error, dev d1:00 (psxcd), sector 0
EXT2-fs: unable to read superblock
Kernel panic: VFS: Unable to mount root fs on d1:00
```

The lookup left the drive streaming at the root directory, so the first read of
the image was a discontinuity: `Pause` → `Setloc` → `ReadN`, issued back to
back. **`Setloc` and `ReadN` both returned INT3, so both looked like they had
succeeded** — but the controller was still working through the Pause, and the
INT3 the read loop then saw was that backlog rather than a data-ready INT1.

Three things are needed after a `Pause`, and `psxcd_hw_stop()` did none of them:

1. **Wait out the cooldown** before issuing anything else. 1.3 s covers
   60 vblanks at PAL 50 Hz with margin.
2. **Drain the result FIFO of the *second* response**, not just acknowledge it.
   Per §1.3 a FIFO read past the end pads with zeroes and then silently
   repeats, so bytes left behind surface as a wrong answer to the next command.
3. **Flush any interrupt raised while it settled** — stale by definition.

An INT3 where INT1 was expected is the signature. It does not mean the command
failed; it means you are reading an *older* command's answer.

**Cost:** ~1.3 s per seek, on top of the ~32 ms Pause itself. Sequential reads
are unaffected. This is a stage-1 price, and it is most of the argument for
§5.6's interrupt-driven machine, which never pauses at all in steady state.

### 5.7 Sleeping, waking, timeouts

`HZ = 100` in this tree [TREE include/asm-mipsnommu/param.h:50], so a jiffy is
10 ms — **longer than the 6.6 ms per-sector interval at 2x.** Do not build any
per-sector timing on jiffies. Use jiffies only for coarse watchdogs:

- **First sector after a `Setloc`+`ReadN`:** includes an implicit seek of unknown
  duration ("The seek timings are still unknown, and they are probably quite
  complicated" [SPX]). Allow **3 seconds**. PSn00bSDK allows 180 vblanks ≈ 3.6 s
  at PAL. [PSN cdread.c `CD_READ_TIMEOUT`]
- **Subsequent sectors:** a much shorter watchdog, e.g. 500 ms, is enough to
  catch a wedged drive without tripping on a slow read retry.
- **Retries:** on timeout or INT5, re-issue `Setloc` at the first sector that
  did not arrive and `ReadN` again, up to ~3 attempts, then `end_request(0)`.
  PSn00bSDK's `_poll_retry()` is exactly this. [PSN cdread.c]

For the sleep itself, use the fixed idiom from `bu.c`'s post-mortem — atomic
test-and-sleep under `cli()`, in a loop:

```c
    save_and_cli(flags);
    while (psxcd_state != DONE && !timed_out) {
        interruptible_sleep_on_timeout(&psxcd_wait, timeout);
        /* re-test under the lock, do not assume the wakeup was ours */
    }
    restore_flags(flags);
```

[TREE docs/22 §7 Fault 1 — the ~25-year-old lost-wakeup bug that made the memory
card driver work in DuckStation and fail on hardware]

### 5.8 Memory

The driver's own footprint should be a few hundred bytes of state plus the
handler. **No sector buffer is required** — DMA writes straight into
`CURRENT->buffer`. On a 2 MB machine with ~420 KB free [TREE docs/23] that
matters: a naive design with a 2 KB bounce buffer plus a read-ahead cache could
easily cost 20-30 KB for no benefit.

`CURRENT->buffer` is a kernel virtual address; DMA3's `MADR` takes a **24-bit
physical** address, so convert with `PHYSADDR()` (strip the KSEG0 base). The
2 MB of RAM is entirely within DMA3's 24-bit reach, so no allocation constraint
applies. **VERIFY** that `bh->b_data` for buffer-cache buffers is always
2048-aligned when `blocksize == 2048` — DMA3 forces address bits 0-1 to zero
[SPX], so an unaligned buffer would silently corrupt.

### 5.9 Build plumbing

- `drivers/block/Config.in`: `bool 'PSX CD-ROM support' CONFIG_PSX_CDROM`
- `drivers/block/Makefile`: `obj-$(CONFIG_PSX_CDROM) += psxcd.o`
- `include/linux/major.h`: `#define PSXCD_MAJOR 209`
- `include/linux/blk.h`: the `MAJOR_NR == PSXCD_MAJOR` arm
- `configs/kernel/*_defconfig`: add the symbol

**GR-001/GR-002 apply:** after any config change, confirm the symbol landed in
`blackroo/include/linux/autoconf.h`, and delete every `*.o`/`*.a` before
rebuilding. `build.sh` does both automatically now, but check the file.

---

## 6. Timing, real-hardware measurements, and cross-implementation checks

### 6.0 Reference timings from psx-spx

All figures measured by nocash "in 33MHz units on a PAL PSone" [SPX]. Converted
here at the PS1 CPU clock of **33,868,800 Hz** [PSN hwregs_c.h `F_CPU`].

| Event | Cycles (avg) | ≈ ms | Range |
|---|---|---|---|
| First response (INT3), Nop, normal | `0C4E1h` = 50,401 | 1.49 | 0.55 – 5.94 ms |
| First response, Nop, motor stopped | `05CF4h` = 23,796 | 0.70 | 0.53 – 1.09 ms |
| First response, `Init` | `13CCEh` = 81,102 | 2.39 | min 0.94 ms, **max unknown** |
| Second response, `GetID` | `4A00h` = 18,944 | 0.56 | |
| Second response, `Pause` @1x | `21181Ch` | 64.0 | ≈ 5 sectors |
| Second response, `Pause` @2x | `10BD93h` | 32.4 | ≈ 5 sectors |
| Second response, `Pause` when already paused | `1DF2h` | 0.23 | |
| Second response, `Stop` @1x | `D38ACAh` | 409 | |
| Second response, `Stop` @2x | `18A6076h` | 763 | **slower** at 2x |
| INT1 rate @1x | `6E1CDh` = 450,509 | 13.3 | exact = 451,584 cyc |
| INT1 rate @2x | `36CD2h` = 224,466 | 6.63 | exact = 225,792 cyc |
| `Reset` (1Ch) mandatory software delay | `400000h` | 124 | = 1/8 s |
| `ReadTOC` second response | — | ≈ 1000 | "about 1 second" |

The exact INT1 figure is `SystemClock × 930h / 4 / 44100` = 451,584 cycles at
1x. [SPX] The listed averages are averages, not the exact period.

**Seek and spin-up times are explicitly undocumented.** "The seek timings are
still unknown, and they are probably quite complicated… The CDROM BIOS contains
some seek distance table, which is probably optimized for 72-minute discs.
80-minute CDRs may have tighter spiral windings… which will raise the seek time."
[SPX] Our current image is only ~18 MB [MEAS], so seeks are short, but the
timeout must be generous and empirical.

Throughput ceiling: 150 sectors/s × 2048 = **300 KB/s** at 2x, 150 KB/s at 1x —
before any per-request `Pause`/`Setloc` overhead. That is an order of magnitude
faster than `/dev/bul` (381 KB total across three cards at ~250 kHz).

### 6.1 Measured on real hardware — and it disagrees with psx-spx

The `ps1-tests` suite by JaCzekanski runs on a real console and commits the
captured logs. [CROSS §7 — https://github.com/JaCzekanski/ps1-tests, `cdrom/timing/psx.log`]
These numbers are more authoritative than any emulator constant and, in places,
than psx-spx's own PSone measurements.

| Event | Measured min | Measured avg | Measured max | ≈ ms (avg) |
|---|---|---|---|---|
| `Init` ACK (INT3) | 69,456 | 75,086 | 93,696 | 2.2 |
| `Init` complete (INT2) | 138,448 | 476,300 | 600,632 | 14.1 |
| `Nop` ACK | 24,024 | 45,993 | 137,104 | 1.36 |
| `Setloc` ACK | 33,856 | 57,955 | 182,160 | 1.71 |
| `Setmode` ACK | 25,824 | 48,242 | 137,776 | 1.42 |
| `Mute` / `Demute` ACK | ~26,700 | ~49,400 | ~177,000 | ~1.46 |
| First sector after `ReadN` @1x | 668,488 | — | 984,136 | 20 – 29 |
| First sector after `ReadN` @2x | 553,960 | — | 1,024,016 | 16 – 30 |
| Steady-state sector period @1x | — | ~446,100 | — | 13.2 |
| Steady-state sector period @2x | — | ~222,300 | — | 6.56 |
| `Pause` ACK | ~28,600 | — | ~44,000 | ~1.1 |
| `Pause` complete @1x | 1,006,000 | — | 1,022,000 | ~30 |
| `Pause` complete @2x | 1,029,000 | — | 1,046,000 | ~31 |

Three things fall out of this that matter for the driver:

1. **Command ACK latency is enormously jittery — a factor of 5.7 between min and
   max for a bare `Nop`** (24k to 137k cycles, 0.71 to 4.05 ms). Any timeout
   tighter than ~10 ms per command will produce sporadic spurious failures on
   hardware while passing perfectly in an emulator.
2. **`Pause` complete is ~30 ms and essentially speed-independent**, not the
   64 ms @1x / 32 ms @2x that psx-spx's "about 5 sectors" rule implies. psx-spx
   is measuring a PSone; this is a different console. Treat both as the right
   order of magnitude, and use ~100 ms as a `Pause` timeout.
3. **The real sector period is ~1.2 % faster than nominal** (446,100 vs the
   theoretical 451,584 at 1x). Anything that assumes an exact 1/75 s cadence
   drifts.

The read state machine's own comment in that test is worth internalising:

> "We must wait for the seeking bit to clear, otherwise we get an error. There's
> a time period inbetween the ACK and before the seeking bit gets set that it's
> also invalid. Instead we just wait for the first sector…" [CROSS §7]

That is: after `ReadN`'s INT3 there is a window in which `stat` reports neither
seeking nor reading, and a driver polling `stat` will read a state that means
nothing. **Wait for the first INT1, not for a status bit.**

### 6.2 Lid open/close, measured

From `cdrom/disc-swap/psx.log` on real hardware: [CROSS §7]

```
  CD IRQ=2, status=0x02
  *** Open the shell now ***
  Got IRQ 5 (expected 5), status 0x01
  *** Close the shell now ***
  Getstat: 0x12
  Getstat: 0x10
  Getstat: 0x00
```

Two surprises:

- **The unsolicited INT5 on lid-open reports `stat = 0x01`** — the error bit
  only. The shell-open bit is **not** set in that response. A driver that keys
  media-change off `stat & 0x10` in the INT5 will miss it.
- **Three consecutive `Getstat` (`Nop`) commands are needed** after the lid
  closes, walking `0x12` → `0x10` → `0x00`. One is not enough. PSn00bSDK's
  `isofs.c` independently arrived at sending `CdlNop` twice before believing the
  lid state. [PSN isofs.c] PCSX-Redux's emulator models the same thing with a
  multi-second `DRIVESTATE_RESCAN_CD` and the comment "**m_statP
  STATUS_SHELLOPEN is 'sticky' and is only cleared by CdlGetStat**". [CROSS §2]

For a root filesystem this mostly means: **if the lid opens, everything is over.**
Fail all outstanding requests and do not try to recover — the ext2 buffer cache
will be inconsistent with whatever disc comes back.

### 6.3 Where the implementations disagree

Cross-checked across DuckStation, PCSX-Redux (emulator *and* its bare-metal
`src/mips/` code), PSn00bSDK, PSXSDK, and the hardware logs. [CROSS]

| Topic | Disagreement | What to do |
|---|---|---|
| **`Setmode` bit 4** | DuckStation & PSn00bSDK: "ignore" bit. PCSX-Redux emulator **and the reverse-engineered retail BIOS**: selects a **2328-byte** sector size (`s_wordsToRead = 0x246`). psx-spx says the 2328 claim "doesn't seem to be true". | **Never set bit 4.** This is the one bit no two implementations agree on. |
| **IRQ ack value** | PSXSDK and Krush206's PSXLinux write `07h`. PSn00bSDK writes `1Fh`. PCSX-Redux bare-metal writes `7` then `0x18`. | Write `1Fh`. See §3.3. |
| **Command ACK delay** | PCSX-Redux emulator: 2,048 cycles for everything. DuckStation: 15,000 / 25,000 / 80,000 (Init). Hardware: 24k–137k, avg 46k. | **PCSX-Redux is off by more than 10×.** A driver that assumes a fast ACK passes there and fails on hardware. PSn00bSDK's `CD_ACK_TIMEOUT = 0x100000` iterations is the right order. |
| **`Init` complete** | DuckStation `INIT_TICKS = 4,000,000` (118 ms) as a floor. PCSX-Redux uses 4,100,000 for `Reset`, credited to Mednafen. Hardware: avg 476,300 (14 ms). | DuckStation is ~8× slow here. Do not calibrate timeouts against it. |
| **`Pause` complete** | DuckStation clamps to 2,000,000 @1x / 1,000,000 @2x. PCSX-Redux does the **inverse**: 1,000,000 @1x / 2,000,000 @2x. Hardware: ~1.01–1.05 M either way. | Neither emulator is right; both are wrong in opposite directions. |
| **Data FIFO model** | DuckStation: 8 sector buffers, re-delivers a missed sector, warns "Interrupt not processed in time, missed N sectors". PCSX-Redux: one byte buffer with a **modulo-wrapping** index that silently re-serves the same sector. | **Read exactly the word count and no more.** Over-reading gives different wrong answers on each emulator, and a third one on hardware (§1.5 padding). |
| **Register base** | PSXSDK, Krush206's PSXLinux, `ps1-tests`: cached KUSEG `0x1F801800`. PSn00bSDK and psyqo: **KSEG1 `0xBF801800`** (psyqo marks every CD register `WriteQueue::Bypass`). PCSX-Redux bare-metal: writes via `0x1F80…`, **reads responses via `0xBF80…`**. | **KSEG1.** See below. |
| **SBUS setup before DMA** | PCSX-Redux bare-metal sets **both** `0x1F801018 = 0x20943` and `0x1F801020 = 0x132C` immediately before every CD DMA. PSn00bSDK sets only `0x1F801018` once, relying on the BIOS default for COM_DELAY. | We do not have BIOS state. **Set both, in probe.** §2.4 Phase 0 already does. |
| **GetlocL / GetlocP / Test 60h responses** | These return **no `stat` byte**. PSn00bSDK's command-flag table encodes them as `0`; DuckStation notes `// NOTE: No STAT here.` PCSX-Redux's emulator pushes `m_statP` as byte 0 for GetlocP with a `// HACK?`. | Do not assume response byte 0 is `stat`. |

### 6.4 The KUSEG question, settled

§1.7 flagged the KUSEG-versus-KSEG1 question as VERIFY. The cross-check settles
the mechanism, if not the tree's current behaviour:

The PS1's R3000A has **no writeback data cache** — its 1 KB "D-cache" is wired as
the scratchpad — so DMA-to-RAM coherency in the usual MIPS sense is a non-issue.
What *does* exist is a **CPU write queue** on KUSEG/KSEG0 stores. That is what
the BIOS's otherwise-inexplicable throwaway read sequence before setting `BFRD`
is for, as PSn00bSDK guesses in its own comment:

> "are the first 4 accesses really needed, or was this just Sony's (dumb) way to
> flush the KUSEG write queue? We definitely don't need to do that since we're
> using KSEG1." [PSN common.c]

psyqo reaches the same conclusion structurally: every CD register is declared
`WriteQueue::Bypass`, which resolves the base to `0xBF801000` rather than
`0x1F801000`, and it exposes an explicit `flushWriteQueue()` for the cases where
a KUSEG write must be ordered against an IRQ-mask change. [CROSS §6]

**Conclusion: use KSEG1 (`0xBF801800`) and the write-queue question disappears.**
The throwaway reads then become optional — but keep them anyway (§4.6); six bus
cycles is not worth the risk.

### 6.5 Prior art: there is essentially none

A dedicated search found **no working CD-ROM driver in any Linux or BSD port to
the PS1.** [CROSS §5]

- `CodeAsm/PS1Linux` — the canonical uClinux 2.4 port this tree descends from.
  Storage is memory-card only. `CDREG0_PORT`..`CDREG3_PORT` are defined in
  `hwregs.h` and **never referenced anywhere**; grep for `1f801800` across that
  tree returns zero hits. All forks checked; none added a CD file. This tree
  inherits exactly the same dead defines.
- `Krush206/PSXLinux` — the one real find. Commit `af30188` (Jan 2024) adds
  `arch/mipsnommu/ps/cdrom.c`, 129 lines, WTFPL, built via `obj-y` but **called
  from nowhere** — no `register_blkdev`, no request function, no sector path.
  It uses cached KUSEG, acks with `07h` only, and has an outright bug:
  `while (params != NULL) { CD_REGS[2] = *params; params++; }` never terminates
  (the header declares a `params_len` the implementation dropped; it only
  "works" because every caller passes NULL). Its `CD_REGS[3] = 0xC0` on bank 0
  is a `HCHPCTL` write (BFRD|BFWR), not the "clear both FIFOs" its comment
  claims. **Read it for orientation; do not port it.**
- No BSD has ever been ported to the PS1 (the R3000A has no TLB, which is why
  this is a uClinux `mipsnommu` port in the first place).

The best actual model to work from is **PCSX-Redux's bare-metal
`src/mips/shell/cdrom.c`** — ~350 lines, MIT, hardware-tested, and it does
precisely init → `GetID` → `Setmode` → `Setloc` → `ReadN` → DMA → `Pause` →
parse the PVD → find a file. That is the same shape as this driver's probe path.
`src/mips/openbios/cdrom/statemachine.c` is the reverse-engineered retail BIOS
state machine and documents the BIOS's own sector-size decode and a retail bug
it declines to reproduce.

One item there is directly relevant to a different Blackroo problem:
`Krush206/PSXLinux`'s `arch/mipsnommu/ps/bios.c`/`bios.h` (`FakeEnqueueCdIntr`,
`SetConf`, `InstallDevices`, `AdjustA0Table`) is the only prior art anywhere for
re-entering the BIOS after a kernel takeover — see `docs/18-CART-RESIDENT-BOOT-RESEARCH.md`.

---

## 7. Known pitfalls and traps

### 7.1 Traps documented in the hardware reference

1. **Never send a command while an interrupt is pending.** The HC05 mainloop
   will not execute it; it "will simplily sit in the command register unhandled
   and can be easily overwritten"; and the controller "seems to hang" if the
   command is sent during `BUSYSTS`. [SPX] Check `BUSYSTS == 0` *and*
   `HINTSTS & 7 == 0`.

2. **Acknowledging drains the result FIFO.** Read the response bytes first.
   PSn00bSDK does it the other way with a delay loop [PSN common.c] — this is an
   unresolved conflict; take the conservative order. **VERIFY** whichever you
   choose against real hardware.

3. **The result FIFO wraps, it does not empty.** Past the response length it
   pads with `00h` to 16 bytes then restarts at byte 0, "so it'll always return
   the same 16 bytes". [SPX] Loop on `RSLRRDY` with a hard cap; never read a
   fixed count.

4. **The data FIFO pads by repeating a byte.** Reading past `[7FFh]` or `[923h]`
   repeats the byte at `[7F8h]` or `[920h]`. [SPX] Over-reading gives
   plausible garbage, not an error.

5. **`Setmode` bit 4 silently corrupts positions.** "data is randomly returned
   from the 'Setloc position minus 0..3 sectors'". [SPX] Keep it clear.

6. **After `Pause`, a bare `ReadN` re-delivers the last sector.** Always
   `Setloc` first. [SPX]

7. **`Init` resets mode to `20h`** (1x, 924h sectors). `Setmode` must come after
   `Init`, never before. [SPX]

8. **A second `Init` while the first is still pending is silently dropped** —
   no INT3, no INT5, nothing. A driver waiting for a response will hang. [SPX]

9. **Only ~2-3 sector slots are usable** despite a 32 KB SRAM buffer. "BUG: The
   drive controller seems to allow only 2 of those 8 sectors… it appears to jump
   directly to INT1 for the newest sector (skipping all other unprocessed
   sectors). There is no known way to get around that effect… sectors would be
   lost without notice (there appear to be absolutely no overrun status flags,
   nor overrun error interrupts)." [SPX] **There is no way to detect a dropped
   sector except by comparing headers via `GetlocL`.** If the filesystem shows
   sporadic corruption, this is the first suspect.

10. **`Stop` is slower at double speed than single speed** (763 ms vs 409 ms).
    [SPX] Counter-intuitive; do not use `Stop` in any hot path.

11. **`Pause` can fail with INT5(stat,80h) during a seek** — including the
    implicit seek at the start of `ReadN`. [SPX] Aborting a read that has not
    started yet needs a retry loop, not a single `Pause`.

12. **Unsupported commands leave their parameters in the FIFO**, which then leak
    into the next command. [SPX] Not an issue for the commands this driver uses,
    but any `Test` probing must follow an INT5 with `HCLRCTL` bit 6 (CLRPRM).

13. **Never do a 32-bit load from any CD-ROM port.** Auto-increment is off by
    default, so a 32-bit read of `1F801800h` returns the same byte four times —
    and turning auto-increment on to fix that breaks DMA. [SPX] 16-bit access is
    listed as untested in psx-spx's compatibility table. **8-bit only.**

14. **Command ACK latency varies by a factor of ~5.7.** Measured on hardware:
    a bare `Nop` acknowledges anywhere from 24,024 to 137,104 cycles
    (0.71 – 4.05 ms). [CROSS §7] Any per-command timeout under ~10 ms will
    produce sporadic failures on the console and none in an emulator.

15. **The lid-open INT5 reports `stat = 0x01`, not `0x11`.** The shell-open bit
    is not yet visible in that response. [CROSS §7, `disc-swap/psx.log`] Key
    media-change detection off the INT5 *event*, not off `stat & 0x10`.

16. **After the lid closes, one `Getstat` is not enough.** Hardware walks
    `0x12` → `0x10` → `0x00` across three consecutive `Nop`s. [CROSS §7]
    PSn00bSDK independently sends `CdlNop` twice before trusting the lid state.
    [PSN isofs.c]

17. **There is a window after `ReadN`'s INT3 where `stat` means nothing** —
    neither the seek bit nor the read bit is set yet. [CROSS §7] Wait for the
    first INT1, never for a status bit.

18. **`Pause` is rejected while a read or seek has just started.** DuckStation
    marks this "verified with hardware tests… the mech will reject pause commands
    if the game just started a read/seek, and it hasn't processed the first
    sector yet. This makes some games go bananas and spam pause commands until
    eventually it succeeds, but it is correct behaviour." [CROSS §1] Independent
    confirmation of psx-spx pitfall 11. Retry the `Pause`.

19. **An INT1 already in flight cannot be cancelled.** DuckStation: "there isn't
    much that can stop an INT1 once it's been queued on real hardware."
    [CROSS §1] A driver that issues `Pause` and then assumes no more sectors
    arrive is wrong; drain and discard.

20. **After a `Pause`, the controller ignores commands for a while.** PSn00bSDK
    defers the next attempt by 60 vblanks (~1.2 s at PAL) because "the drive
    controller will not process any command properly for some time after a
    CdlPause command". [PSN cdread.c] This is the strongest argument for §5.6's
    "keep streaming, `Pause` only on a discontinuity".

### 7.2 Emulator versus real hardware

This project has been burned by exactly this class of bug three times already
(`docs/22-WHAT-WENT-WRONG.md`). The specific CD-ROM exposures:

**a) The unstable IRQ flag glitch is a hardware-only artefact.** psx-spx measures
it at 0.1% of INT3s and 1% of INT5s on older consoles, and says it does not occur
on a PSone. [SPX] No emulator reproduces it. A polling loop that works perfectly
in DuckStation for a million sectors can misread a type value on real silicon.
Mitigation is one extra 8-bit load — do it unconditionally.
**VERIFY** which side of the LATE-PU-8/PSone line an SCPH-750x sits on; psx-spx
does not say. Probe with `Test 19h,20h` and match against the firmware date
table (`98h,06h,10h,C3h` = PU-22, `99h,02h,01h,C3h` = PU-23/PM-41).

**b) The lost-wakeup race is more likely on hardware, not less.** This is
counter-intuitive and it is exactly what bit `bu.c`:

> "A card pulls /ACK about 100 µs after each byte, so on real silicon it usually
> wins that race. Emulators deliver the interrupt later and more coarsely, and
> usually do not. That is the entire 'works in DuckStation, fails on hardware'
> mystery." [TREE docs/22 §7]

The CD's fastest response is ~0.55 ms, slower than 100 µs but still fast enough
to land between a test and a `sleep_on`. Make the test-and-sleep atomic.

**c) Interrupt latency and the 6.6 ms budget.** Emulators run the CPU as fast as
the host allows and typically deliver CD IRQs on a schedule that never stresses
the sector budget. On the real machine the CD IRQ is the **lowest-priority**
source in this tree (§3.6) and competes with a 115200-baud serial console. An
emulator will not show you the dropped sectors, and — per pitfall 9 — neither
will the hardware, directly. **Test on the console with the serial monitor
active, and validate the data** (checksum the image, or mount and `md5sum` a
known file).

**d) Sector delivery of unlicensed / raw discs differs.** psx-spx notes that on
unlicensed CD-Rs "the returned data is the whole sector or so… in fact the total
received data in the Data Fifo is 4096 bytes… (also Setloc doesn't seem to work
accurately on unlicensed CD-R's)". [SPX] Our discs carry a transplanted PAL
licence area (GR-005), so this should not apply — but if `Setloc` positions come
back off by a few sectors, check the licence area before suspecting the driver.

**e) Emulator timing constants are demonstrably wrong, in both directions.**
PCSX-Redux acknowledges every command in 2,048 cycles where hardware takes
24,000–137,000 — more than a factor of 10 optimistic. DuckStation floors `Init`
at 4,000,000 cycles where hardware averages 476,300 — a factor of 8 pessimistic.
The two disagree with each other about `Pause` in *opposite* directions, and
hardware matches neither. [CROSS §8.2-8.4] **Do not calibrate any timeout by
watching an emulator.** Use the `ps1-tests` numbers in §6.1, and give every
timeout an order of magnitude of headroom.

**f) Emulator disc images may not be Mode 2/2352.** The image format matters:
mounting a 2048-byte-per-sector `.iso` in an emulator exercises a different path
than the `.bin`/`.cue` Mode 2/2352 that gets burned. Test with the same
`output/blackroo.bin` that goes on the disc.

**g) Host-side verification is unreliable.** GR-007: `cdrdao read-cd --read-raw`
on the TSSTcorp SN-208FB returns near-zero data for these discs, which looks
like a catastrophic burn and is not. Verify with `dd if=/dev/sr0 bs=2048`.
[TREE GUARDRAILS.md]

### 7.3 Traps specific to this kernel tree

1. **`ps_irq_setup()` masks every interrupt.** `arch/mipsnommu/ps/setup.c` does
   `outw(0, INT_MASK_PORT)` under a comment reading "Unmask all irq". Writing 0
   to `I_MASK` **disables** everything. [TREE] `request_irq` → `unmask_irq` will
   re-enable CD, but do not assume any other source is live.

2. **CD is the lowest-priority IRQ** (index 10 of 10). [TREE, §3.6]

3. **`do_IRQ` already acks `I_STAT`.** Do not ack it again in the handler. [TREE]

4. **`hwregs.h` MDEC defines collide with DMA DPCR/DICR.** [TREE, §1.7]

5. **`mips_io_port_base = 0x1f800000` is KUSEG.** Use explicit KSEG1 pointers.
   [TREE, §1.7]

6. **`root=/dev/bul` does not resolve.** [TREE, §5.2] Use a numeric `root=`.

7. **Config changes need `autoconf.h` regeneration and a full object wipe.**
   GR-001, GR-002. [TREE GUARDRAILS.md]

---

## 8. Suggested bring-up order

Each step produces a specific, checkable observation. Do not skip ahead.

1. **`brmon cd id`** — a monitor command that runs the init sequence and issues
   `Test 19h,20h`, printing the four firmware bytes. Success: a plausible date
   like `98 06 10 C3`. This proves register access, the command path, and IRQ
   or polling, with no DMA and no disc dependency. Also settles the PU-22 vs
   PU-23 question from §7.2(a).
2. **`brmon cd stat`** — `Nop`, print `stat`. Success: bit 1 (motor) behaviour
   changes when you open the lid.
3. **`brmon cd read <lba>`** — full `Setloc`/`ReadN`/one INT1/PIO-read/`Pause`,
   hexdump the first 64 bytes. Read LBA 16 and expect `01 "CD001" 01 00
   "PLAYSTATION"`. [MEAS] This is the single most valuable test in the list: it
   proves the whole protocol against a known-good byte pattern.
4. **Same, via DMA3.** Compare against the PIO result byte for byte. This is
   also the cache-coherency test from §4.5(3): read the DMA buffer back through
   KSEG0 and through KSEG1 and compare.
5. **Multi-sector streaming** — 64 consecutive sectors in one `ReadN`, checksum
   against the host's copy of `output/blackroo.bin`. This is the test that
   catches dropped sectors (pitfall 9), and it must be run **with the serial
   monitor active** to be meaningful.
6. **Register the block device**, `dd if=/dev/psxcd bs=2048 count=N | md5sum`
   from the monitor or a userspace binary.
7. **`mount -t ext2 -o ro`**, then `root=0xd100 ro`.

Steps 1-3 are essentially PCSX-Redux's `src/mips/shell/cdrom.c` reimplemented
inside `brmon` — that file is ~350 lines of MIT-licensed, hardware-tested code
doing exactly this sequence, and is the best thing to read before writing the
first line. [CROSS §6]

---

## 9. Sources

### Local — read this session

| Path | What it gave |
|---|---|
| `~/projects/toolchains/psn00bsdk/sdk/PSn00bSDK-0.24-Linux/` | **Binary-only release** — `include/libpsn00b/psxcd.h`, `hwregs_c.h`, `psxetc.h`, prebuilt `libpsxcd_*.a`, examples. **No psxcd C source in the release.** |
| `…/include/libpsn00b/psxcd.h` | `CdlCommand` opcodes, `CdlModeFlag`, `CdlStatFlag`, `CdlIntrResult`, `CdlLOC`, `itob`/`btoi` |
| `…/include/libpsn00b/hwregs_c.h` | `IOBASE 0xbf800000`, `CD_REG(N)`, `DMA_MADR/BCR/CHCR`, `BUS_CD_CFG`, `F_CPU 33868800` |
| `…/include/libpsn00b/psxetc.h` | `IRQ_CD = 2`, `DMA_CD = 3` |
| `~/projects/blackroo-linux/bootloader/src/kernel.c` | Working `cdrom_boot()` using PSn00bSDK; one `Setloc` per sector (correct but slow) |
| `~/projects/psx-video/player/main.c` | `CdReadyCallback` streaming, `CdlSetfilter` + `CdlModeRT|CdlModeSpeed|CdlModeSF`, `CdlReadS`, `CdGetSector` per sector |
| `~/projects/blackroo-linux/blackroo/drivers/block/bu.c`, `bu.h` | The in-tree 2.4 block-driver idiom to copy |
| `…/blackroo/include/asm-mipsnommu/ps/interrupts.h`, `hwregs.h`, `io.h` | IRQ numbering, register offsets, `PSX_HW_REG_BASE` |
| `…/blackroo/arch/mipsnommu/ps/irq.c`, `int-handler.S`, `setup.c` | IRQ dispatch, priority order, `I_STAT` ack placement |
| `…/blackroo/fs/ext2/super.c` | The `hardsect_size` → ext2 blocksize constraint |
| `…/blackroo/init/main.c` | `name_to_kdev_t` / `root=` parsing |
| `…/blackroo/include/linux/major.h`, `blk.h` | Free major numbers, `DEVICE_REQUEST` plumbing |
| `~/projects/blackroo-linux/docs/19-BOOTABLE-CD.md`, `21`, `22`, `23` | Disc format, console identity, prior failures, the rootfs plan |
| `~/projects/blackroo-linux/GUARDRAILS.md` | GR-001/002/005/007 |
| `~/projects/blackroo-linux/output/blackroo.bin` | **Measured** sector layout (Mode 2 Form 1, 2352) |

### PSn00bSDK source — fetched from GitHub master

The 0.24 release ships no `psxcd` source, so it was fetched directly:

- `https://raw.githubusercontent.com/Lameguy64/PSn00bSDK/master/libpsn00b/psxcd/common.c` — `CdInit`, `_cd_irq_handler`, `CdCommandF`, `CdControl`, `CdSync`, command-flag table
- `.../libpsn00b/psxcd/cdread.c` — `CdRead`/`CdReadRetry`, the sector callback, retry/cooldown logic
- `.../libpsn00b/psxcd/misc.c` — `CdGetSector` (DMA), `CdGetSector2`, `CdDataSync`, `CdIntToPos`/`CdPosToInt`, `CdGetRegion`, `CdUnlock`, `CdGetToc`, `CdMix`
- `.../libpsn00b/psxcd/isofs.c`, `isofs.h` — ISO9660 lookup, a model for §5.5(B)
- `.../libpsn00b/psxcd/readme.txt`
- Repo root: `https://github.com/Lameguy64/PSn00bSDK` (MPL 2.0)

Fetch these from the upstream repository when working on the driver.

### psx-spx — primary hardware reference

Rendered pages, and the raw markdown they are generated from:

- https://psx-spx.consoledev.net/cdromdrive/ — `https://raw.githubusercontent.com/psx-spx/psx-spx.github.io/master/docs/cdromdrive.md`
- https://psx-spx.consoledev.net/cdromformat/ — `.../docs/cdromformat.md`
- https://psx-spx.consoledev.net/dmachannels/ — `.../docs/dmachannels.md`
- https://psx-spx.consoledev.net/interrupts/ — `.../docs/interrupts.md`
- https://psx-spx.consoledev.net/memorycontrol/ — `.../docs/memorycontrol.md`
- https://psx-spx.consoledev.net/iomap/ — `.../docs/iomap.md`
- https://psx-spx.consoledev.net/unpredictablethings/ — `.../docs/unpredictablethings.md`
- https://psx-spx.consoledev.net/cdrominternalinfoonpsxcdromcontroller/ — `.../docs/cdrominternalinfoonpsxcdromcontroller.md`
- Source repo: https://github.com/psx-spx/psx-spx.github.io

**Note:** `iomap.md` is partly stale — it still lists `1F801801h.W.1` and
`1F801801h.W.2` as "Unknown/unused", where `cdromdrive.md` documents them as
`WRDATA` and `CI`. Prefer `cdromdrive.md`.

Also note that the current psx-spx text has been substantially rewritten from
nocash's original: registers are now named after the Sony CXD1199 datasheet
(`HSTS`, `HINTSTS`, `HCLRCTL`, `HCHPCTL`, `RDDATA`, …) and the index is called a
"bank". Older documents and code (including PSn00bSDK) use the old names.

### Cross-implementation sources — [CROSS]

**Real hardware measurements (highest authority for timing):**

- **JaCzekanski `ps1-tests`** — https://github.com/JaCzekanski/ps1-tests —
  `cdrom/timing/psx.log`, `cdrom/disc-swap/psx.log`, `cdrom/getloc/psx.log`.
  Tests run on a real console with the captured logs committed. This is where
  every number in §6.1 and §6.2 comes from.

**Bare-metal, hardware-tested code (best models to work from):**

- **PCSX-Redux `src/mips/shell/cdrom.c`** —
  https://github.com/grumpycoders/pcsx-redux/blob/main/src/mips/shell/cdrom.c —
  ~350 lines, MIT. A complete poll-driven boot loader: init → `Init` → `GetTN` →
  `GetID` → `Setmode` → `Setloc`(PVD) → `ReadN` → DMA → `Pause` → parse PVD →
  find `SYSTEM.CNF;1`. Almost exactly this driver's probe path.
- **PCSX-Redux `src/mips/openbios/cdrom/statemachine.c`, `helpers.c`** — the
  reverse-engineered retail BIOS CD state machine, including its sector-size
  decode (`0x200`/`0x249`/`0x246` words) and a retail bug it declines to
  reproduce.
- **PCSX-Redux `src/mips/common/hardware/cdrom.h`** — ships both the KUSEG and
  the `_UC` KSEG1 mirrors of every register.
- **PCSX-Redux `src/mips/psyqo/`** — `hardware/cdrom.hh` declares every CD
  register `WriteQueue::Bypass` (base `0xBF801000`); `hardware/cpu.hh` has
  `flushWriteQueue()`; `src/cdrom-device-readsectors.cpp` is a clean
  `Setloc`→`Setmode`→`ReadN`→per-sector-DMA→`Pause` state chain.

**Emulators (document expected behaviour; treat their constants with care):**

- **DuckStation** `src/core/cdrom.cpp` (~4500 lines) —
  https://github.com/stenzek/duckstation — the most detailed seek model
  anywhere, an 8-sector-buffer data model, and a set of `// hardware tests
  show...` comments (notably: the mech rejects `Pause` if a read/seek has just
  started and the first sector has not been processed).
- **PCSX-Redux** `src/core/cdrom.cc` —
  https://github.com/grumpycoders/pcsx-redux — its lid/spin-up state machine
  carries "timing used in this function was taken from tests on real hardware".

**Other SDKs:**

- **PSXSDK** `libpsx/src/cdrom.c` — https://github.com/eagarciam/psxsdk
  (note: `frno7/psxsdk` does not exist). Poll-mode only, no DMA, no ISO9660,
  acks with `07h`, and has several outright bugs (§6.3). Reference only.

**Prior art in OS ports (there is almost none — see §6.5):**

- `CodeAsm/PS1Linux` — https://github.com/CodeAsm/PS1Linux — the canonical
  uClinux 2.4 port. Defines `CDREG0..3_PORT` and never uses them.
- `Krush206/PSXLinux` — https://github.com/Krush206/PSXLinux — commit `af30188`
  adds `arch/mipsnommu/ps/cdrom.c` (129 lines, WTFPL, unreferenced, buggy).
  Its `arch/mipsnommu/ps/bios.c` is separately relevant to `docs/18`.

**Vendor documentation:**

- CXD1199 decoder datasheet, "host interface" section — the official register
  documentation psx-spx's current text is derived from.
- Sony Developer Seminar Fall '96 CD-ROM notes —
  https://psx.arthus.net/sdk/Psy-Q/DOCS/TRAINING/FALL96/cdrom.pdf
- LibPSn00b Reference —
  http://psx.arthus.net/sdk/PSn00SDK/Docs/LibPSn00b%20Reference.pdf

---

## 10. Open questions — the VERIFY list

Collected in one place so they can be crossed off:

| # | Question | How to settle it |
|---|---|---|
| 1 | Read-then-ack, or PSn00bSDK's ack-then-delay-then-read? | Read a `GetTN` response both ways; compare against a known TOC |
| 2 | ~~Is KUSEG I/O safe, or must the driver use KSEG1?~~ | **Settled (§6.4):** the issue is the CPU *write queue*, not a data cache. Use KSEG1 `0xBF801800` — psyqo and PSn00bSDK both do. If `bu.c` is later shown to misbehave, its KUSEG `inb`/`outb` is a candidate cause |
| 3 | Does DMA into a KSEG0 buffer need a cache invalidate? | Almost certainly not — the R3000A has no writeback D-cache (§6.4). Prove it: DMA one sector, read back via KSEG0 and KSEG1, compare |
| 4 | Is the unstable-IRQ-flag glitch present on SCPH-750x? | `Test 19h,20h` for the firmware date; and do the double read regardless |
| 5 | Can the serial console's IRQ latency drop CD sectors? | 64-sector streaming checksum with the monitor active |
| 6 | Are 16-bit reads of `RDDATA` safe on this console? | Compare a PIO sector read done 8-bit vs 16-bit |
| 7 | Which `mkpsxiso` flag emits the LBA listing for the installed version? | `mkpsxiso --help`; or parse the built image |
| 8 | Is `bh->b_data` always 2048-aligned at `blocksize == 2048`? | Assert in the driver during bring-up |
| 9 | What `read_ahead` value is right with ~420 KB free? | Measure mount time and free memory at 2, 8, 16, 32 |
| 10 | Does `root=/dev/bul` in `docs/23` actually work? | Read `ROOT_DEV` from the boot log |
| 11 | Real cost of a PIO sector read on this console | Time it with Timer 1 or the cycle count from the monitor |
| 12 | Exact seek time distribution on an 18 MB CD-R | Time `Setloc`+`ReadN` first-INT1 across LBA distances |
| 13 | Does this tree's `do_IRQ` (one source per exception, then unmask) reliably re-enter on a second stacked CD interrupt? | Drain in a loop until `cause & 15 == 0` (psyqo's approach) and see whether the loop ever iterates |
| 14 | Which `Pause`-complete figure is right for SCPH-750x — psx-spx's ~64/32 ms or `ps1-tests`' ~30 ms? | Time it with Timer 1; use a 100 ms timeout meanwhile |
