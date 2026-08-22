# Getting Linux to boot on a PlayStation 1: everything that went wrong

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: BINFMT_FLAT.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> A field report from the session of 2026-08-20/21, in which Blackroo Linux
> booted from a CD-ROM on real PlayStation hardware for the first time.
> Written for anyone reconstructing the story — including the bits where the
> diagnosis was wrong.

The project began as a revival of **Runix**, a Linux 2.4 port to the PS1's
MIPS R3000A abandoned around 2007. Before this session it compiled, and it
booted in an emulator. It had never reached an interactive prompt on real
hardware.

By the end: **disc in, power on, Linux running, shell on the serial port.**

Nine distinct faults stood in the way. Most had nothing to do with Linux.

---

## The target

Measured from the machine itself, not assumed (`docs/21-TARGET-CONSOLE.md`):

| | |
|---|---|
| Console | SCPH-750x, PAL, modchipped |
| BIOS | `System ROM Version 4.1 12/16/97 E` |
| RAM | **2 MB stock** — no 8 MB mod |
| CPU | R3000A, `PRId = 0x00000002` |
| Link | SIO1 serial at 115200 to an FTDI FT232R |

Two consoles, identical except that the second has a **POWER REPLAY III**
cheat cart in the expansion port — which matters for later work, because the
BIOS pre-boot ROM scan is a way to boot without any disc at all.

---

## 1. The disc that would not boot (three CD-Rs)

**Symptom:** "Please insert PlayStation CD-ROM", then on later discs a frozen
SCE logo with the drive spinning forever.

**What we assumed:** the console was modchipped and a retail game (THPS2) had
just been played on it, so burned discs must boot.

**What was true:** a pressed disc proves nothing about a modchip. That
assumption cost three discs before it was tested.

**The actual fault:** the disc's **licence area** — the first 16 sectors. A
PlayStation checks it, and *a modchip does not cover this check*. The chip
answers the wobble/SCEx test; the licence check is separate.

Three states, three behaviours:

| Licence area | Console does |
|---|---|
| 28,032 zero bytes | "Please insert PlayStation CD-ROM" — clean rejection |
| Real `LICENSEE.DAT` via **mkpsxiso 2.20** | SCE logo, spins forever, no PlayStation logo |
| First 16 sectors copied from a disc known to boot | **Boots** |

The middle row is the trap, and it took the longest to see. Everything
compared identical against a working disc — TOC control flags (`control: 4`,
data), `CD-XA001` at PVD offset 1024, all identifiers, licence sectors 4-11
byte-for-byte — **except disc sectors 12-15**, the Form-2 tail of the licence
area. mkpsxiso 2.20 leaves those sector bodies empty; the older mkpsxiso that
built the homebrew discs this console boots starts each with
`00 00 08 00 00 00 08 00`.

The BIOS draws the PlayStation logo from that licence data. Ours hung exactly
there, before the logo ever appeared.

**Fix:** `iso/build-iso.sh` now transplants sectors 0-15 verbatim from a
reference image after mkpsxiso runs. Those sectors are pure licence area and
contain nothing of ours, so it is a safe splice.

**Lesson:** *boot a CD-R that has worked on that console before, as step one.*
It costs nothing and would have saved three discs and several hours.

---

## 2. Tooling that lied

Two pieces of tooling produced confidently wrong evidence.

**`cdrdao read-cd --read-raw`** on this drive (TSSTcorp SN-208FB) returns 0.6%
non-zero data — it looks exactly like a catastrophically bad burn. The same
disc read through the kernel driver (`dd if=/dev/sr0 bs=2048`) gives a perfect
volume descriptor and `SYSTEM.CNF`. The drive simply will not return raw
Mode 2 data. Fifteen minutes lost chasing a burn failure that never happened.

**PCSX-Redux renders the BIOS logo and then never shows our framebuffer.** For
hours it looked like the kernel was hanging at boot while it was in fact
running fine and answering on the serial port. DuckStation displays the same
kernel correctly.

**Lesson:** when a tool disagrees with another tool, find out which one is
lying before believing either.

