# Next stage — options, with what each actually costs

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: a plan superseded by 29-LINEAGE-AND-ROADMAP.md.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> Written 2026-08-21, at the point where a userspace shell runs as pid 1 on
> real hardware. Pick one; they are largely independent.

## Where we are

Boots from CD-R with no host PC, mounts an ext2 root from an initrd, and runs
`brsh` as pid 1 with an interactive prompt over serial. The kernel multitasks
normally (scheduler, kswapd, preemption). Three memory cards are joined as
`/dev/bul` (381 KB). Iteration is ~90 seconds: build, upload, boot.

---

## A. CD-ROM block driver — the biggest unlock

**Why:** 700 MB the console already reads reliably, against ~420 KB of RAM.
It turns storage from a constraint into a non-issue, and makes a real
distribution possible: kernel plus a filesystem on one disc.

**Ready to start:** `docs/24-CDROM-DRIVER-RESEARCH.md` (1790 lines) has the
register map, command protocol, a worked "read sector N", interrupt handling,
DMA-vs-PIO tradeoffs, a Linux 2.4 block-layer design, twenty pitfalls, and a
bring-up order. Every claim is tagged by source; 14 open questions are marked
VERIFY.

**Notable findings from that research:**
- No Linux or BSD PS1 port has ever had a working CD driver. This would be new.
- The best model is PCSX-Redux's bare-metal `src/mips/shell/cdrom.c` (MIT,
  hardware-tested, ~350 lines).
- `hardsect_size = 2048` forces `mke2fs -b 2048`; ext2 rejects a filesystem
  blocksize below the hardware sector size.
- CD is the **lowest-priority IRQ** in this tree's handler, competing with a
  115200-baud serial console for a 6.6 ms/sector budget.
- `do_IRQ` already acks `I_STAT` before dispatch — the handler must not ack
  again.
- Use KSEG1 for the registers: the PS1 has no writeback D-cache, but it does
  have a CPU write queue.

**Shape:** put a plain ext2 image on the disc as a file, expose the disc as a
block device, mount that. No ISO9660 driver needed (and `fs/isofs` is not in
the tree).

**Effort:** the largest item here, and the one with the most hardware-timing
risk — the same class of trouble SIO0 gave us. Budget several sessions.

---

## B. More than one program at a time

**Why:** `binfmt_fixed` loads every binary at the same fixed address, so a
second program cannot be resident. The kernel is perfectly capable of running
several processes; the loader is what limits it.

**Two routes:**

1. **Fixed slots** — a handful of load addresses (say 4 × 16 KB), allocated per
   exec. Crude, ELKS-flavoured, and probably enough for a shell plus a utility.
   A day's work, no toolchain risk.
2. **bFLT properly** — relocatable binaries, the "correct" uClinux answer, and
   the only route to arbitrary process counts. Earlier experiments: plain EGCS
   builds emit `R_MIPS_26` absolute call targets that bFLT relocations cannot
   patch, and `-membedded-pic` is overridden by `-mabicalls`. Would want the
   modern `mipsel-none-elf` toolchain and a custom elf2flt.

**Prerequisite either way:** `fork()` on no-MMU has vfork semantics — the child
shares the parent's memory until exec. Worth checking what this tree actually
does before relying on it.

---

## C. Make the tty layer work — **DONE 2026-08-21 (output side)**

The diagnosis recorded here was wrong, and the fix was one string. `/dev/console`
was not hanging because `serial_psx.c` lacked a transmit path; it was hanging
because it *resolved to the serial tty at all*. `vt_console_driver` in
`drivers/char/console.c` had been renamed `name: "ttyS"`, so `console=tty0`
matched nothing, no console took `CON_CONSDEV`, and `tty_open()` handed
`/dev/console` to ttyS0. Renamed to `"tty"`, it resolves to tty 4:1 — the VT —
and userspace output is drawn on the television by `psxvga_con`.
`blackroo_con_mirror()` in `do_con_write()` copies the same bytes out of SIO1,
so a process reaches the TV and the serial link together. Neither
interrupt-driven TX nor buffering was needed; `serial_psx.c` was not touched.
See GR-011 and `docs/captures/2026-08-21-tty-layer-gpu-output.txt`.

**What is left of C is the input side:** `/dev/console` has no keyboard behind
it, so `brsh` still reads `/dev/brcon` over serial. That is really item F.

---

## D. Root filesystem on the memory cards

**Why:** persistence. 381 KB is small but real, and it is already a working
block device.

**Correction (2026-08-21):** the "blocker" recorded here — that `root=/dev/bul`
does not resolve — is **not real**. `init/main.c` already carries
`{ "bul", (BU_LARGE_MAJOR << MINORBITS) }` under `CONFIG_PSX_MEM_CARD` /
`CONFIG_PSX_LARGE_CARD`, and both are set in `autoconf.h`. Start at the next
item instead:

