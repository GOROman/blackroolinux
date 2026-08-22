# Current blockers, and notes on 16 MB hardware

> Written 2026-08-27, at the end of the run that got memory cards working.
> Everything here is either measured on a real console or explicitly marked
> as unverified.

---

## Memory cards — what it took, and what is left

The cards went from "reads deadlock" to "formatted with ext2 by the console
itself" in one session. **Five separate bugs**, all in `drivers/block/bu.c`,
none of them the one that looked most likely at the time:

| | |
|---|---|
| Request dropped when the bus was busy | `do_bu_request()` returned on `bu_lock` without completing or re-queueing. `psxkbd` polls the same SIO0 bus 100 times a second, so a request arriving during a keyboard poll was abandoned and `bread()` waited forever. |
| Timeout that woke nobody | `bu_timeout()` is the only rescue for a missed acknowledge interrupt, and two of its three exit paths returned without setting `bu_state` or calling `wake_up()`. |
| Unbounded sleep | `bu_ready()` used `sleep_on()`. A missed wakeup should cost a timeout, not the machine. |
| **The actual cause**: revalidate used a running counter | `bu_revalidate()` runs the same sweep as `bu_init()` and never received the fix `bu_init()` got in August — judge each card on its own recorded number, not on how many have been found so far. It also lacked the settle delay and the `bu_probing` bus lock. |
| Wrong ioctl number in userspace | `BLKGETSIZE` is **`0x20001260`** on this kernel, not the generic `0x1260`: MIPS defines `_IOC_NONE` as 1 and shifts it to bit 29. With the wrong number the call returns *without writing anything* and the size stays 0, so `mkfs` reported "device too small" on a perfectly good device. |

### The hardware fact underneath all of it

**An empty multitap slot does not read as empty. It returns a neighbouring
card's first block, magic and all.**

Slot 1 — port 1 tap A, where a DualShock lives and no card does — read as a
valid 127 KB card on every sweep. Checking the card *number* cannot catch that,
because the phantom carries a perfectly valid number that belongs to another
card. Checking the **serial** can: each card gets a unique one when formatted,
so a repeat proves the slot is echoing rather than holding.

This wasted an evening and cost three cards' contents to a reformat that turned
out to be unnecessary — the numbering was never wrong.

### Still open — THE remaining blocker

**A userspace `mount(2)` of `/dev/bul` hangs the machine. The kernel mounting
the same device as root does not.**

Measured 2026-08-27, on the same cards, minutes apart:

```
root=/dev/bul  ->  VFS: Mounted root (ext2 filesystem) readonly.     WORKS
mount -r /dev/bul /mnt/mcdrive from brsh  ->  silence, forever       HANGS
```

Not a panic: a panic drops into BRMON, which answers serial, and it does not.
So it is stuck in a loop with interrupts still live - the keyboard poll keeps
running - rather than crashed.

Ruled out by measurement, not reasoning:

- **Not the write path.** `-r` mounts read-only and still hangs. (An earlier
  read-write attempt also hung; the read-write theory was wrong.)
- **Not the filesystem.** The kernel mounts it successfully. `mkfs` output
  verified against `e2fsck` on the host before it was ever run.
- **Not scattered reads.** The kernel's mount reads the same superblock, group
  descriptor, inode table and root directory.
- **Not sys5.** `arch/mipsnommu/kernel/scall_o32.S` `stackargs:` reads
  argument 5 with `lw t1, 16(t0)` from the saved user sp, which is exactly
  where brsh's stub puts it.

What differs between the two, and is worth instrumenting next:

1. The root mount runs from `mount_root()` before userspace exists; the brsh
   one runs from `sys_mount()` with a ramdisk root already mounted and the
   buffer cache holding it.
2. `psxkbd` has locked onto a keyboard by the time brsh runs and is polling
   SIO0 at 100 Hz, taking `bu_lock` each time. `do_bu_request()` defers on a
   busy bus and re-arms a one-jiffy timer; if the poll wins consistently the
   retry could starve. At boot the keyboard has not locked on yet.
3. A second mounted filesystem on a 2 MB machine is real buffer pressure.

**Next step: put a printk in `do_bu_request()`'s deferred branch and count
retries.** If it climbs without bound, it is (2) and the fix is for `psxkbd`
to back off while a block request is outstanding. That is one boot to find out,
and no more guessing.

- **Writing a file** through a mounted filesystem remains unproven, blocked
  behind the above. `mkfs` writes 18 metadata blocks successfully, so the write
  path itself works; a file write additionally exercises indirect blocks and
  the inode allocator.