---

## 3. The loader could not load the kernel — twice

**Symptom:** kloader's "Boot from CD-ROM" hung. Serial upload of the kernel
died at **offset 6144, deterministically, three times**, including once with
byte-level pacing that should have changed the behaviour if it were a timing
problem.

**Root cause:** kloader occupies `0x80010000..0x8001E800`. The kernel's load
address was `0x80010000`. `receive_data()` writes the payload straight to
`hdr.t_addr` — so the upload was **overwriting the running loader**. The first
three 2 KB chunks land on startup and menu code that is not executing; the
fourth reaches `0x80011800`, something live in the receive path, and the
machine dies mid-transfer.

Moving the kernel to `0x80020000` made it **worse** — now it failed at offset
0. PSn00bSDK places its heap immediately after the executable, and kloader's
`FntOpen()` allocates the font stream buffer there. `0x20000` is only 6 KB past
kloader's end: the very first chunk smashed the heap.

**Fix:** kernel relinked to `0x80090000`, clearing both the code and ~450 KB of
heap. Nothing is wasted — `prom_free_prom_memory()` returns the low memory to
the allocator once the kernel is running.

That relink also fixed kloader's CD chainload, which had been broken since it
was written, for the same reason.

---

## 4. Serial transfers that ate themselves

**Symptom:** with the addresses fixed, uploads still failed — sometimes at
offset 0, sometimes further in, always with kloader reporting an error or
simply freezing.

**Root cause:** kloader's `serial_getchar()` polls SIO1's RX-ready flag and
**cannot throttle the host** — there is no RTS handshake in that path. SIO1 has
an 8-byte receive FIFO. Any stall longer than ~700 µs at 115200 loses bytes,
and the VSync interrupt handler — which polls the controller over SIO0 — is
exactly that long. kloader then waits forever for data that already went past.

**First attempt (wrong):** pace the host — 8-byte bursts with 3 ms gaps, sized
so the FIFO can absorb a stall. It worked, at **1.5 KB/s**: ten minutes for a
742 KB kernel.

**Real fix:** mask interrupts for the duration of the transfer. The poll loop
is then never preempted:

```c
saved_imask = SHELL_INT_MASK;
SHELL_INT_MASK = 0;
... receive ...
SHELL_INT_MASK = saved_imask;
```

**742 KB in 71 seconds at line rate**, and the pacing hack became unnecessary.

The same insight fixed the screen: kloader could never show progress during a
transfer, because drawing means waiting for VSync and that drops bytes. The
answer is to draw **before the acknowledgement** — while the host is still
waiting for `OKOK`, nothing is in flight. It now shows
`RECEIVING - do not touch` at the one moment that is free.

---

## 5. A cache flush that crashed the machine

**Symptom:** the kernel launched and the console died instantly. No serial, no
video, screen frozen on kloader's last frame, no beacons.

**Root cause:** kloader's hand-rolled instruction-cache flush:

```c
"mfc0 $t0, $12\n"   /* read the status register... */
"lui  $t0, 1\n"     /* ...and discard it one instruction later */
"mtc0 $t0, $12\n"   /* SR := IsC only — IEc, BEV, everything cleared */
```

The `mfc0` result is thrown away by the next instruction, so the status
register is stomped to a bare "isolate cache" bit. Worse, the zeroing loop
runs **from cached KSEG0 while the cache is isolated** — with `IsC` set,
instruction fetches come from the isolated cache, so the loop can fetch
garbage and die. It was crashing *inside kloader*, before the jump.

**Fix:** call the BIOS `FlushCache()`, which is written for this CPU and is
available because the BIOS vectors are still installed. Confirmed at the
prompt afterwards: `SR = 0x10000401`, **`IsC=0`**.

While there: `DPCR = 0x07654321` was documented as "stop all DMA" but is the
*enable-everything* default. Now actually zero.

---

## 6. Two red herrings worth naming

