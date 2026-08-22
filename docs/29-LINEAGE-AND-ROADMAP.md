# Where this came from, and where it is going

> One page for the whole project: the code's lineage, what has been built, and
> what is left. Written 2026-08-26, at release 0.5.0 "Rootstock26".

---

## Part 1 — the lineage

### The PlayStation was never meant to do this

No MMU. No keyboard port. No disk. 2 MB of RAM that a Linux 2.4 kernel very
nearly fills by itself. Every part of this project is a way around one of those
four facts.

### Runix (~2003–2007) — the base, and all that survived of it

Someone ported Linux to the PlayStation and stopped. The project was hosted on
Google Code, the authors are not fully known, and what came down to us is:

- **`arch/mipsnommu/`** — the entire no-MMU MIPS architecture port. Boot entry,
  exception handlers, interrupt mapping, the PS1 timer, the serial console,
  RAM setup, CPU identification, the ELF-to-ECOFF converter.
- **Documentation recovered from the Wayback Machine**, 2003–2004 captures.

That is the foundation, and it is genuinely most of the hard architectural work.
What it did **not** have: a bootloader, any storage driver, a CD-ROM driver, a
keyboard, a usable userspace, or a way to build it on a modern machine.

`SOURCE-ATTRIBUTION.md` names every file that came from Runix.

### Hoang Haviss — initrd and early userspace

The initrd path and early userspace work. Still load-bearing: `HoangFlag` in
`blackroo/Makefile` sets the include path on every compile of every file in the
kernel.

### Blackroo (2022–2026) — everything else

Research first, code later. Three years of reading psx-spx, PSn00bSDK,
PCSX-Redux's bare-metal sources and JaCzekanski's hardware test logs before most
of this was written, because the cost of guessing on this machine is a CD-R and
a boot cycle per attempt.

What was built here, none of which existed in Runix:

| | |
|---|---|
| **kloader** | a bootloader with a menu, memory-card manager, video modes and serial upload |
| **`bu.c`** | memory cards as a block device, eight of them joined through a multitap |
| **`psxcd.c`** | the CD-ROM as a Linux block device — the first working one on any PS1 Unix port |
| **`psxkbd.c`** | a keyboard on the controller bus |
| **`psxcon.c`** | a console drawn by the GPU on the television |
| **`binfmt_fixed.c`** | a fixed-address ELF loader for a machine with no MMU |
| **`brsh`** | the shell, and the utilities, in one freestanding binary |
| **BRMON** | an in-kernel monitor — where every driver above was proven first |

### The method, stated once

**Prove it in the monitor before writing a driver.** BRMON has no interrupts, no
scheduler and no other users of the bus, so a failure there is wiring or
protocol, not a race in somebody else's code. The CD-ROM driver was written
around a command path that had already been shown to work by hand. So was the
keyboard. So were the memory cards.

The corollary, learned the expensive way: **a milestone without a dated capture
is a claim, not a result.**

---

## Part 2 — where it is now

Release 0.5.0 boots from a CD-R with no host PC involved:

```
BIOS ─► kloader ─► LINUX.EXE ─► psxcd finds ROOT.IMG ─► ext2 ─► brsh on the TV
```

- **Root filesystem on the disc**, found by walking ISO9660 — no LBA compiled in
- **192 KB userspace window**, paid for by taking 96 KB out of the kernel
- **Memory cards** readable as `/dev/bul`, 508 KB; kloader saves its settings to
  card 0 block 1
- **Keyboard** on the controller bus; **console** on the television
- **The complete source ships on the disc** as `SOURCE.TGZ`

---

## Part 3 — the roadmap

Ordered by what unblocks what, not by how interesting it is.

### Storage and writing

**Memory-card writes through the block layer.** Only reads have run on
hardware. kloader writes cards (that is how settings persist), so the protocol
is proven — but `bu.c`'s write path has never been exercised. Until it is,
nothing can be saved on this machine.

