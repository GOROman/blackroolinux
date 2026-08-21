# Next: a working root filesystem

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: BINFMT_FLAT, and -b 1024 for the CD image (psxcd needs -b 2048).
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> Written 2026-08-21 at the end of the session that got Linux booting from CD
> with 381 KB of memory card storage. This is the plan for the next one.

## The goal

Boot to a **real userspace shell** — not the in-kernel monitor — with a root
filesystem the system can read and write.

## The one thing that actually blocks it

A root filesystem is useless without something the kernel can `execve()`, and
that is where this project has been stuck since 0.2:

- `CONFIG_BINFMT_FLAT=y` and **no ELF loader is built** (`fs/binfmt_elf.o` does
  not exist in this tree).
- Every BusyBox in the tree links at **`0x00400000`** — 4 MB. The target has
  **2 MB**, and the PS1's R3000A has no MMU to map it elsewhere.
- So `execve()` cannot succeed, and `mount_root()` succeeding just moves the
  panic one step later: "No init found".

**Mount first, exec second is the wrong order.** Get one binary running, then
give it a filesystem to live on.

## Phase A — one executable binary (the real work)

Three routes, in order of how likely they are to work quickly:

### A1. Fixed-address ELF loader (recommended)

Add a small `binfmt` that loads a statically linked ELF at the **fixed
address it was linked for**, with the kernel reserving that region at boot.
No relocation, no bFLT, no toolchain archaeology.

- ~150 lines: read `Elf32_Ehdr`, copy `PT_LOAD` segments to their `p_vaddr`,
  set up a stack, jump to `e_entry`.
- Link userspace at a KSEG0 address inside the 2 MB, e.g. `0x80160000`
  (the kernel currently ends at `0x80168ab0` — pick clear of it and reserve).
- One resident program at a time, which is fine for a shell.
- Toolchain: **`mipsel-none-elf-gcc` 12.3.0 is now installed** with PSn00bSDK
  (`~/projects/toolchains/psn00bsdk`). It has no Linux libc, but a shell that
  makes raw `syscall` calls needs none — see `tools/host/mkinit.c` for the
  syscall numbers already worked out (`write=4004`, `read=4003`,
  `execve=4011`, `exit=4001`).

### A2. bFLT with GOT relocation

The "correct" uClinux answer. Compile PIC so every absolute reference goes
through the GOT, then emit bFLT relocations pointing at the GOT slots.

Session experiments (2026-08-20) with the bundled EGCS 2.91.66:
- plain builds emit `R_MIPS_26` absolute call targets, which bFLT relocations
  **cannot** patch;
- `-membedded-pic` is overridden by the target's `-mabicalls` default and
  produces GOT/`$gp` code anyway.

So it is possible, but it is a research task, not an afternoon.

### A3. Rebuild BusyBox linked low

Needs BusyBox source plus a Linux-targeting MIPS toolchain (`gcc-mipsel-linux-gnu`
is installable without root via `apt-get download` + `dpkg-deb -x`, the same
trick used for cmake/ninja this session). Still lands on A1 or A2 for the
loader, so do it *after* one of those works.

**Start with A1 and a shell of our own**: read a line, split it, run a builtin.
`ls`, `cat`, `hexdump`, `mount`. Perhaps 300 lines, entirely under our control,
and it proves the whole path.

## Phase B — a filesystem to run it from

Two options, and the initrd is the easier first step:

### B1. initrd (RAM)

The machinery already works: the kernel finds the `INRD` magic after `_end`,
loads it into `/dev/ram0`, and mounts ext2 (this was verified in emulation
back in April). `tools/addpsexe_initrd` embeds it.

Budget: the kernel is 750 KB and free RAM is ~420 KB, so an initrd must be
small — 128-256 KB. Fine for a shell and a few utilities.

Build it without root using `mke2fs -d` (genext2fs is not installed here):

```bash
mke2fs -F -b 1024 -d rootdir -t ext2 -I 128 -r 0 initrd.img 256
debugfs -w -R "mknod /dev/console c 5 1" initrd.img
```

### B2. Memory cards (persistent)

`/dev/bul` is real now: **381 KB across three cards**, block device major 208,
reads verified through `bread()`. To use it as root:

1. Write an ext2 image onto the cards. `bu.c` reserves the first
   `BU_FIRST_BLOCKS` (8) blocks per card for the Blackroo header, and the
   joined device presents the rest linearly.
2. Boot with `root=/dev/bul`.
3. **Writes are unverified** — everything so far is reads plus the monitor's
   own sector writes. Test `card wr` before trusting it.

Cards are slow (~250 kHz clocked, 128-byte sectors), so expect a filesystem
that is usable, not brisk.

### B3. CD-ROM (the big one, later)

700 MB the console already reads, versus 420 KB of RAM. No ISO9660 driver is
needed: put a plain ext2 image on the disc as a file, have a CD block driver
expose it, and mount that. The tree has **no CD driver and no `fs/isofs`** —
both were stripped — so this is a driver project, and the right one to do once
userspace works.

## Suggested order for the next session

1. **A1**: `binfmt_fixed` + a hello-world binary at a fixed address. Success is
   a `printk` from the kernel saying it jumped, and output from the program.
2. Grow that into a small shell (read/parse/builtins).
3. **B1**: put it in a 256 KB initrd, boot `root=/dev/ram0 init=/bin/sh`, and
   drop the `brmon` cmdline so the kernel boots to userspace on its own.
4. **B2**: ext2 on `/dev/bul`, verify writes, then `root=/dev/bul`.

Milestone: **a shell prompt that is a process, not the monitor.**

## What is already in place

| | |
|---|---|
| Boot | CD-R → kloader → kernel at `0x80090000`, no host PC |
| Iteration | build → 72 s serial upload → boot with custom cmdline |
| Monitor | BRMON: `peek/poke/md/ram/hw/cpu/mem/card/sio0/tty` |
| Storage | `/dev/bul` 381 KB, 3 cards, reads via `bread()` |
| Builds | kernel (EGCS via `sdk/toolchain-local`), kloader (PSn00bSDK native) |
| Panic | drops into the monitor instead of hanging |

## Traps to remember

- `include/linux/autoconf.h` is generated by `build.sh` from `.config`; a
  config change wipes all objects (GR-001, GR-002).
- Discs must have their licence area transplanted or the console will not boot
  them (GR-005) — `iso/build-iso.sh` does it.
- The kernel must not be linked where kloader or its heap lives (GR-008);
  `0x80090000` is clear.
- Card timing is real: 2 ms settle after `/JOYn`. If something only works with
  `DEBUG` on, it is a delay you are missing.