**A baud mismatch that looked like a dead machine.** After a failed transfer
the console was left in the 518400 shell while the host talked at 115200.
Result: `0xAB` garbage, no beacons, and a confident (wrong) conclusion that the
kernel had crashed. The console's own debug screen — `BAUD: 518400` — settled
it in one photograph.

**Reading my own tool's output as the device's.** A diagnostic script printed
`blackroo> <command>` as a *label* before each command. For a while that was
mistaken for the console answering, and the monitor was reported working on
hardware when it was not. Actual device output arrived later and looked
completely different.

**Lesson:** photographs of the machine's own screen ended several of these in
seconds. Ask the hardware what it thinks before theorising.

---

## 7. Memory cards: three faults stacked

The driver reported `card in slot 1 not found` on hardware while DuckStation
reported `2 cards joined, total size = 254 Kbytes` from identical code.

A polled probe written into the monitor settled the hardware question
immediately:

```
blackroo> sio0 0 81
  byte 2 TX 00 -> RX 08  ack=YES      FLAG: card present
  byte 3 TX 00 -> RX 5a  ack=YES      ┐ memory card ID
  byte 4 TX 00 -> RX 5d  ack=YES      ┘
```

**The cards were fine.** Tap sub-port B (`0x82`) answers identically.

### Fault 1 — a lost-wakeup race, ~25 years old

```c
bu_state = BU_WAIT;
... add_timer(&bu_timer);
// check - may be we lose interrupt ?     <- the original author's comment
if (bu_state != BU_READY) {
    sleep_on(&bu_wait);
}
```

The acknowledge interrupt can arrive between the test and the sleep.
`bu_interrupt()` sets `BU_READY` and wakes a queue nobody is on yet; the wakeup
is lost; the task sleeps until the timer expires; the caller reports "not
found".

A card pulls `/ACK` about 100 µs after each byte, so **on real silicon it
usually wins that race**. Emulators deliver the interrupt later and more
coarsely, and usually do not. That is the entire "works in DuckStation, fails
on hardware" mystery.

Fixed by making the test atomic with `cli()`, and looping rather than testing
once.

### Fault 2 — the multitap was never addressed

`bu_rd_state0()` already emitted `0x81 + floor`, which is correct multitap
sub-port addressing. Nothing ever set `floor`. `BU_MINORS` was 2 and the field
was hardcoded to zero, so only the console's own two ports were ever probed —
and the cards were behind a tap.

Now `BU_MINORS = 8` with `minor = (port << 2) | floor`.

### Fault 3 — the driver demands its own header

```c
if (first_block.block.id != BU_ID)   /* 0x1234 */
    return 0;                        /* "bad card id" */
```

`bu_read_first_block()` rejects any card whose block 0 does not carry the
Blackroo magic. **A stock Sony card is therefore "not found" by design.** This
only ever worked in emulators because `mkmemcard` writes that header into
`.mcd` images.

So the cards need formatting on the console, which meant writing a memory-card
sector from the monitor — and that produced its own instructive bug.

### The sector-number bug

Three attempts returned `FF` — "bad sector" — while the card's opening
handshake was perfect. Instrumenting the *first* replies gave the answer in one
cycle:

```
head: ff ff 08 5a     FLAG then 5A — the card accepted the write command
```

The card was answering correctly; the sector number was in the wrong place.
The write sequence sends **two dummy bytes** after the command, during which
the card returns its `5A 5D` ID, and only *then* the sector:

```
81h -> N/A    57h -> FLAG    00h -> 5Ah    00h -> 5Dh
MSB -> 00h    LSB -> pre     data[0..127]  CHK
```

Sending MSB/LSB immediately after `'W'` made the card read `data[0], data[1]`
as the sector number — `34 12`, the low half of the `0x1234` magic being
written — i.e. sector 13330 on a 1024-sector card. **The card was right to
refuse.**

It hid for so long because only **sector 0** was ever read, and its address
bytes are `00 00` wherever they sit in the sequence.

**Result:** `end byte 47 = 'G' good`, and the header read back:
`id=00001234 size=1024 serial=b1acc000 number=0`. All four cards formatted.

### Fault 10 — a driver that only worked with its debugging on

