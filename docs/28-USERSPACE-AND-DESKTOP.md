# Userspace, a distribution, and a desktop — the plan

> Written 2026-08-25, once the CD-ROM block driver worked. Everything before
> this point was about making the hardware do things; this is about what to put
> on it, and it is constrained almost entirely by 2 MB of RAM.

## The floor: 2 MB, no mod

The 8 MB modification is not available — the chips could not be sourced — so
**every design here has to work in 2 MB.** That is a discipline, not a
disappointment: it is the same budget that produced the software this plan
imitates.

Measured on hardware, booting from CD-R:

```
Memory: 436k/1984k available (689k kernel code, 1548k reserved, 29k data, 40k init)
mem:  free pages 59   (= 236 KB)
```

**436 KB free at boot, 236 KB once the ramdisk is populated.** Earlier notes in
this project said "about 1 MB free"; that was wrong, and the correction matters
because it is most of the budget for everything below.

The kernel is the problem, not userspace. 689 KB of text on a 2 MB machine, and
it has never had a size pass — removing the FPU emulator once saved 47 KB, which
suggests there is more of that sitting there.

### Where the space is meant to end up

| | Target | Actual, 2026-08-25 |
|---|---|---|
| Kernel text | 500 KB (from 689 — config trim + flags) | **631 KB** (646,496) |
| Kernel data + structures | ~150 KB | 149 KB (data + bss) |
| **Userspace window** | **512 KB** | **192 KB** |
| Buffer cache / free | ~350 KB, elastic | not yet measured on hardware |

The 500 KB text target is not reached and may not be reachable: what is left
is `fs/` (245 KB), the VT console, the tty layer and ext2, and none of it is
optional on a machine that has to mount a filesystem and draw on a television.
The 512 KB window is correspondingly out of reach; 192 KB is what the 96 KB
saved could honestly fund, and it is still three times what there was.

`CONFIG_BLACKROO_USER_RESERVE_KB` is how the window is taken: the kernel is
simply told there is less RAM than there is, so the page allocator never sees
the top of memory and `binfmt_fixed` hands it out by hand. There is no inherent
limit on its size — only on what can be spared.

It is **one** number, in `include/asm-mipsnommu/blackroo-user.h`. It used to be
three — `prom/memory.c`, `fs/binfmt_fixed.c`, and a copy in
`userland/blackroo.ld` that no comment mentioned — and they had already
drifted: `userland/build.sh` documented `0x801c0000, 256 KB` for code that used
`0x001f0000, 64 KB`. `userland/build.sh` now refuses to build if the link
script and the defconfig disagree.

## Why a CD root changes the arithmetic

A ramdisk root is a **fixed** cost that never shrinks: the whole filesystem is
resident forever. A CD root is served by the buffer cache, which is **elastic**
and gives memory back under pressure. That difference is worth more than the
28 KB the current initrd occupies, because a real root filesystem would be
megabytes.

The disc is read-only, and Linux wants somewhere to write. The answer uses
hardware that already works:

**The CD is the system disk; the memory cards are the writable volume.**
700 MB of read-only `/` on the disc, 381 KB of `/dev/bul` as `/home` or `/var`.
That is how a 1985 workstation was arranged, and both halves are already
driver-complete.

## What actually runs on a no-MMU kernel

Worth stating plainly, because it decides the shape of everything:

- **No `fork()`** — only `vfork()` semantics, where the parent is suspended
  until the child execs or exits. This is what breaks job control and most
  build systems.
- **No demand paging, no COW, no overcommit.** Every binary is fully resident.
- **No memory protection.** A userspace bug can scribble on the kernel. This is
  why `binfmt_fixed` validates every `PT_LOAD` against the reserved window
  before copying a byte.
- **One load address**, so one distinct program image at a time. This is an
  implementation limit, not an architectural one — see "the way out" below.

The kernel itself is fully preemptive and multitasks properly. The limit is
**program loading**, not scheduling. The accurate description is
*single-program-image*, not "cooperative".

Things that are known to work on no-MMU Linux: BusyBox (it has explicit
`BB_MMU` support), ash/dash, most coreutils, `vi`. Things that do not: anything
expecting `fork()` to return a copy, anything mmapping files privately, anything
assuming address-space isolation.

## Two different products, not two options

A BusyBox *clone* would only ever be a worse BusyBox. But a monolithic binary of
our own is not competing with it:

- **Our binary = System + Finder.** Shell, file manager, editor, desktop, all
  integrated in one image with internal commands.
- **BusyBox = the Unix userland**, for when `sed`, `find` and pipelines are
  wanted.

They coexist. The order is decided by what blocks what:

- **Real BusyBox is blocked on a toolchain.** Prebuilt mipsel uClibc toolchains
  target mips32r2, which an R3000 cannot execute, so it means crosstool-NG or
  Buildroot configured for **MIPS I**. Days of work with real risk of fighting
  it. `tools/busybox-mips-uclibc` proves the size is fine — a static R3000
  BusyBox at **255 KB resident** — but it links at `0x400000`, which does not
  exist on a 2 MB machine and cannot be aliased there (2 MB mirrors that
  address onto the kernel).
- **Our own binary is blocked on nothing.** `mipsel-none-elf-gcc` works today
  and `userland/brsh.c` proves the model: freestanding, raw syscalls, 7 KB.

So: grow `brsh` now, build the toolchain later. And note the binary that grows
is the same one that becomes the desktop — nothing is thrown away.