- **Card writes are slow.** ~31 KB/s by protocol. A 381 KB volume takes about
  12 seconds to fill.
- **`/dev/bul` is the joined set only.** `bu0..bu3` (major 207) are registered
  but individual-card access has never been exercised.

---

## Blockers, in the order they block things

1. **One binary does not serve all three memory sizes.** The userspace window
   sits at the *top* of RAM, so both its size and its address move with the
   machine: `0x001d0000` on 2 MB, `0x00700000` on 8 MB, `0x00f00000` on 16 MB.
   `binfmt_fixed` loads a program at the address baked into its ELF, and no
   runtime probe can change an address that is already in the file.

   **The fix:** put the window at a *fixed low address* just above the kernel
   and let its *size* be decided at runtime from detected RAM. Base identical
   everywhere, size scales, and `CONFIG_PSX_RAM_AUTO` becomes the only RAM
   setting. Costs a `reserve_bootmem()` hole instead of under-reporting the
   size. This is the single change that collapses three builds into one.

2. **`output/` is shared across variants.** Building a different release leaves
   binaries behind that the next `convert` will bundle. This actually happened:
   a 2 MB kernel shipped with a 16 MB `brsh` and `binfmt_fixed` refused it -
   *"segment 0 is 0xf00000, outside the reserved window 0x001d0000..0x001fd000"*.
   The validation earned its place, but the release script should build each
   variant into its own tree.

3. **The initrd is paid for twice at boot.** Once in the PS-EXE the loader put
   in RAM, again in the ramdisk `rd_load` copies it into. 112 KB became ~224 KB
   of peak footprint and produced *"Out of memory and no killable processes"* on
   a 2 MB machine. Keep it small, or boot from the CD root, which is served by
   the buffer cache and elastic.

4. **`brsh` cannot be driven over serial.** Input comes from `/dev/console`
   (the keyboard) only, so every test has to be typed on the console itself.
   Mitigated by running a command from the kernel command line
   (`init=/bin/sh mkfs /dev/bul yes`), but the real fix is `sys_select(142)` on
   both `/dev/brcon` and `/dev/console`.

5. **CD-ROM seeks cost ~1.3 s.** The controller ignores commands for about a
   second after a `Pause`, and stage 1 is synchronous. `docs/24` §5.6's
   interrupt-driven machine never pauses in steady state.

---

## 16 MB hardware — "Big Skippah", and why it is coming soon

**The `RAM_SIZE` register has no published 16 MB encoding.** It documents three
values, and that is all:

```
2 MB  0x0888      4 MB  0x0988      8 MB  0x0B88
```

So the 16 MB variant is built with `CONFIG_PSX_RAM_AUTO` - the runtime probe -
rather than being told a number that might be a lie. A kernel that believes in
memory the hardware does not have does not fail loudly; it corrupts (GR-004).

It also needs `CONFIG_PSX_16MB_RAM=y` *alongside* the probe, and this is worth
understanding because it is not obvious:

> The probe decides what the memory **controller** is told.
> The size constant decides where a **binary was linked**.
> These are different questions, and a runtime probe cannot reach an address
> that is already in the ELF.

A config setting only `RAM_AUTO` silently places the window at the top of 2 MB.
That produced a "16 MB" release which was really a 2 MB layout with 1 MB carved
out of it - on a stock console it would have swallowed half the RAM.

### What exists on the hardware side

`docs/01` §"16 MB dual-bank mod" records the parts research: eight
`M5M4V17805CJ-6` or `KM48V2104AJ-6` chips, 2M x 8, 16 Mbit, **3.3 V**, 2048
refresh, SOJ-28. Eight chips is four for an 8 MB mod plus spares, **or all eight
for the dual-bank 16 MB mod**. The Kingston `KTM2MX72S` module is a confirmed
donor.

**The trap that already cost a rework:** `M5M417805CJ-6` without the `V` is the
**5 V** part and gives no video. `V` = 3.3 V, `C`/no-V = 5 V.

### What is genuinely unknown

- Whether the memory controller can be made to address 16 MB at all
- What `RAM_SIZE` value would do it
- Whether the probe in `prom/memory.c` detects it correctly - it is already
  noted as having mis-read as 1 MB under emulation
- Whether the address decoding on a dual-bank mod mirrors or extends

**Until a 16 MB machine exists to measure, "Big Skippah" builds and is shipped
as coming soon.** It is not a release; it is a placeholder with the plumbing
already correct so that the day the hardware exists, only the register value is
missing.

8 MB - "Blackbelt" - has no such doubt. The mod is well documented, the register
value is known, and the build is correct. It has not been run on real 8 MB
hardware because neither console here has the mod fitted.