With cards formatted, detection still found multitap sub-ports A and C but not
B — while the monitor's polled probe talked to every sub-port and had just
formatted all of them.

Three theories, all wrong: a settle delay *between* slot probes; the sequence
cascade (real, but not sufficient); a stuck `/JOYn` left asserted after a
successful transfer.

What actually found it: switching on the driver's own `#ifdef DEBUG` printks —
**and detection started working**. Nothing else changed. A driver that only
works with its diagnostics enabled has a timing bug, and the diagnostics are a
delay in disguise.

The fix is a 2 ms settle after asserting `/JOYn` in `bu_rd_state0()` — exactly
what the monitor's `mc_select()` had been doing all along, which is why the
polled code could reach sub-ports the driver could not.

**Verified with `DEBUG` off:**

```
PSX joined card: 127 Kbytes card found in slot 1
PSX joined card: 127 Kbytes card found in slot 3
PSX joined card: 127 Kbytes card found in slot 5
PSX joined card: driver initialized: 8 cards joined, total size = 381 Kbytes
```

381 KB of persistent storage on a machine with about 420 KB of free RAM.

---

## The scoreboard

| # | Fault | Where |
|---|---|---|
| 1 | Licence area Form-2 tail (mkpsxiso 2.20) | disc authoring |
| 2 | Assuming a modchip works because a retail disc does | process |
| 3 | Kernel linked on top of the loader's code | memory map |
| 4 | Kernel linked on top of the loader's heap | memory map |
| 5 | VSync IRQ eating serial bytes mid-transfer | loader |
| 6 | Corrupt hand-rolled cache flush | loader |
| 7 | Lost-wakeup race on the card ACK interrupt | driver (since ~2001) |
| 8 | Multitap sub-port never addressed | driver |
| 9 | Sector number two bytes early in card writes | new code |
| 10 | Missing settle after card select — only worked with DEBUG printks | driver (since ~2001) |

Two were in Linux. The rest were in disc authoring, the bootloader, the build
environment, and assumptions.

Raw serial captures for the card work are in `docs/captures/`.

---

## What it takes to build this today

The build environment was itself broken at the start of the session:

- The bundled EGCS 2.91.66 cross-compiler is 32-bit x86 and the host had **no
  32-bit loader** and no root. Solved by unpacking `libc6-i386` into the
  project and `patchelf`-ing a copy of the toolchain
  (`sdk/setup-local-toolchain.sh`).
- The kernel configuration was **decorative**: `include/linux/autoconf.h` was
  hand-maintained and 31 symbols out of step with the defconfigs, so config
  changes did nothing. `build.sh` now generates it and wipes stale objects.
- kloader could not be built at all — its Docker image was gone. **PSn00bSDK
  0.24 + GCC 12.3.0** now installed natively under `~/projects/toolchains`
  (`bootloader/build-native.sh`).

---

## The result

```
PSX SIO console enable
RAMDISK driver initialized: 16 RAM disks of 2048K size 1024 blocksize
Starting kswapd v1.8
PSX serial port driver

  ####  BLACKROO MONITOR  ####
  Linux 2.4.0.0pre0 on PlayStation (MIPS R3000A)
blackroo>
```

The prompt is **BRMON**, an in-kernel monitor on the serial port —
`peek`/`poke`/`md`/`ram`/`hw`/`cpu`/`mem`/`card`/`sio0`. It exists because
this kernel builds `BINFMT_FLAT` with no ELF loader, so the 1.5 MB ELF BusyBox
in the tree can never be exec'd. A monitor running in kernel context needs no
userspace, no initrd, and fits a 2 MB console with the kernel alone.

It also debugged the memory-card problem that a userspace shell could not have
touched.

**Storage works:** three memory cards joined as `/dev/bul`, 381 Kbytes,
readable through `bread()`.

**Still open:** a real userspace (every busybox in the tree links at
`0x00400000` — 4 MB, which does not exist on this machine), a CD-ROM block
driver (700 MB of storage the console already reads, against 420 KB of free
RAM), and a filesystem on the cards.