## Mac System 1 is the right model, not a whimsical one

System 1 ran in 128 KB with a 400 KB floppy, no memory protection and no
multitasking. **The Finder quit to launch an application, and the application
returned to the Finder.** Everything was drawn with QuickDraw primitives —
rects, regions, blits — not into a pixel framebuffer.

Every one of those constraints is ours. Finder→app→Finder *is*
single-program-image with a return-to-shell, described from the user's side
instead of the loader's. And a primitive-based drawing model maps onto the PS1
GPU's display lists far better than any modern toolkit, all of which assume a
framebuffer the CPU can write — which this machine does not have at all.

System + Finder was around 216 KB in a 128 KB machine. We have 2 MB and 700 MB
of disc. **We are richer than the machine this design shipped on.**

### How the desktop gets built

Same single binary, gaining a graphics mode:

1. `ioctl(KDSETMODE, KD_GRAPHICS)` — already honoured in this tree
   (`vt.c:521`, and `console.c` checks `vcmode` in six places), so the text
   console stops drawing over you
2. A `/dev/psxgpu` char device to submit display lists — a thin wrapper over
   the GPU code already in `arch/mipsnommu/ps/libpsx/libpsx.S`
3. Draw with primitives: Gouraud quads for bevels and title bars, sprites for
   icons and the pointer, a texture-atlas font
4. **Assets live in VRAM**, a *separate* megabyte — icons, fonts and chrome
   cost nothing from the 2 MB
5. "Applications" are internal modes: Finder→app→Finder without leaving the
   binary

Phased so each step stands alone:

- **Phase A — a TUI on the existing console.** A Finder-shaped file manager in
  text at 78×21. No new kernel code; useful the moment there is a filesystem to
  browse, and it settles the interaction model and every file operation.
  **Started 2026-08-25:** `brsh` now has `ls`/`cat`/`hexdump`/`stat`/`cd`/`pwd`/
  `mkdir`/`rmdir`/`rm`/`cp`/`mv` as internal commands — the file operations a
  Finder needs, proven at a prompt before anything has to draw them.
- **Phase B — `/dev/psxgpu`**, drawing the same UI with primitives.
- **Phase C — the mouse**, folded into `psxkbd`'s existing poller as one SIO0
  input driver rather than a second competing one.

## On writing it in MIPS assembly

**MIPS is the worst architecture to hand-optimise for size.** Fixed 32-bit
instructions and no compressed encoding — MIPS16 came later and the R3000 does
not have it. Hand-written assembly might beat the compiler 20–30% on a specific
routine, but nobody is rewriting 689 KB of kernel, and the density simply is not
there the way it would be on x86 or Thumb.

The levers that actually pay, in order — **revised 2026-08-25, after
measuring**:

1. ~~**Compiler flags.**~~ **Measured and closed.** `-Os` does exist in EGCS
   2.91.66 (its `cc1` carries `optimize_size`, so the flag is not silently
   ignored) and it is worth **2,400 bytes — 0.32%**, not the 5–15% guessed
   here. `-O1` is 23 KB *worse* than `-O2`; `-fno-inline-functions` changes
   nothing, because 2.91's `-O2` never enabled `-finline-functions`. This
   compiler predates the pass work that made `-Os` mean something. The flag is
   kept — it is free — via `BLACKROO_OPT` in the top Makefile, but nothing else
   is coming from here. The full table is in `logs/sizepass/SUMMARY.md`.
2. **Not linking what is unused.** This turned out to be the *entire* game:
   **95,936 bytes, 12.9%**, all of it from here. `CONFIG_PROC_FS` off (34,052);
   `net/` not linked at all (30,200 — `CONFIG_NET` was already off and 2.4 was
   building `socket.o` and `net/core` regardless); pty, `/dev/raw`, the misc
   registry and the entropy pool (17,168); `kmod` and `binfmt_flat` (7,616);
   `/dev/vcs` (4,500).
3. **Root off RAM**, per above. Still outstanding, and now the largest single
   remaining item: it retires `drivers/block/rd.o`, 17,692 bytes.

Assembly is a distant fourth, and only ever for something specific and hot.

**The lesson worth carrying forward:** the estimate that led this list was out
by a factor of thirty, and the one-line test that disproved it took nine
seconds. Measure the lever before ordering the list by it.

## The plan

1. **Kernel size pass.** Measure `-Os`, drop `CONFIG_PROC_FS`, find what
   `char.o` and `network.o` really cost. Low risk, entirely measurable, and
   every KB goes straight to the userspace window. **Do this first** — it tells
   us whether 512 KB is realistic before anything is designed around it.
2. **CD as root.** An `mke2fs -b 2048` image on the disc, `root=/dev/psxcd`.
   Implement the **ISO9660 lookup** (`docs/24` §5.5 option B, ~60 lines) rather
   than hardcoding the image's LBA — otherwise every disc rebuild needs a
   matching kernel rebuild, and disc contents will be iterated constantly.
3. **Grow `BLACKROO_USER_RESERVE`** with whatever 1 and 2 freed.
4. **Grow `brsh`** into the monolithic shell: `ls`, `cat`, `cd`, `cp`, `mv`,
   `rm`, `mkdir`, `hexdump`. Freestanding, no toolchain work.
5. **Then fork the effort** — the uClibc toolchain for real BusyBox, or
   straight at the GPU desktop.

Steps 1 and 2 set the budget for everything after them, which is why they come
before the interesting part.