**A filesystem on `/dev/bul`.** `mount` and `umount` exist in the shell now, so
once writes work: `mount /dev/bul /mnt` and `cp` onto a card. 508 KB, formatted
`-b 1024`. This is the writable half of the design in `docs/28` — the CD is the
system disk, the cards are `/home`.

**PicoMemcard on the expansion port** (`docs/11`, `docs/15`). The RP2040 already
emulates a memory card on the controller bus. On the **parallel/expansion port
(EXP1, `0x1F000000`)** it could be much more: bulk storage without the card
protocol's 8 MB ceiling and ~31 KB/s, and **WiFi** via a Pico W. The port is
already proven addressable — `docs/18` confirmed the BIOS pre-boot hook against
a real Power Replay III cart, and `pioflash.c` erases and programs cart flash.
Wants: a parallel-port block driver, and a protocol that is not the SIO0 one.

### Input

**The PlayStation mouse.** Hardware is in hand. Device ID `0x12`, on the same
controller bus as the keyboard, and it needs no adapter — `docs/14` rejected the
homebrew mouse-keyboard protocol precisely because a real mouse is simpler.
Folds into `psxkbd`'s existing poller as one SIO0 input driver rather than a
second one competing for the bus. Prerequisite for the desktop in `docs/28`.

**The PS2 infrared receiver.** Also in hand. The PS2 IR unit speaks a documented
protocol; on a PS1 it would be a remote for a media-player mode, and it is the
only input device here that needs no cable to the user.

### The console and the shell

**A full-screen `top` and a full-screen editor.** Both exist as line-oriented
commands now (`top`, `edit`), and both are limited by the same missing piece:
brsh has no termios, so it cannot put the terminal in raw mode or address the
cursor. Add raw-mode `ioctl` handling and the screen versions follow, including
a real `htop` — the process data is already there via `sys_blackroo_tasks`.

**BusyBox.** Blocked on a toolchain, not on the kernel: prebuilt mipsel uClibc
toolchains target mips32r2, which an R3000 cannot execute. Needs crosstool-NG or
Buildroot configured for **MIPS I**. `tools/busybox-mips-uclibc` proves the size
is fine — a static R3000 BusyBox at 255 KB — but it links at `0x400000`, which
does not exist on a 2 MB machine.

### The drive

**Interrupt-driven CD streaming** (`docs/24` §5.6). Stage 1 is synchronous and
polled, and pays ~1.3 s per seek because the controller must be let alone after
a `Pause`. The state machine that keeps the drive streaming and never pauses in
steady state is the fix, and it is the single biggest performance item left.

### Networking

**An ESP32 serial bridge, not a network stack.** Telnet *into* Linux would mean
restoring `CONFIG_NET` (30 KB of the 96 saved), writing a network driver for a
bus that has none, and running a daemon on a kernel with no `fork()`. An ESP32
on **SIO1** bridging WiFi to serial gives the console, kloader's upload protocol
and BRMON over the network for **zero kernel bytes** — including uploading
kernels without walking to the console. Do this one first; the other is a
project in its own right.

**Two consoles, linked** (`docs/22`). Still open, and more interesting once
either of the above works.

### The desktop

`docs/28` phases B and C: a `/dev/psxgpu` char device to submit display lists,
then drawing the file manager with GPU primitives instead of text, then the
mouse. Assets live in VRAM — a separate megabyte — so icons and fonts cost
nothing from the 2 MB. The model is Mac System 1, because its constraints were
ours: no memory protection, no multitasking, one program at a time, and
everything drawn with primitives rather than into a framebuffer.

---

## Credits

```
Chelson Aitcheson    project lead - research, hardware bring-up, integration
Hoang Haviss         initrd and early userspace
The Runix project    the original PS1 no-MMU kernel port (~2003-2007)
```

And the people whose measurements this is built on: psx-spx, Lameguy64
(PSn00bSDK, mkpsxiso), the PCSX-Redux authors, JaCzekanski (`ps1-tests`), and
the DuckStation authors. Full attribution in `SOURCE-ATTRIBUTION.md`.