- verify **card writes** through the driver (only reads are proven — the
  monitor's own sector writes are separate code)
- put an ext2 image on the joined device
- boot `root=/dev/bul`

---

## E. Utilities worth having

The shell has builtins only. Small, self-contained programs that would earn
their keep on this machine:

- `hexdump` / `peek` / `poke` — the monitor's abilities, from userspace
- `cat`, `ls` — needs `open`/`read`/`getdents` in brsh
- a memory-card tool — read/write blocks of `/dev/bul` from a process

Each is tens of lines against the raw syscall interface, and each also tests a
different part of the kernel from userspace, which has value of its own.

---

## F. A keyboard — the input half of C, and the end of the host PC

Output now reaches the TV, so the only reason a host PC is still attached is
that nothing can type. The VT is already wired to the GPU; a keyboard driver
only has to call `handle_scancode()` and input arrives at `/dev/console`, at
which point `brsh`'s `infd` moves off `/dev/brcon` and the console is
self-contained. `arch/mipsnommu/ps/kbd-no.c` is what the tree builds today;
`psx_kbd.c` exists but its `kbd_init_hw()` is empty.

**The hardware is already here.** A Pi Pico is wired to the memory card bus,
and `docs/15` establishes why one device can be both storage and keyboard: the
card slot and the controller port share SIO0 and are selected by an **address
byte in the data stream** — `0x81` memory card, `0x01` controller/keyboard — so
Core 1 branches on the first byte the `cmd_reader` PIO captures. No extra state
machines. There is also a real PlayStation mouse on hand, which is device ID
`0x12` on the standard protocol and needs no Pico at all.

Protocol choice from `docs/14`: emulate **ID 0x96** (Lightspan), the documented
standard, PS/2 Scan Code Set 2. Do **not** copy the Spectrum adaptor v2 (ID
`0xE8`) — it is documented to answer *any* `0x01` byte on the bus, which would
collide with the memory cards.

Storage note for the same device: the card protocol addresses sectors with two
bytes, so **8 MB per slot** (65536 x 128 B) is the ceiling before the protocol
itself needs extending — 64x today's 128 KB. But SIO0 runs at ~250 kHz, roughly
31 KB/s at best, so the Pico is the *writable* storage answer and the CD driver
(~150 KB/s) is still the *throughput* one.

---

## G. Real programs — the current question

`brsh` has `help`, `echo` and `exit`. Getting past that runs into three
blockers, and it is worth being precise about which:

1. **One load address.** `binfmt_fixed` puts every binary at `0x001f0000` in a
   64 KB window, so no second program can be resident — no pipes, no launching
   commands.
2. **No libc for the target.** EGCS 2.91 builds the kernel; `mipsel-none-elf-gcc`
   12.3 builds freestanding code. Neither can link an ordinary C program.
3. **~1 MB free** on a 2 MB console.

**Size is not a blocker**, which is the encouraging part.
`tools/busybox-mips-uclibc` is a static, stripped, little-endian **MIPS R3000**
BusyBox whose two PT_LOADs come to a **255 KB** resident footprint. That fits.
It is linked at `0x400000` — 4 MB — which does not exist here, and cannot be
aliased there either: with 2 MB the PS1 mirrors `0x400000` onto `0x000000`,
which is where the kernel lives.

Three routes:

- **A — freestanding tools.** `ls`, `cat`, `hexdump` against raw syscalls, the
  way `brsh` was built. Tens of lines each; as builtins they need nothing else.
  Cheapest, works today, no new toolchain.
- **B — rebuild BusyBox linked low.** The real "bash-like" answer, and the
  toolchain is the catch: prebuilt mipsel uClibc toolchains target **mips32r2,
  which an R3000 cannot execute**, so this means crosstool-NG or Buildroot
  configured for **MIPS I**. Then BusyBox no-MMU with `FEATURE_SH_STANDALONE`
  and NOFORK/NOEXEC applets — most commands run in-process, which sidesteps
  blocker 1 for a long time — plus growing `BLACKROO_USER_RESERVE` from 64 KB
  to ~320 KB.
- **C — the 8 MB RAM mod.** Makes `0x400000` a real address, so the existing
  255 KB binary could load nearly as-is with `binfmt_fixed` retargeted, and
  removes the RAM pressure for good. Smallest software effort; hardware work.

---

## Suggested order

1. **F, a keyboard** — the output half of the console is done, and this is what
   unplugs the host PC. **Started 2026-08-21 via BlueRetro; see
   `docs/27-KEYBOARD-BRINGUP.md`.**
2. **D's card writes** — `root_dev_names[]` turned out to need no fix; what is
   actually unproven is the write path.
3. **B1 fixed slots** — cheap, and makes the shell able to launch anything.
4. **A, the CD driver** — the real prize, with research already done.

## Reminders for whoever picks this up

- Syscalls here: `exit 1`, `read 3`, `write 4` (`__NR_Linux = 0`).
  `tools/host/mkinit.c` still carries the wrong 4000-based numbers.
- Userspace runs in **user mode, in KUSEG**. Kernel mode breaks `current`.
- Header edits do not trigger rebuilds — delete the `.o` files.
- Verify, do not report: three separate stages in the initrd work claimed
  success while doing nothing.
