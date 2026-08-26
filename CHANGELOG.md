# Blackroo Linux Changelog

All notable changes to this project are documented here. Each entry describes what changed, why, and where the work originated.

This project follows a principle of **full traceability** — every piece of code, every design decision, and every external reference is documented so that anyone can understand how we got from a dead 2007 project to a working Linux distribution on the PlayStation 1.

---

## [Unreleased] - Active Development

### 2026-08-27 - **Memory cards: formatted and mounted by the console itself**

The cards went from "every access deadlocks" to "the PlayStation formats them
and the kernel mounts them" in one run. Everything below is measured on a real
PAL SCPH-750x unless it says otherwise.

```
mkfs /dev/bul: 381 blocks of 1024, 96 inodes, 12 inode-table blocks
  done. 363 blocks free.

VFS: Mounted root (ext2 filesystem) readonly.
```

**Six bugs, all in the storage path, and the obvious suspect was wrong every
time.**

| | |
|---|---|
| Request dropped on a busy bus | `do_bu_request()` returned on `bu_lock` without completing or re-queueing. `psxkbd` polls the same SIO0 bus 100 times a second, so a request arriving during a poll was abandoned and `bread()` waited forever. |
| Timeout that woke nobody | `bu_timeout()` is the only rescue for a missed acknowledge interrupt, and two of its three exits returned without setting `bu_state` or calling `wake_up()`. |
| Unbounded sleep | `bu_ready()` used `sleep_on()`. A missed wakeup should cost a timeout, not the machine. |
| **The cause**: revalidate used a running counter | `bu_revalidate()` runs the same sweep as `bu_init()` and never got the fix `bu_init()` received in August - judge each card on its own recorded number, not on how many have been found. It also lacked the settle delay and the `bu_probing` bus lock. |
| Phantom slots | An empty multitap slot returns a NEIGHBOUR'S first block, magic and all. Slot 1 - a DualShock, no card - read as a valid 127 KB card every sweep. Card numbers cannot catch that; **serials can**, since each card gets a unique one at format time. |
| `BLKGETSIZE` was the wrong number | **`0x20001260` on this kernel, not the generic `0x1260`** - MIPS defines `_IOC_NONE` as 1 and shifts it to bit 29. The wrong number returns *without writing anything*, so the size stayed 0 and `mkfs` reported "device too small" on a perfectly good device. |

**`mkfs` was verified before it touched hardware.** The same layout was
generated on the host and run through `e2fsck`, which caught two real bugs a
console test would have hidden: the block-bitmap padding was off by one (the
bitmap covers blocks 1..blocks-1, so padding starts at bit blocks-1), and
`s_blocks_per_group` must be **8192** - one bitmap block's worth of bits - not
the actual block count. Diffed byte for byte against a real `mke2fs` image of
the same geometry.

**Still broken, and now precisely stated.** A userspace `mount(2)` of
`/dev/bul` hangs where the kernel's own root mount succeeds - same cards, same
read-only flag, minutes apart. Ruled out **by measurement, not argument**: the
write path (read-only hangs too), the filesystem (the kernel mounts it),
scattered reads, the 5-argument syscall stub (checked against
`scall_o32.S`'s `stackargs:`), and bus contention (a deferral counter in the
driver records none). `docs/30-BLOCKERS-AND-HARDWARE-NOTES.md` carries the
eliminations and what to instrument next.

So **the root filesystem stays on the CD**, which is the design in `docs/28`
anyway. Cards are manual, experimental storage for now.

**Three release variants**, built by `scripts/make-release.sh`:

| | | |
|---|---|---|
| 2mb | **Little Joey** | a stock console. The one to burn. |
| 8mb | **Blackbelt** | the 4-chip mod, 1 MB userspace window. Builds; no 8 MB hardware here to run it on. |
| 16mb | **Big Skippah** | **coming soon.** The `RAM_SIZE` register documents 2/4/8 MB only and there is no published 16 MB encoding, so it uses the runtime probe rather than a guessed value. |

Each builds into its own `output/release-<size>/` because all three otherwise
produce a file called `blackroo.exe`. That is not hypothetical: a 2 MB kernel
shipped with a 16 MB `brsh` during this session and `binfmt_fixed` caught it -
*"segment 0 is 0xf00000, outside the reserved window 0x001d0000..0x001fd000"*.

**The design weakness worth naming.** The userspace window sits at the TOP of
RAM, so its size AND its address move with the machine. `binfmt_fixed` loads a
program at the address baked into its ELF, and no runtime probe can change an
address that is already in the file - so one binary cannot serve all three
sizes. Putting the window at a fixed LOW address with a runtime size collapses
three builds into one, and is the single most valuable change left.

**New in userspace:** `mkfs`, `lshw` (hardware read from the registers, since
there is no PCI to enumerate), `peek`/`poke`, `ps`/`top` via a new
`sys_blackroo_tasks(217)` syscall that costs 484 bytes against `/proc`'s 34 KB,
`df`, `edit`, `mount`/`umount`, and `fetch` with a kangaroo. brsh also runs a
command handed to it on the kernel command line, which is how these tests were
driven without typing on the console.

**Kernel fixes:** `si_meminfo()` reported `totalram_pages` where this port needs
`num_physpages` - free memory exceeded total, and `used = total - free`
underflowed to 4 GB. The boot banner claimed "MIPS MMU kernel" on a kernel whose
architecture is the absence of one. The hostname was `(none)` because nothing
here ever calls `sethostname()`.


### 2026-08-26 - **0.5.0 "Rootstock26" - the disc carries its own root filesystem**

Burned, and **only half tested**. The disc boots and the kernel runs; whether
it mounts its root filesystem off the CD is the open question at the end of
this entry. Everything below that is not marked otherwise was verified on the
host, not on the console.

**The point of the release.** `drivers/block/psxcd.c` now finds the root
filesystem itself: read the volume descriptor at LBA 16, walk the ISO9660 root
directory, match a filename, take the extent. `docs/24` §5.5 option B, about
sixty lines as predicted. The LBA is no longer baked into the kernel, so **the
disc can be rebuilt without rebuilding Linux** - which matters now that disc
contents change every iteration.

`psxcd_base=` still overrides it, `psxcd_file=` names the image (default
`ROOT.IMG`), and `psxcd_probe=0/1` forces the probe off or on.

**Proven on the host before burning anything.** `tools/host/iso-find.py` runs
the *same* algorithm - same offsets, same both-endian handling, same `;1`
stripping - against the `.bin`:

```
  root directory: LBA 22, 2048 bytes
    ROOT.IMG;1       LBA    827    4194304 bytes
  psxcd would report: image at LBA 827, 4096 KB
  ext2 magic 0xef53 OK, block size 2048 OK
  -> looks mountable
```

Negative-tested too: splice a `-b 1024` image in at LBA 827 and it correctly
refuses, because this tree's `fs/ext2/super.c` rejects a blocksize below the
2048-byte hardware sector (`docs/24` §5.3).

**The probe only runs when the disc is the root device.** Every loop in the
lookup is bounded, but "bounded" is a claim about code, and a wrong claim here
means a console that hangs before userspace on *every* boot from the disc -
including the ramdisk boot that has nothing to do with the CD. So
`psxcd_init()` checks `MAJOR(ROOT_DEV)` first. A `root=/dev/ram0` boot does not
touch the drive.

**When the file is not found, it prints the directory.** "not found" cannot
distinguish a disc without the file from a walk reading rubbish, and those need
opposite fixes. It now lists every name, type, LBA and size. All `__init`.

**kloader's CD menu carries its own settings.** Booting from the disc needs
`root=`, `init=` and `console=` to agree with *which* kernel is booting, and
getting it wrong fails quietly - pick `KERNEL.EXE` with `root=/dev/psxcd` and
its built-in ramdisk simply wins. Each menu entry now hard-codes the one
known-good combination (`CDBOOT_PROFILE_*` in `bootloader/src/kernel.h`) and
**System Settings is not consulted**. The command line is printed on screen
before launch, because a boot that goes wrong from there is otherwise
undiagnosable from the television. A custom cmdline still overrides, and says
so on screen when it does.

`ROOT_DEV_CDROM` is a new root option, and the settings menu wraps on
`ROOT_DEV_COUNT` rather than a hardcoded 3.

**GPL v2 compliance: the source is ON the disc.**

```
  SOURCE.TGZ;1     LBA   2875    7620920 bytes
  COPYING.TXT;1    LBA   6597      18633 bytes
  README.TXT;1     LBA   6607       4568 bytes
```

Section 3 allows shipping the source or promising it later. Promises are harder
to keep than facts and the disc had 675 MB spare, so it ships the source.
`scripts/make-source-dist.sh` builds it; verified by extracting `SOURCE.TGZ`
**back out of the disc image** (byte-identical, sha1 `7febf8e0...`) and building
a working kernel from it.

The archive **refuses to ship if it contains a compiled binary**. That gate
earned itself immediately: the first run leaked a third-party BusyBox binary
(a GPL obligation this project has no reason to take on), `mkmemcard`, and nine
kernel host tools, because an anchored `./build` exclude does not match
`bootloader/build`. `.depend` files *are* shipped - they look generated, but
this tree had the unused driver subdirectories deleted and 2.4's `make dep`
dies on `acpi: No such file or directory` without them.

`bootstrap.sh` is the "one command": checks the host, prints the exact apt line
for anything missing, obtains the toolchain, builds kernel -> root fs -> initrd
-> PS-EXE -> bootloader -> disc images. Never runs as root.

**A licensing error, corrected.** `SOURCE-ATTRIBUTION.md` said PSn00bSDK was
**MIT**. It is **MPL 2.0** - checked against the SDK's own
`share/psn00bsdk/doc/LICENSE.md`. kloader links it and ships on the disc, so
this mattered. We do not modify the SDK, so §3.2 is satisfied by telling
recipients where to get it, which `iso/README.TXT` now does. Also recorded
there: `mkpsxiso` is GPL v2-or-later (used to build, never shipped), and the
EGCS toolchain is deliberately **off** the disc, because shipping GCC binaries
would oblige this project to supply GCC's source. PSn00bSDK takes the same
position about its own bundled toolchain.

---

**THE BUG THAT NEARLY ATE THE NIGHT.** Every kernel built on 2026-08-25 linked
at `0x80010000` - straight into kloader's own text, the failure GR-008 exists
to prevent. The 2026-08-21 relink to `0x80090000` had been made by editing
`arch/mipsnommu/ld.script`, which is **generated** from `ld.script.in` and
**deleted by `make clean`**. `LOADADDR` in `arch/mipsnommu/Makefile` still said
`0x10000`, so the first clean of the size pass silently undid it, four days
later. Found only because a failed upload prompted reading the PS-EXE header.
`LOADADDR` now lives where `clean` cannot reach it, `iso/build-iso.sh` prints
the load address of everything it packs, and this is GR-023.

**Two more, both mine.** `blackroo-serial.py`'s `upload` discarded its return
value and exited 0 after failing, so a watcher reported "UPLOADED" for an
upload that never happened (GR-024). And a source archive built in a scratch
directory came out 31,232 bytes larger than the same source in this tree, which
looked like a GPL-correspondence defect and was not: GCC embeds `__FILE__` as
an absolute path, there are 485 of them in the image, and the scratch path was
74 characters deeper (GR-025).

**New tools.** `scripts/size-report.sh`, `scripts/make-cdroot.sh`,
`scripts/make-source-dist.sh`, `scripts/serial-autoupload.sh` (retries until
kloader beacons, so nobody has to coordinate timing), `scripts/serial-capture.sh`,
and `tools/host/iso-find.py`.

**Hardware status, honestly.** The 0.5.0 disc burned clean (10,875 blocks, no
underruns) and the pre-burn checks all passed: licence area identical to
`license-area.bin`, both kernels at `0x80090000`/`sp 0x801fff00`, ROOT.IMG
mountable. A serial upload of the fixed kernel ran at the documented
10.4 KB/s with no retries, which is `0x80090000` working. Then the console
left BRMON on `cont` and went silent - no further output, no response, and not
back in the monitor (a panic returns there). **Unresolved.** The next session
starts by finding out whether that is a hang or a shell running happily on a
television nobody was watching.


### 2026-08-25 - **96 KB out of the kernel, and a shell that can read the disc**

Steps 1, 3 and 4 of `docs/28`. **None of it has run on hardware yet** - it
builds, it links, the numbers below are measured, and the machine has not seen
it. See "How to test this" at the end.

```
text     742,432  ->  646,496     -95,936   (-12.9%)
bss      132,672  ->  115,632     -17,040
PS-EXE   956,416  ->  874,496     -81,920   (~8 s off every serial upload)
```

Per-step measurements, each a full clean rebuild, are in
`logs/sizepass/SUMMARY.md`; `./scripts/size-report.sh` reproduces any of them.

**`docs/28`'s first lever is closed, and it was the wrong one.** That document
put compiler flags first and estimated `-Os` at "5-15% of text - 35-100 KB -
for a one-line change", with a note to test it before assuming. Tested:

| | text |
|---|---:|
| `-O1` | 765,824 |
| `-O2` | 742,432 |
| `-Os` | 740,032 |

**2,400 bytes. 0.32%.** EGCS 2.91.66 is not ignoring the flag - its `cc1`
carries `optimize_size` - it simply predates the work that made `-Os` mean
something, and `-O1` is 23 KB *worse* than `-O2`. `-Os` is kept because it is
free, and `BLACKROO_OPT` in the top Makefile makes the level a knob so the next
person can re-measure in one line instead of editing `CFLAGS` by hand.

So the whole 96 KB came from lever 2, not linking what cannot be used:

- **`CONFIG_PROC_FS` off - 34,052.** Nothing on this machine reads `/proc`.
- **`net/` not linked at all - 30,200.** `CONFIG_NET` was already off, and
  2.4 was building and linking `net/socket.o` plus `net/core` anyway, because
  stock Linux assumes `socket(2)` must exist even with no network device. On
  a 2 MB machine that is 6% of the kernel for an API nothing can reach. The
  three symbols the rest of the kernel calls unconditionally - `sock_init`,
  `sock_fcntl`, `sys_socketcall` - are answered in a new `kernel/nonet.c` the
  way a kernel with no protocol families registered would answer them.
- **pty, `/dev/raw`, the major-10 misc registry, the entropy pool - 17,168.**
  All four were `obj-y` in 2.4 with no config option; each now has one.
  `/dev/random` and `/dev/urandom` **do not exist** on this kernel rather than
  being stubbed, because a predictable `/dev/random` is worse than an absent
  one. The entropy *contributors* (`add_interrupt_randomness` and friends, on
  hot paths in `irq.c`, `bu.c` and `keyboard.c`) became empty inlines in
  `<linux/random.h>`; the *consumers* were deliberately left undeclared so
  anything that wants random bytes fails to link instead of quietly getting
  predictable ones.
- **`kmod` and `binfmt_flat` - 7,616.** No `/sbin/modprobe` exists to call,
  and userspace is ELF through `binfmt_fixed`, never bFLT.
- **`/dev/vcs` - 4,500.**

What was deliberately *not* removed, with measured costs and reasons, is in
`logs/sizepass/SUMMARY.md`. Two worth repeating: `mmnommu/vmscan.o` stays
because `docs/28`'s whole argument for a CD root is that the buffer cache is
elastic, and vmscan is the code that shrinks it; and `brmon.o` stays because
27 KB is a fair price for the only debugger this machine has.

**A bug found on the way, in `<linux/blk.h>`.** It re-declared
`add_blkdev_randomness()` itself, a few lines after the header that properly
declares it. Harmless while `random.o` was always linked; the moment the
declaration had to become an inline, that second plain declaration silently
defeated it and only `bu.c` failed to link. `blk.h` now includes
`<linux/random.h>` like it should have.

**The userspace window: 64 KB -> 192 KB**, and it is one number now. It had
been three - `fs/binfmt_fixed.c` and `arch/mipsnommu/ps/prom/memory.c` each
carried a `#define` with a comment asking the reader to keep them in step by
hand, and `userland/blackroo.ld` carried a third copy that nobody mentioned.
`userland/build.sh`'s header comment claimed `0x801c0000, 256 KB` while the
code used `0x001f0000, 64 KB` - drift that had already happened and that
nothing caught. Now: `asm/blackroo-user.h` derives everything from
`CONFIG_BLACKROO_USER_RESERVE_KB`, and `userland/build.sh` refuses to build if
`blackroo.ld` disagrees with the defconfig.

Userspace links at **`0x001d0000`** now, not `0x001f0000`. Every userspace
binary must be rebuilt.

**`brsh` grew from a demo into the shell.** 7 KB of `help`/`echo`/`exit` became
16 KB with the file commands built in:

```
/ $ ls -l /etc
-rw-rw-r--      218 motd
-rw-rw-r--       31 release
/ $ cat /etc/release
Blackroo Linux
release 0.5-dev
```

`ls` (with `-l`), `cat`, `hexdump`, `stat`, `cd` (including `..`), `pwd`,
`mkdir`, `rmdir`, `rm`, `cp`, `mv`, `echo`, `help`, `exit`. Still freestanding,
still raw syscalls, still no libc - `-Wall -Wextra` clean. The prompt carries
the working directory, because with no job control and no scrollback it is the
only place the user can see where they are.

This is `docs/28`'s System+Finder model taken literally: `binfmt_fixed` has a
single load address, so exactly one program image is resident at a time and an
external `/bin/ls` could not run *while the shell was running* even if it
existed. The shell **is** the utilities.

The syscall numbers were re-derived by counting `SYS()` entries from zero in
`arch/mipsnommu/kernel/syscalls.h` rather than trusting that file's numbered
comments, several of which are off by one - `sys_getitimer` carries a `/* 105 */`
comment at index 106. `stat` is 106 (`sys_newstat`), `getdents` 141,
`getcwd` 183.

**And a latent corruption in the initrd, which only became reachable today.**
`mke2fs` and `debugfs` stop writing at the last *used* block, so
`output/initrd.img` came out 42 KB for a filesystem whose superblock claimed
64 - `e2fsck -fn` says "the physical size of the device is 42 blocks ... likely
to be corrupt". That was harmless for as long as `brsh` had no way to write to
the disc. `mkdir` and `cp` can now allocate a block past the end of the image,
which after `rd_load` is past the end of the ramdisk device. The image is
padded to its declared size before it ships. The initrd also gained `/etc/motd`,
`/etc/release` and an empty `/tmp`, so the new commands have something to be
pointed at, plus `/dev/bul` and `/dev/psxcd` nodes.

**How to test this** (nothing below has been done):

1. `./build.sh kernel && ./scripts/make-userspace-initrd.sh && ./build.sh convert`
2. kloader CD in, `Serial Shell (115200)`, upload `output/blackroo.exe`, `cont`
3. At the prompt: `ls -l /`, `cat /etc/motd`, `cd /etc`, `pwd`, `cd ..`,
   `mkdir /tmp/x`, `cp /etc/release /tmp/x/r`, `cat /tmp/x/r`, `hexdump /etc/release`
4. **Check the boot line's free-memory figure** - it should be roughly 96 KB
   better than the 436k/236k recorded in `docs/28`, minus the 128 KB the
   window grew by. If it is not, the reserve arithmetic in `prom/memory.c` is
   wrong and that is the first thing to look at.
5. `mkdir /tmp/x` is the specific test for the initrd padding fix: before it,
   that could have written past the end of the ramdisk.


### 2026-08-25 - **The CD-ROM is a Linux block device**

```
blackroo> blk d1 0 10 800
bread(major 209, minor 0, block 16, 2048)...
0000: 01 43 44 30 30 31 01 00 50 4c 41 59 53 54 41 54  |.CD001..PLAYSTAT|
```

`drivers/block/psxcd.c`, major 209. That is a sector arriving through the
Linux block layer and buffer cache, from a disc, on a PlayStation. It comes up
by itself at boot: `psxcd: PlayStation CD-ROM, image at LBA 0, 16384 KB,
read-only`.

Stage 1 is deliberately the *provable* shape rather than the fast one:
synchronous, polled, one sector at a time from inside the request function -
the same path BRMON already proved, with a block device wrapped round it.
`docs/24` §5.6's interrupt-driven streaming machine is stage 2, and should not
be attempted until this mounts a filesystem.

The one thing that would make stage 1 unbearable is avoided: **`Pause` only on
a discontinuity**, never per request. A Pause costs ~32 ms at 2x and PSn00bSDK
reports the controller ignores commands for roughly a second afterwards, so
pausing per request would be slower than the drive itself. Sequential reads
keep the drive streaming and cost one INT1 and one DMA each.

`hardsect_size = 2048`, which forces `mke2fs -b 2048` on the disc image - ext2
refuses a filesystem whose blocksize is below the hardware sector size. Also
added: `PSXCD_MAJOR` 209, a `psxcd` entry in `root_dev_names[]` so
`root=/dev/psxcd` resolves, and `psxcd_base=` / `psxcd_size=` setup parameters.
BRMON's `blk` command now takes a blocksize, which is how the above was tested
without burning a disc.

**A correction worth recording: this machine has far less free RAM than these
notes have been claiming.** Measured on hardware:

```
Memory: 436k/1984k available (689k kernel code, 1548k reserved, 29k data, 40k init)
mem:  free pages 59   (= 236 KB)
```

**436 KB free at boot, 236 KB once the ramdisk is populated** - not the "~1 MB"
asserted previously. The kernel is the problem, not userspace: 689 KB of text
that has never had a size pass.

`docs/28-USERSPACE-AND-DESKTOP.md` is new and carries the plan that follows
from all this - the 2 MB budget, why a CD root changes the arithmetic, what
actually runs on a no-MMU kernel, and why Mac System 1 is the right model for a
desktop here rather than a whimsical one.


### 2026-08-25 - **A CD-ROM sector, read on the PlayStation**

```
blackroo> cd rd 10
  LBA 16 -> MSF 00:02:16 (BCD)
  0000: 01 43 44 30 30 31 01 00 50 4c 41 59 53 54 41 54  |.CD001..PLAYSTAT|
  0020: ...        42 4c 41 43 4b 52 4f 4f               |        BLACKROO|
```

`docs/24` opens by noting that **no Linux or BSD port of the PS1 has ever had a
working CD driver**. This is the first sector, and it worked first time.

`cmd_cd()` in `arch/mipsnommu/ps/brmon.c` - `cd init`, `cd stat`, `cd id`,
`cd rd <lba>`. Deliberately the slow shape that `docs/24` §4.5 asks for as the
bring-up step: **polled, PIO, no DMA and no interrupt handler**, so the command
path is proven without also proving DMA. The CD interrupt is masked out of
`I_MASK` for the duration - `HINTSTS` is the controller's own register and
latches regardless - which keeps this entirely clear of `psxkbd`'s timer and
`bu.c`.

**What it proves.** Every command answered with the interrupt `docs/24`
predicted, including `Init`'s two-stage INT3-then-INT2. The LBA→MSF conversion
in BCD with the 150-frame lead-in is right: LBA 16 became MSF `00:02:16`,
matching the value §2.2 derived from the disc image, and it returned sector 16
rather than a neighbour. 8-bit PIO reads of `RDDATA` work. The BFRD handshake
in the documented order - request data, *then* acknowledge - works, dummy
BIOS accesses and all. `Setloc` before every `ReadN` meant the Pause/ReadN
duplicate-sector trap in §2.3 never got a chance.

Reading `CD001` and the volume id `BLACKROO` is our own disc, read by our own
code - not merely bytes moving.

**Measurements for `docs/24`'s open questions.** Drive firmware (`Test 20h`)
is `98 06 10 c3` - 1998-06-10, version C3. Before `cd init` ran, `BUS_CFG` was
already `00020943` and `COM_DELAY` `00001325`. §1.6 records psx-spx's recipe
saying `1325h` while the BIOS trace on the same page says `132Ch`; the value
found in place on this console is **`1325h`**. `cd_init()` writes `132Ch`
regardless, following PCSX-Redux, and reads work - so both look serviceable at
2x with PIO.

Capture: `docs/captures/2026-08-25-cdrom-first-sector.txt`.

**DMA works too, same day.** `cd cmp <lba>` reads a sector both ways and
compares all 2048 bytes — identical at two different LBAs — and `cd dma` reads
arbitrary sectors (LBA 256 comes back as MIPS instructions, i.e. `KERNEL.EXE`
on the disc). `MADR = addr & 0xffffff`, `BCR = 0x00010200`,
`CHCR = 0x11000000`, poll bit 24. That is the block driver's data path proven:
0.36 ms a sector instead of several.

**And the cache question is settled.** `cd kseg` compares cached and uncached
views of the DMA buffer: **0 of 2048 bytes differ**, so DMA needs no invalidate
— the R3000A has no writeback data cache, its 1 KB "D-cache" being wired as the
scratchpad. Confirmed rather than assumed.

**A correction to `docs/24` §1.7 fell out of it.** That section frames the
question as KSEG0 vs KSEG1, but **this kernel is linked in KUSEG** — symbols are
plain physical addresses (`0015758c`, not `8015758c`), which `mem` corroborates.
So the uncached alias is `(addr & 0x1fffffff) | 0xa0000000`; the obvious
`addr | 0x20000000` produces a KUSEG address 537 MB into a 2 MB machine and
**hangs the console with no output at all**. The same fact is why `MADR` needs
no translation: a kernel buffer's address already *is* physical, so §4.5's "no
bounce buffer" conclusion holds for a different reason than it gives.

Next: the Linux block driver (`docs/24` §5) — 2048-byte `hardsect_size`, an ext2
image as a plain file on the disc, read-only.


### 2026-08-22 - **0.4 on a disc: two bugs between building it and booting it**

The 0.4 disc was burned, hung, and was fixed. Both faults were mine and both
are now guardrails.

**GR-017 - the penguin hung the GPU.** The disc reached the PlayStation logo
and stopped. Ruled out in order: the licence area (the image matched
`license-area.bin` byte for byte, and the logo drawing *proves* the BIOS read
it), the burn (1984 sectors read back identical), the media, and the write
speed. What actually happened is that the PS1 GPU walks a DMA linked list, and
`setPolyFT4()` fills in a primitive's length and command code but **not** the
24-bit "next" pointer. The primitives sat in `.bss`, so the GPU followed
whatever was there and never returned. Fixed with `catPrim` + `termPrim` +
`DrawOTag`.

The symptom is worth remembering: `logo_draw()` returned normally and the hang
appeared inside **`FntFlush()`**, the next unrelated function to touch the GPU.

**GR-018 - the root device default.** With that fixed, kloader ran but Linux
panicked. `settings_default()` said `root_device = ROOT_DEV_CARD_RAID`, which
had been harmless only because `cdrom_boot()` used a hard-coded
`root=/dev/ram0` string. Making it build the command line from settings - so
the L2/R2 video mode could reach the kernel - promoted that never-read default
to the boot device, and the kernel tried to mount memory cards with no
filesystem on them. Now `ROOT_DEV_RAM`.

**The technique that made this affordable.** Debugging kloader normally costs a
CD-R per attempt, because uploading it over serial would land it at
`0x80010000`, inside the running kloader (GR-008). Adding `-DBLACKROO_TEST_HIGH`
links a test build at `0x80090000` instead, so the *running* kloader can upload
and launch it: **7 seconds a cycle instead of a blank disc**. With
`BOOT_TRACE` markers over SIO1 that located the GPU hang in two runs.

Also of note: the emulator was useless here. kloader's `serial_putchar()` spins
unbounded on TXRDY, and SIO1 will not transmit without CTS - which the FTDI
provides on the console and nothing provides under emulation. The first tracing
attempt hung *itself*.

**Verified on hardware:** the disc now boots kloader, and Linux from it, to the
shell with the keyboard live.


---

## [0.4] - 2026-08-22 — "Untethered"

> 0.3 put the shell on the television. 0.4 is the release where the host PC
> stopped being *required*: a keyboard on the controller bus, output on the
> GPU, and a machine that drives itself. The serial cable now carries the log,
> not the session.

### Highlights

- **A keyboard.** `drivers/char/psxkbd.c` polls the Lightspan device
  (SIO0 id `0x96`) from a timer, turns PS/2 Scan Code Set 2 into Linux
  keycodes and hands them to `handle_scancode()`. It finds the adapter itself,
  sweeping both ports and all four multitap floors — ours answers `0x02`,
  behind the tap. `brsh` reads `/dev/console`, so typing happens on the
  console and appears on the television.
- **Selectable video modes, PS2-style.** L2/R2 walk eight modes on the kloader
  menu — 256/320/512/640 wide, 50 or 60 Hz — and the kernel boots in whichever
  is live. Deliberately self-recovering: if a mode shows nothing, press again.
  No dialog you have to be able to read in order to escape one.
- **`psxcon` honours the choice.** Console geometry is chosen at run time from
  `psxvideo=WxH@R` on the command line instead of two compile-time pairs, and
  **the NTSC vertical range is no longer wrong** — `InitGPU()` writes the PAL
  pair unconditionally, 15 lines too many for a 60 Hz field. Hi-res gives a
  **78x21** console rather than 37x21.
- **The Linux logo on the boot menu.** Larry Ewing's Tux, converted by
  `bootloader/tools/mklogo.py` straight out of this tree's own
  `include/linux/linux_logo.h` — the logo of the kernel kloader boots.
- **Memory cards behind a multitap, from kloader.** The card manager walks all
  eight slots as `(port << 2) | floor`, the same numbering as `bu.c` and BRMON,
  so a slot means one thing everywhere. Formatting confirms first.
- **The 2 MB config is the default again.** `build.sh` had been quietly
  producing 8 MB kernels for a 2 MB console (GR-015).

### Known issues at 0.4

- **`brsh` still has builtins only** — `help`, `echo`, `exit`. Real programs
  are the next question; `docs/26` §G costs the three routes. Size is not the
  blocker: `tools/busybox-mips-uclibc` is a static R3000 BusyBox with a 255 KB
  footprint, unreachable only because it links at `0x400000`.
- **One program resident at a time** — `binfmt_fixed` uses a single address.
- **No CD-ROM driver.** 700 MB the console reads reliably, still unreachable
  from Linux. `docs/24` has the research; this is the next major piece.
- **The 60 Hz modes are untested.** The vertical-range fix is reasoned from
  psx-spx, not yet seen on a television. Harmless to try: L2/R2 cycles onward
  if a set will not display one.
- **The chosen mode is not saved across power cycles** unless you save
  settings from the System Settings menu; within a session it carries into the
  kernel, which is what the menu promises.
- **Card writes through the block driver remain unproven** — only `bread()`
  reads. `root=/dev/bul` resolves.
- `tools/host/mkinit.c` still carries 4000-based syscall numbers, wrong for
  this tree (`__NR_Linux = 0`).

### Verified on hardware for this release

```
PSX: 1984 KB RAM configured
psxcon: 640x256 PAL 50Hz, console 78x21
psxkbd: keyboard on port 1, address 02 (multitap floor)
PSX joined card: driver initialized: 8 cards joined, total size = 381 Kbytes
```

The kloader half of 0.4 — logo, video menu, eight-slot card manager — is
build-verified only; it needs the disc to run at all, since uploading
`bootloader.exe` over serial would land it inside the running kloader's own
text (GR-008).

---


### 2026-08-21 - **The keyboard driver: typing on the PlayStation itself**

```
psxkbd: keyboard on port 1, address 02 (multitap floor)
brsh: console fd 0x00000004
$ PERFECT
```

`drivers/char/psxkbd.c` polls the Lightspan device on SIO0 from a kernel timer,
translates PS/2 Scan Code Set 2 to Linux keycodes and hands them to
`handle_scancode()`. The VT was already drawing on the GPU, so a keypress
becomes a character on the television with nothing further to do. `brsh` now
reads `/dev/console`, and the host PC is out of the input path entirely.
Capture: `docs/captures/2026-08-21-keyboard-driver.txt`.

**It finds the adapter itself.** BlueRetro is in multitap port B, so it answers
`0x01 + floor` = `0x02`, not `0x01`. The driver sweeps both ports and all four
floors at its idle rate, locks on, and reports where (GR-013).

**`CONFIG_PC_KEYB` is off, replaced by `CONFIG_PSX_KEYB`.** `pckbd_translate()`
expects Set 1 from an 8042 this machine does not have, so the driver emits
Linux keycodes directly from one Set 2 table and `kbd_translate()` is a
pass-through — one table instead of two.

**Sharing SIO0 with the cards** is `psx_sio0_trylock()` in `bu.c`, guarding the
same `bu_lock`, with `cli()` around the test-and-set. A lost race costs one
sample; corrupting a card transfer costs the filesystem.

**Three faults found and fixed on the way, each on hardware:**

1. *Detection sweep interference.* The keyboard timer starts during console
   init, before `bu.c`'s initcall, so it interleaved with the several-second
   card sweep and `/dev/bul` came up 254 KB instead of 381 KB — a corrupted
   first block reads as "card not found". Fixed with `bu_probing`, a flag
   separate from `bu_lock`; using `bu_lock` for it made the sweep lock itself
   out and every slot failed (GR-014).
2. *Serial input losing characters.* `KBD_ACK_SPINS` was 20000 iterations,
   ~6 ms on a 33 MHz R3000 — and the last byte of a frame never gets an
   acknowledgement, so every poll burned the full timeout. Stalls far longer
   than SIO1's 8-byte FIFO holds at 115200, so `kbd scan` arrived as `kbd sca`.
   `psxkbd_xfer()` now takes `want_ack` and skips the wait on the final byte.
3. *Doubled letters eaten.* "hello" came out "helo". A trace of raw frames
   settled why — see below.

**The protocol, settled with data rather than inference.** Two hypotheses fitted
and wanted opposite fixes, so the driver was made to dump raw frames:

```
nn=0:            244   nothing happened
nn=1: 59         237   right shift MAKE
nn=2: f0 59       26   right shift BREAK
```

Make and break codes, one event per frame — an **event stream**, not a report
of currently-held keys. So the frame de-duplication that had been added was
wrong: two identical frames are two real keypresses, and suppressing the second
ate input. The unbroken column of shift-makes that prompted it was ordinary
typematic autorepeat (237 makes to 26 releases — a held key), which the console
layer already handles; the printk made it look pathological, not the events.

Also from the trace: the adapter does not queue, so events between polls are
lost — poll rate 50 Hz to 100 Hz. And idle frames outnumber busy ones even
while typing, so a frame now stops once `nn` says there is nothing to collect:
five bytes instead of fifteen, which is what makes 100 Hz affordable.

**Unrelated but important, found in the same boot log:** `build.sh` had been
producing **8 MB kernels for a 2 MB console** all session — see GR-015 and the
entry below.

### 2026-08-21 - **build.sh was silently building 8 MB kernels**

`PSX: 8128 KB RAM configured` on a machine with 2 MB. `DEFCONFIG` defaulted to
`blackroo_8mb_defconfig` and `setup_config()` copies it over `.config` on every
build, so the first rebuild of the session switched away from the 2 MB image it
had started from, and nothing said so. It kept booting because everything in
use sits below 2 MB and PS1 RAM mirrors — which is what made it dangerous.
Default is now `blackroo_2mb_defconfig` (docs/21: both consoles are 2 MB stock);
a modded machine wants `DEFCONFIG=blackroo_8mb_defconfig ./build.sh kernel`.


### 2026-08-21 - **A keyboard talks to the PlayStation**

```
blackroo> kbd
  15 bytes: ff 96 5a 00 00 00 00 00 00 00 00 00 00 00 00
  keyboard present, 0 scancode byte(s)

blackroo> kbd watch
  [down 2d] 'r'   [down 42] 'k'   [down 43] 'i'   [down 31] 'n'
  [down 34] 'g'   [down 29] ' '   [down 24] 'e'   [down 1c] 'a'
```

An Apple aluminium Bluetooth keyboard through BlueRetro's Lightspan emulation,
onto SIO0, decoded by the monitor. Raw PS/2 Scan Code Set 2, make and break,
with ACK on every byte of the frame. The memory cards moved to port 2 and are
untouched — `/dev/bul` still 381 KB, which is the address-byte split from
`docs/15` working on real silicon: `0x01` keyboard, `0x81` cards, one bus.

No kernel driver involved. This is the polled probe in BRMON, deliberately,
because the monitor is the only place here where SIO0 has no other users.
Capture: `docs/captures/2026-08-21-keyboard-scancodes.txt`.

**Two obstacles, both invisible from the console side:**

1. The adapter was flashed with a custom Apr-2023 build **hardcoded to PS2**.
   BlueRetro fixes the console at compile time and `0x96` is a PS1 device, so a
   perfectly healthy adapter could never have answered. Reflashed to v25.04 hw1
   universal.
2. It then answered `41 5a` — a **gamepad**. BlueRetro maps a keyboard onto pad
   buttons unless the output port's `dev_mode` is `DEV_KB`
   (`main/wired/ps_spi.c:975`). That is normally set from the Web Bluetooth
   config app, which needs Chrome — not installed here, and the adapter turned
   out not to advertise over BLE while a keyboard is connected.

   So the config was patched directly in flash instead. `struct config` is
   `magic`(4) + `global_cfg`(4) + `out_cfg[]`(2 each), stored in `/fs/config.bin`
   on the ESP32's FAT `storage` partition at `0x310000`, with a magic and no
   checksum. Two bytes: `out_cfg[0].dev_mode` `0` → `2`. Full recipe in
   `hardware/blueretro/README.md`.

**The three states are worth recognising**, because they look alike from the
PS1 and mean different things:

```
ff 41 5a ff ff 80 80 80 80 ...   gamepad  - dev_mode is DEV_PAD
ff ff ff ff ff ...               nothing  - no Bluetooth device connected
ff 96 5a nn ...                  keyboard - correct
```

Next: the kernel driver, and arbitrating SIO0 with `bu.c`.


### 2026-08-21 - **BlueRetro adapter flashed for the keyboard work**

The adapter was on the bench and working — Bluetooth up, `internal_flag_init:
External adapter`, so the right HW1 variant — but its firmware was a custom
`35958de-dirty` build from **Apr 2023** carrying:

```
# Hardcoded system : 17: PS2
```

BlueRetro fixes the target console at compile time, and the Lightspan keyboard
is a **PS1** device (`0x96`); on PS2, keyboards go over USB instead. So a
perfectly healthy adapter would never have answered the `kbd` probe, and the
symptom would have looked exactly like bad wiring.

Reflashed with **v25.04 `BlueRetro_hw1_universal.bin`** — universal rather than
`playstation` so the adapter stays usable on other consoles; it auto-detects
rather than assuming, which means it must be connected to a powered console to
pick a system. All four images hash-verified on write; the board now reports
`App version: v25.04 hw1 universal` and is scanning for Bluetooth devices.

The old firmware was a `-dirty` build and is not reproducible from any release,
so the full 4 MB was read back first and kept:
`hardware/blueretro/blueretro-ps2-35958de-dirty-backup.bin` (+ md5).

**Notes for next time, in `hardware/blueretro/README.md`:** the BlueRetro log is
at **921600** baud, not 115200 — the ROM banner comes out at 115200 and the app
then reconfigures the UART, so the app log looks like line noise unless you
change rate. And `esptool` on this box is broken: the pipx venv at
`~/.local/bin/esptool.py` has `cpython-312` binaries under a Python 3.14 venv,
orphaned by a pyenv upgrade. `~/.pyenv/versions/3.11.10/bin/python` has a
working pip.


### 2026-08-21 - **BRMON `kbd`: a Lightspan keyboard probe, before any driver**

Keyboard work started, and the route chosen is **BlueRetro** — an ESP32 adapter
that already implements PS1 Lightspan emulation (device ID `0x96`) from any
Bluetooth HID keyboard. `docs/14` had this on file; Chelson has the kit.

The reason is not that it saves writing RP2040 firmware. All three candidate
routes need the *same* kernel driver, so what BlueRetro actually buys is that
**the driver becomes the only variable** — a failure is ours, not a race
between two new things. Rationale and the full plan: `docs/27-KEYBOARD-BRINGUP.md`.

**New:** `kbd` and `kbd watch` in BRMON. Clocks the exact Lightspan frame
(`01 42 00 ... 06`) at SIO0 address `0x01`, decodes `96 5A nn`, and prints
Set 2 make/break codes as `[down 1c] 'a'` / `[up 1c]`. `kbd watch` polls until
a key arrives on the serial side.

It lives in the monitor deliberately: that is the only place on this machine
where SIO0 has no other users — no interrupts, no `bu.c`, no scheduler. If
scancodes appear there, the wiring is right and everything after is software.
The decode anchors on finding `0x96` anywhere in the frame rather than at a
fixed offset, because SIO0 replies lag one transfer behind the sends — the
lesson the memory cards charged a day for.

It also distinguishes `nn = 0xFF` ("adapter is talking, no keyboard paired")
from no `0x96` at all ("nothing is answering address 01"), which are different
problems with different fixes.

Also added `mon_pending()`, a non-blocking counterpart to `mon_getc()`.


---

## [0.3] - 2026-08-21 — "Phosphor"

> 0.2 was the state at which development resumed: the kernel compiled and
> converted to a PS-EXE, and did not boot to a shell on hardware. 0.3 is the
> release where it does — and where the shell stopped needing a host PC to be
> seen at all. A PAL SCPH-750x boots Linux from a CD-R, mounts a root
> filesystem, runs a userspace process as pid 1, and draws its prompt on the
> television through the PlayStation's own GPU.

### The line that names the release

```
$ echo A machine from 1997 running a real operating
A machine from 1997 running a real operating
```

On the TV and on the serial link at once, typed into pid 1 on real silicon.

### Highlights

- **Boots on hardware, no host PC.** BIOS → kloader from CD-R → kernel at
  `0x80090000` → BRMON. Three fixes got it there: kloader's hand-rolled cache
  flush was corrupt (now the BIOS `FlushCache()`), the kernel was relinked clear
  of kloader's text and heap, and kloader masks `I_MASK` during bulk receive —
  which took serial uploads from "fails every time" to 912 KB in 87.5 s at a
  flat 10.4 KB/s.
- **Userspace.** `fs/binfmt_fixed.c` loads a static ELF at its fixed link
  address in a reserved 64 KB window at KUSEG `0x001f0000`; `userland/brsh.c` is
  a freestanding shell built with `mipsel-none-elf-gcc` 12.3.0. Nine separate
  faults stood between "no userspace has ever run here" and a prompt — the full
  account is `docs/25-ROOT-MOUNT-JOURNEY.md`.
- **Output on the television.** `/dev/console` had been resolving to the serial
  tty because `vt_console_driver` was misnamed `"ttyS"`; corrected, it resolves
  to tty 4:1 — the VT — which `psxvga_con` draws on the GPU.
  `blackroo_con_mirror()` in `do_con_write()` sends the same bytes out of SIO1,
  so a process reaches the screen and the serial link together.
- **Storage.** Three memory cards joined as `/dev/bul`, 381 KB, formatted and
  read on hardware. Four distinct faults: a lost-wakeup race in `bu_ready()`,
  multitap floors never addressed, a cascading sequence check, and a card that
  needed 2 ms to settle after `/JOYn` — that last one found because enabling
  `DEBUG` printks *made it work*.
- **BRMON**, an in-kernel serial monitor that needs no userspace, entered from
  the command line before `mount_root` and automatically on panic.
- **Bootable discs that actually boot.** `iso/build-iso.sh` transplants the
  first 16 sectors from a known-booting image; mkpsxiso 2.20's licence area
  hangs the console at the SCE screen, modchip or not.

### Known issues at 0.3

- **No keyboard.** Output reaches the TV, but `/dev/console` has no input side —
  `kbd-no.c` is what builds, and `psx_kbd.c`'s `kbd_init_hw()` is an empty stub.
  `brsh` still reads `/dev/brcon` over serial, so a host PC is needed to type.
- **One program resident at a time** — `binfmt_fixed` uses a single load
  address. The kernel itself multitasks fine.
- **Builtins only** — `help`, `echo`, `exit`. No `ls`, no `cat`.
- **Memory-card writes unproven** through the block driver; only reads via
  `bread()` have run. `root=/dev/bul` resolves, but there is no filesystem on
  the cards.
- **No CD-ROM driver.** 700 MB the console reads reliably, still unreachable
  from Linux. Research is done — `docs/24-CDROM-DRIVER-RESEARCH.md`.
- **The burned CD-R predates the userspace kernel**, so standalone boot still
  shows the older build; this kernel goes on by serial upload until a reburn.
- `tools/host/mkinit.c` still carries 4000-based syscall numbers, which are
  wrong for this tree (`__NR_Linux = 0`).

---


### 2026-08-21 (later still) - **The tty layer works: userspace draws on the television**

```
BLACKROO: /dev/console -> console "tty0" = tty device 4:1
brsh: console fd 0x00000004
$ echo A machine from 1997 running a real operating
A machine from 1997 running a real operating
```

On the TV as well as on serial, confirmed on the console. Capture:
`docs/captures/2026-08-21-tty-layer-gpu-output.txt`.

**The bug was one string, and it was not the transmit path.** Every note in the
tree blamed `serial_psx.c` for having no working TX. The actual fault was in
`drivers/char/console.c`:

```c
struct console vt_console_driver = {
	name:		"ttyS",			//???PSX   "tty"
```

`console_setup()` files each `console=` in `console_cmdline[]` and points
`preferred_console` at the **last** one, so `console=ttyS0,115200 console=tty0`
makes `tty`/0 preferred. `register_console()` grants `CON_CONSDEV` only when a
console's **name** matches that entry - and named `"ttyS"`, the VT console
matched entry 0, the *serial* one. So nothing was ever marked `CON_CONSDEV`,
`console_drivers` kept whatever registered first (siocons, since `console_init`
precedes `con_init`), and `tty_open()` - which resolves `/dev/console` by
walking that list and calling the head's `->device()` - handed out **ttyS0**.
Userspace writes went to the serial tty and slept there. The GPU was never in
the path at all.

Renaming it `"tty"` puts `/dev/console` on tty **4:1**, the VT, which
`psxvga_con` has been drawing on the GPU for printk all along.

**New:** `blackroo_con_mirror()` in `do_con_write()` - the same bytes out of
SIO1, so a process reaches the TV *and* the serial link. Hooked in
`do_con_write()` rather than `vt_console_print()` on purpose: printk does not
pass through `do_con_write()`, so serial still shows each kernel message once
rather than twice. It runs before `spin_lock_irq(&console_lock)` because the TX
poll costs ~87us a byte and must not hold interrupts off.

**`brsh`** now writes to `/dev/console` and still *reads* `/dev/brcon`.
Reading `/dev/console` would wait on the keyboard driver, and `kbd-no.c` is
what this port builds - `psx_kbd.c` exists but its `kbd_init_hw()` is an empty
stub. When a keyboard lands, input moves over and the host PC becomes optional.

**Also new:** a boot-time line reporting what `/dev/console` resolved to, so a
silent misroute cannot happen twice; and
`scripts/make-userspace-initrd.sh`, because the initrd had been assembled by
hand and could not be rebuilt after editing `brsh` (mke2fs + debugfs - this box
has neither root nor genext2fs).

**Untouched:** `serial_psx.c`. It was never the problem, and with
`/dev/console` no longer pointing at it there is no reason to disturb it. Note
`sio_ready()` *is* bounded (`SIO_TIMEOUT` is defined in siocon.c), so the old
"infinite spin in the TX path" theory was wrong on that count too.


### 2026-08-21 (later) - **Userspace runs on the real PlayStation**

```
BLACKROO: exec /bin/sh ...
binfmt_fixed: /bin/sh at 0x1f0000..0x1f0840, entry 0x1f03fc, stack 0x1fffb8
brcon: open by pid 1
brsh: userspace is alive on the PlayStation.
$ echo hardware: a real PlayStation, not an emulator
hardware: a real PlayStation, not an emulator
```

A PAL SCPH-750x, BIOS 4.1 E, 2 MB stock. Path: kloader off the CD-R ->
`Serial Shell (115200)` -> **912 KB uploaded to `0x80090000` in 87.5 s at
10.4 KB/s, no retries and no checksum errors** -> boot -> `cont` at BRMON ->
ext2 initrd root mounted -> `binfmt_fixed` loads `/bin/sh` -> interactive
prompt. The memory cards came up at **381 Kbytes** on the same boot.

Until now the pid-1 shell had only ever been seen in emulation, and the burned
CD-R still carries the older kernel — the serial-upload path is what proves it
on silicon. Capture:
`docs/captures/2026-08-21-userspace-shell-hardware.txt`.

**Operational note that cost a false start:** the PS1's SIO1 cable was on
**`/dev/ttyUSB1`** (the FTDI), not `ttyUSB0` (the Prolific). The adapters
enumerate in plug order, so `ttyUSB0` is not a stable name for the console.
Listen for a `BK>>` beacon on each port before uploading — the wrong port is
simply silent, which reads exactly like a dead console.

**`0x80090000` held up.** GR-008 records uploads dying at offset 6144 when the
payload landed inside kloader's own text; this build is linked clear of it and
the transfer ran end to end at a flat 10.4 KB/s.

### 2026-08-21 - **Userspace shell independently re-verified; three stale claims corrected**

The pid-1 shell milestone had **no capture on file** — `docs/25` §10.5 was the
only record of it, and this project has a guardrail about stages that claimed
success while doing nothing. Reproduced from scratch in PCSX-Redux against the
build #52 `output/blackroo.exe`: kernel boots, `cont` leaves BRMON, ext2 initrd
root mounts, `binfmt_fixed` loads `/bin/sh` at `0x1f0000` (entry `0x1f03fc`),
and the process answers `help` and `echo` over `/dev/brcon`. It is real.
Filed as `docs/captures/2026-08-21-userspace-shell-emulator.txt`.

**Corrected — `root=/dev/bul` resolves after all.** `docs/25` §11, `docs/26`
and `SESSION-STATE.md` all recorded "`bul` is not in `root_dev_names[]`" as the
next thing to fix. It has been there since 2026-08-20:
`init/main.c`, `{ "bul", (BU_LARGE_MAJOR << MINORBITS) }`, guarded by
`CONFIG_PSX_MEM_CARD`/`CONFIG_PSX_LARGE_CARD`, both set in `autoconf.h`. The
real unknown on that path is the **card write** path — only reads through
`bread()` have ever run.

**Corrected — Redux SIO1 settings live in `emulator.Debug`, not `emulator`.**
Keys placed one level too high are ignored *and silently discarded* when Redux
rewrites `pcsx.json` on exit, so the symptom is a missing TCP listener with no
error. `docs/20` now names the full paths and gives `ss -ltn | grep 6699` as
the check. Also documented `-loadexe output/blackroo.exe -run`, which skips
building a disc image.

**Corrected — a comment in `userland/brsh.c`** still claimed "no-MMU processes
run in kernel mode", the arrangement §10.4 found to be broken. They run in user
mode in KUSEG.

**Was unproven on hardware at the time of writing** — `output/blackroo.bin`
(the burned ISO) dates from 08:03, older than the 17:25 kernel that contains
`binfmt_fixed`, `brcon` and `brsh`, so no existing CD-R carries it. Resolved
the same day by serial upload; see the entry above. The CD-R still wants a
reburn if standalone boot matters.


### 2026-08-21 - **Userspace: a shell running as pid 1**

```
brcon: open by pid 1
brsh: userspace is alive on the PlayStation.
$ echo hello from a real process
hello from a real process
```

First process ever to run on this port. Root filesystem mounted from an ext2
initrd; `brsh` opens a device, reads keystrokes and writes output through real
syscalls.

**New:** `fs/binfmt_fixed.c` (loads a static ELF at its fixed link address,
rejecting anything outside the reserved window), `drivers/char/brcon.c` (polled
SIO1 character device, major 60), `userland/brsh.c` + `blackroo.ld` (freestanding
shell, 2 KB, built with mipsel-none-elf-gcc 12.3.0), 64 KB of RAM reserved in
`prom/memory.c` by declaring less than the machine has.

**Faults fixed on the way (full account in docs/25-ROOT-MOUNT-JOURNEY.md):**

1. initrd placed after the loaded image instead of `_end` - landed in BSS and
   was erased by head.S. Dormant since April behind "Couldn't find valid RAM
   disk image".
2. `addpsexe_initrd` then overflowed its buffer: padding measured from `_end`,
   which includes BSS and occupies no file space.
3. The initrd copy went through `f_op->write()` and **ignored the return
   value**, reporting success while storing nothing.
4. The bootmem bitmap is placed at the first page after the kernel - exactly
   where the initrd lives. `reserve_bootmem()` came too late. Now bootmem
   starts above the image.
5. `rd_open()` returned `-ENXIO` for any inode without `i_bdev`, but
   `blkdev_get()` - how `mount_root()` opens the root device - passes a fake
   inode. The check guarded only an optimisation.
6. Syscall numbers: this tree has `__NR_Linux = 0`, so `write` is **4**, not
   4004. `mkinit.c` has the wrong ones, which is why the April FLAT experiment
   could never have worked.
7. `write()` to `/dev/console` never returns - the tty layer has no working
   transmit path here. Hence brcon.
8. Kernel-mode processes break `current` (derived from the kernel stack
   pointer; kernel-mode traps don't switch stacks) - pid read as 0 and
   `sys_open` crashed into kloader's resident code.
9. Programs must therefore live in **KUSEG** (`0x001f0000`), which this CPU
   maps straight to RAM. The TLB-refill code in head.S is dead - there is no
   TLB.

**Also:** two kernel source files were truncated to zero by a careless
`open(path, 'w')` in an edit script that then failed on encoding; restored from
`~/projects/blackroolinux-main` and the day's changes re-applied. Edits now go
through a temp file and `os.replace()`.

**Research delivered:** `docs/24-CDROM-DRIVER-RESEARCH.md` - 1790 lines on
writing a PS1 CD-ROM block driver, sourced from the PSn00bSDK psxcd source,
psx-spx, and real-hardware measurements, with 14 items flagged VERIFY. Notable:
no Linux or BSD PS1 port has ever had a working CD driver.

### 2026-08-21 — Session close: 381 KB of storage, and the next target

**Memory cards finished** — three joined as `/dev/bul`, 381 Kbytes, verified
with `DEBUG` off. Two final fixes after the format worked:

1. **The sequence check cascaded.** It compared each card's number against a
   running count of cards found so far, so one failed probe poisoned every card
   after it — slot 3 was read perfectly and rejected with "found 2 instead 1".
   Now each card is judged on its own `number` (in range, unclaimed).
2. **`bu_rd_state0()` had no settle after asserting `/JOYn`.** Found by
   accident: enabling the driver's `#ifdef DEBUG` printks made detection start
   working, because the print latency supplied the missing delay. The monitor's
   polled `mc_select()` had always waited ~2 ms, which is exactly why it could
   reach multitap sub-ports the driver could not. Fixed with `BU_DELAY(2000)`.

Also tried and wrong along the way: a settle *between* slot probes, and
deasserting `/JOYn` after each transaction (kept anyway — releasing the bus is
correct, it just was not this bug).

**Documentation for others following this work:**
- `docs/22-WHAT-WENT-WRONG.md` — full post-mortem, ten faults with symptoms,
  root causes and fixes
- `docs/captures/` — raw serial captures with a README explaining how to read
  the detection sweeps
- `docs/23-ROOT-FILESYSTEM-PLAN.md` — the plan for the next session

**Next goal: a real userspace shell on a root filesystem.** The blocker is
unchanged and is not the filesystem: nothing can be `execve()`d on this kernel
(BINFMT_FLAT only, no ELF loader built, all BusyBox binaries linked at 4 MB on
a 2 MB machine). The plan starts with a fixed-address ELF loader and a small
shell of our own, then an initrd, then ext2 on the cards.

### 2026-08-21 — **Memory cards work: formatted, detected, read through the block layer**

All four cards formatted on real hardware, and the driver now claims one and
serves reads through `bread()`.

**The last fault was the sector number's position in a card write.** Three
attempts returned FFh "bad sector" while the opening handshake looked perfect.
Instrumenting the first replies (`head:`) answered it in one cycle: the card
was replying FLAG then 5Ah, i.e. it had accepted the 'W' command. The write
sequence sends **two dummy bytes** after the command — during which the card
returns its 5Ah 5Dh id — and only then the sector:

```
81h -> N/A    57h -> FLAG    00h -> 5Ah    00h -> 5Dh
MSB -> 00h    LSB -> pre     data[0..127]  CHK
```

Sending MSB/LSB straight after 'W' made the card read data[0]/data[1] as the
sector number — 34h 12h, the low half of the 0x1234 magic being written, i.e.
sector 13330 on a 1024-sector card. **The card was right to refuse.** It hid
because only sector 0 was ever read, and its address bytes are 00 00 wherever
they sit in the sequence.

**Result on hardware:**

```
card format 0 yes 0 .. 3 yes 3
  head: ff ff 08 5a 5d 00      FLAG, id pair, then the sector
  tail: 00 5c 5d 47            acknowledge pair, then 'G'
  verify: id=00001234 size=1024 serial=b1acc000 number=0   (and 101/1, 202/2, 303/3)

card
  joined device (major 208):  /dev/bul  127 KB      (was 0 KB)

card rd 0
  0000: a0 00 00 00 00 00 00 00 ff ff 00 00 ...       via bread(), block layer
```

Tap sub-ports C and D formatted fine here, having looked silent to the earlier
probe — that was the probe's slow ACK sampling desyncing them, not dud cards.

**Still open:** only 1 of the 4 cards is joined; the boot detection sweep needs
capturing to see whether slots 2-4 report "not found" or "Bad card sequence".
Prime suspect is the driver's own probe hitting the same slow-ACK sensitivity
that the monitor's transfer timing fix cured.

**Written up:** `docs/22-WHAT-WENT-WRONG.md` — the full session post-mortem,
nine faults with symptoms, root causes and fixes.

### 2026-08-21 (later) — Memory cards: three faults found, two fixed

Debugged live from the monitor on real hardware, with `sio0`, a new polled
SIO0 probe in BRMON.

**The cards were never the problem.** `sio0 0 81` on the console returns
FLAG `08`, ID `5A 5D`, and `/ACK` on every byte — a textbook healthy exchange.
`sio0 0 82` (multitap sub-port B) answers identically. `0x83`/`0x84` do not
respond, either unseated or dud.

**Fault 1 — lost-wakeup race in `bu_ready()` (fixed).** It set `bu_state`,
armed a timer, tested the state and called `sleep_on()` with interrupts
enabled throughout; the original author even left a `// check - may be we lose
interrupt ?` comment on the test. The acknowledge IRQ can land between the
test and the sleep, `bu_interrupt()` wakes a queue nobody is on yet, the
wakeup is lost, and the caller reports "card not found". A card pulls /ACK
~100us after each byte, so on real silicon it usually wins that race — while
emulators deliver the IRQ later and coarser and usually do not. That is
exactly the "detection works in DuckStation, fails on the console" behaviour
recorded in April. Now `cli()` around the test, `while` instead of `if`.

**Fault 2 — the multitap was never addressed (fixed).** `bu_rd_state0()`
already emitted `0x81 + floor`, which is the correct sub-port addressing, but
nothing ever set `floor`. `BU_MINORS` was 2 and `bu_curr_request.floor` was
hardcoded to 0, so only the direct ports were ever probed. Now `BU_MINORS` is
8 with `minor = (port << 2) | floor` — minors 0-3 are port 1 sub-ports A-D,
4-7 are port 2. A directly-plugged card is sub-port A, so the old behaviour is
preserved.

**Fault 3 — cards need a Blackroo header (not yet working).**
`bu_read_first_block()` rejects any card whose block 0 does not begin with
`BU_ID` (0x1234), so a stock Sony card is *always* "not found" by design.
`mkmemcard` writes that header into `.mcd` images, which is why this only ever
worked in emulators. Added `card format <slot> yes [seq]` to BRMON: a polled
SIO0 sector write of the header, sequence-numbered for
`CONFIG_PSX_LARGE_CARD`, requiring a literal "yes" because it erases block 0.

**Where it stops:** the write returns `FF` (bad sector) and the read-back shows
`id=434d0000` — bytes `00 00 4D 43`, i.e. Sony's `MC` magic *still intact*
(no data lost) but **shifted 2 bytes**. A consistent 2-byte offset through a
multitap points at the tap inserting protocol bytes, which would also make the
card read the sector number 2 positions off and answer "bad sector". Next test
is a card plugged directly into port 1 with the tap removed: if that formats
cleanly, the format code is right and only tap framing remains.

**Also learned:** replies on SIO0 lag one transfer behind sends, so status
bytes must be found by anchoring on the `5C 5D` acknowledge pair rather than by
counting transfers — the first two attempts assumed fixed offsets and misread
an intermediate `FF` as failure.

**Cost of the wider probe:** detection now walks 8 slots with full timeouts,
turning a fast failure into a ~90 s boot delay. Worth adding an ACK-based
presence check so a dead slot is skipped immediately.

### 2026-08-21 — **Linux boots from CD on real hardware, with an interactive prompt**

The milestone. Disc in, power on, no host PC in the boot path:

```
PSX SIO console enable
RAMDISK driver initialized: 16 RAM disks of 2048K size 1024 blocksize
PSX joined card: driver initialized: 2 cards joined, total size = 0 Kbytes
Starting kswapd v1.8
PSX serial port driver

  ####  BLACKROO MONITOR  ####
  Linux 2.4.0.0pre0 on PlayStation (MIPS R3000A)
blackroo>
```

Verified live on the console: `cpu` -> PRId 0x00000002 (R3000A rev 0.2),
SR 0x10000401 with **IsC=0**; `mem` -> 512 pages / 2048 KB, 105 free, kernel
0x90000..0x168ab0; `card`, `tty off` all answering.

Path: BIOS boots kloader off the disc -> **Boot from CD-ROM** -> kloader reads
KERNEL.EXE and chainloads it -> kernel runs -> BRMON on SIO1.

**Three fixes made this work, all found today:**

1. **kloader's cache flush was corrupt** (`bootloader/src/kernel.c`). It did
   `mfc0 $t0,$12` then `lui $t0,1`, discarding the status register it had just
   read and setting SR to a bare IsC — and it ran the zeroing loop from cached
   KSEG0 while the cache was isolated, which is unsafe on real silicon. On
   hardware this crashed *inside kloader*: no beacons, no kernel, screen frozen
   on the last frame. Replaced with the BIOS `FlushCache()`. `IsC=0` at the
   prompt confirms the fix.
2. **The kernel was relinked to 0x80090000** (`arch/mipsnommu/ld.script`).
   kloader's text is 0x80010000..0x8001E800 and PSn00bSDK puts its heap
   immediately above, where `FntOpen()` allocates the font buffer. 0x10000
   overwrote kloader's code (died at offset 6144), 0x20000 overwrote its heap
   (died at offset 0). 0x90000 clears both — and this is also what finally made
   kloader's CD chainload work, after it had been broken since it was written.
3. **Interrupt masking during bulk receive** (`bootloader/src/shell.c`).
   `serial_getchar()` cannot throttle the host and SIO1 has an 8-byte FIFO, so
   the VSync IRQ (which polls the pad over SIO0) dropped bytes and hung the
   transfer. Masking I_MASK for the transfer took uploads from "fails at
   offset 0" to **742 KB in 71 s at line rate**, and a paced 8-byte/3 ms
   workaround is no longer needed.

**Also:** kloader now shows `RECEIVING - do not touch` when a transfer starts,
drawn before the header ack (the only moment nothing is in flight — drawing
after the ack is what drops bytes). DMA is now actually stopped at handover
(`DPCR = 0`; the old value 0x07654321 *enabled* every channel despite the
comment). The `BRCL` cmdline tag is compiled in, so custom boot strings work.

**Build unblocked:** PSn00bSDK 0.24 + GCC 12.3.0 installed natively under
`~/projects/toolchains/psn00bsdk`, with cmake/ninja unpacked locally — no
Docker, no root. `bootloader/build-native.sh` builds kloader again, after
months where it could not be built at all.

**First hardware finding from the new prompt:** `card` reports `/dev/bul 0 KB`
and both slots not found, while DuckStation reports `2 cards joined, 254 KB`
from the same driver. The SIO0 memory-card detection failure from April is
reproducible, and now has an interactive prompt in front of it.

**Known cosmetic issue:** with `tty` echo on, every monitor line appears twice
on serial, because `printk` writes to all consoles including ttyS0.

### 2026-08-21 — kloader can't load the kernel by any route (GR-008)

Serial upload of the kernel via kloader's shell fails at offset **6144 every
time** — same root cause as the CD chainload hang: the kernel loads at
`0x80010000`, which is inside kloader's own text (`0x80010000..0x8001E800`), so
`receive_data()` overwrites the running loader. Three chunks land on
startup/menu code that isn't executing; the fourth hits the live receive path.

Byte-level pacing (`tools/host/paced-upload.py`, written on the theory that
kloader's `serial_getchar()` lacks RTS throttling) made no difference — the
failure is deterministic, not an overrun. The tool is kept because the
flow-control observation is still true and will matter for bulk transfers, but
it was not this bug.

**Consequence: BIOS CD boot is currently the only way to run the kernel on
hardware.** Fixes, all needing a kloader rebuild (blocked on PSn00bSDK): relink
kloader high, relink the kernel away from `0x80010000`, or a two-stage loader.

### 2026-08-21 — Both consoles profiled; a real cheat cart found and backed up

Read out over SIO1 from kloader's serial shell, running off the CD.

**Console #1:** PAL, BIOS `System ROM Version 4.1 12/16/97 E`, **2 MB stock**,
PAL video, EXP1 empty.

**Console #2:** identical BIOS/region/RAM, but EXP1 holds a
**`^_^ POWER REPLAY III (Y2K VER.) ^_^`** — an Action Replay clone, 256 KB flash,
~118 KB used, second 256 KB of the window empty.

**Consequences:**
- Neither console has the 8 MB mod, so **2 MB is the target, permanently**. The
  742 KB kernel with ~1.2 MB free is the real budget; a 2.8 MB kernel+initrd is
  not loadable on this hardware.
- Both are PAL with the same BIOS, so one disc and one kernel build serve both.
- `docs/18` (cart-resident boot) has hardware at last, and its central claim is
  now **verified on silicon**: signature `"Licensed by Sony Computer
  Entertainment Inc."` at `0x1F000084`, entry pointer at `0x1F000080`
  (`-> 0x1F000120`), header duplicated at `0x1F000000`.

**Backed up before anything touches it:** `carts/powerreplay3-256k-backup.bin`
plus md5 and provenance in `carts/README.md`. This is the only route back from a
bad flash write.

**Also learned:** the ROM carries a built-in MIPS debugger (register-name table),
a cheat UI, a PC-link mode ("PC IS IN CONTROL") and PAL/NTSC selection.

**Userspace investigation (no code yet):** all three busybox binaries in the tree
are linked at `0x00400000` — 4 MB, unreachable on a 2 MB console in any binary
format. EGCS 2.91 won't produce FLAT-friendly code either: plain builds emit
`R_MIPS_26` absolute call targets that bFLT relocations cannot patch, and
`-membedded-pic` is overridden by the target's `-mabicalls` default (GOT/`$gp`
code). Options recorded in the session notes: a fixed-address ELF loader with our
own small shell (simplest), GOT-slot relocation into bFLT (the "correct" uClinux
route), or a CD block driver plus an ext2 image on the disc, which sidesteps the
RAM limit for storage entirely.

### 2026-08-20 (night) — kloader boots off CD on real hardware

**First time Blackroo has booted from a disc on the actual console.** The
kloader menu came up on the TV from a burned CD-R on the modchipped PS1.

**Root cause of three failed discs — the licence area's Form-2 tail (GR-005):**
mkpsxiso 2.20 writes disc sectors 12-15 with empty bodies; the older mkpsxiso
that produced the homebrew discs this console boots writes
`00 00 08 00 00 00 08 00` at the start of each. Sectors 4-11 are byte-identical
either way, as are the TOC control flags, `CD-XA001`, and every PVD identifier —
this was the *only* structural difference between our disc and a booting one.
The BIOS renders the PlayStation logo from that licence data, and the console
hung at the SCE screen without ever drawing it.

**Fix:** `iso/build-iso.sh` now transplants the first 16 sectors verbatim from
`iso/license-area.bin`, a reference lifted from a known-booting image, after
mkpsxiso runs. Our discs are now byte-identical to a booting disc across the
whole licence area, and carry our own filesystem from sector 16 on.

**Also learned the hard way:**
- A retail disc booting proves nothing about a modchip (GR-006). THPS2 playing
  was taken as evidence the CD-R path worked; it isn't. Three discs and a batch
  of kernel changes went into chasing a fault that was never in the kernel —
  kloader, proven on this console in April, failed off CD identically.
- `cdrdao read-cd --read-raw` returns near-zero data on this drive and looks
  like a ruined burn; the kernel CD driver reads the same disc perfectly (GR-007).

**Kernel changes made during the hunt (kept — they are correct regardless):**
- `head.S`: `kernel_entry` now quiesces the machine before anything else —
  `SR=0`, `I_MASK=0`, `I_STAT=0`, `DPCR=0`, `DICR=0`. Every previous boot came
  via UniROM/kloader, whose `kernel_launch()` did this for us; the BIOS CD
  loader hands over with the BIOS shell's IRQs and DMA still live.
- `head.S`: emits `BR!` on SIO1 as the first act of the kernel, before C, before
  BSS — an unambiguous "we were entered" marker, with a bounded TX wait so a
  machine with no cable falls through instead of hanging.

**Reference discovered:** the disc that boots on this console is not one of ours
— volume `MOVIE`, publisher `OLD-HARD`, 2026-03-20, a third-party homebrew
release. Its SYSTEM.CNF is `BOOT=cdrom:\player.exe;1 / TCB=4 / EVENT=10 /
STACK=801FFFF0`, exe at LBA 24.

**Serial still unproven** — the cable produced 177 B/s of garbage while
connected and silence since; nothing readable at any baud from 9600 to 518400.
Next test is kloader's own serial shell, which worked on this console in April.

### 2026-08-20 (later) — First interactive prompt: BRMON on SIO1, booted from CD

**The tech-demo disc.** `output/blackroo.bin` boots on a modchipped stock
2 MB console: BIOS reads `SYSTEM.CNF`, loads the 742 KB kernel PS-EXE off the
disc, and the kernel stops in a new in-kernel serial monitor before it ever
looks for a root filesystem. No host PC, no UniROM, no memory-card exploit.

**Added — `blackroo/arch/mipsnommu/ps/brmon.c` (BRMON):**
- Interactive monitor in kernel context, talking to SIO1 directly (its own
  MODE/CTRL setup with RTS asserted, per docs/13) so it works with or without
  a working tty layer.
- `md` `peek` `poke` `fill` `ram` `hw` `cpu` `mem` `reboot` `cont`, line
  editing, and automatic KSEG0/KSEG1 address selection so `md 1f801814` reads
  GPU registers uncached while `md 10000` reads RAM cached.
- Entry points: `brmon` on the command line (checked in `init/main.c` after
  `do_initcalls()` and *before* `mount_root()`, so no root fs is needed), and
  as the fallback when every `execve` fails instead of `panic("No init found")`.
- Rationale: `CONFIG_BINFMT_FLAT` with no `binfmt_elf` built means the 1.5 MB
  ELF BusyBox in `initrd/skeleton/` can never be exec'd — that, not the initrd,
  is why the project never reached a prompt. Full writeup:
  `docs/20-SERIAL-MONITOR.md`.

**Fixed — the kernel config had no effect on the kernel (GR-001):**
- `include/linux/autoconf.h` was hand-maintained and had drifted 31 symbols
  away from the defconfigs, including `CONFIG_PSX_2MB_RAM` and
  `CONFIG_PSX_LARGE_CARD`. `build.sh` now generates it from `.config` on every
  build, and wipes all `*.o`/`*.a` when it changes (GR-002 — a stale `traps.o`
  otherwise links the previous config).

**Fixed — 2 MB targets were not actually 2 MB:**
- `prom/memory.c` hard-wired `RAM_SIZE = 0x0B88` / 8 MB with the config-driven
  block inside `#if 0`; restored (GR-004). A stock console was being told it
  had 6 MB of RAM that does not exist.
- `tools/elf2psexe.c` hard-wired the PS-EXE stack to `0x807fff00`, unaddressable
  on a stock console. Now an argument, defaulting to `0x801fff00`; `build.sh
  convert` picks it from `CONFIG_PSX_*MB_RAM` (GR-003).
- `arch/mipsnommu/kernel/Makefile`: `softfp.o` was gated on
  `CONFIG_MIPS_FPU_EMULATOR`, but `do_fpe()`'s *non*-emulator path calls
  `simfp()` from it — inverted gating, so turning the emulator off would not link.

**Changed — smaller kernel:** FPU emulator off in all defconfigs (−47 KB text).
Kernel PS-EXE is **742 KB** (was 782 KB), `0x80010000..0x800c9800`, leaving
~1.2 MB free on a stock console.

**Changed — command line is no longer hardcoded:** `prom/cmdline.c` takes a
loader-supplied line from `0x80000180` when tagged `"BRCL"`, else the built-in
default (`brmon root=/dev/ram0 init=/bin/sh console=ttyS0,115200 console=tty0`).
`bootloader/src/kernel.c` updated to emit the tag — **not rebuilt**, no
PSn00bSDK on this machine.

**Changed — disc images swapped roles:**
- `output/blackroo.bin/.cue` — BIOS → `KERNEL.EXE` → monitor. **Burn this one.**
- `output/blackroo-kloader.bin/.cue` — BIOS → kloader menu (unchanged binary).

**Build environment restored without Docker or root:**
`sdk/setup-local-toolchain.sh` unpacks `libc6-i386` into `sdk/i386-runtime/`
and patchelfs a copy of the bundled EGCS 2.91.66 toolchain into
`sdk/toolchain-local/` (it also resolves the dangling absolute symlinks for
`as`/`ld`/`ar`/... that pointed into an old `Archive/` path). `build.sh` picks
it up automatically when the host has no `/lib/ld-linux.so.2`.
`tools/host/mkmemcard.c` needed `#include <unistd.h>` for modern gcc.

**Tested in emulation the same day — it works:**
- **DuckStation**: disc boots, kernel runs, GPU console shows
  `Memory: 1076k/2048k available` (the 2 MB fix, confirmed) and
  `PSX joined card: 2 cards joined, total size = 254 Kbytes` (card RAID alive),
  then stops at `PSX serial port driver` — that is BRMON waiting on a serial
  byte DuckStation cannot deliver (no SIO1 host bridge in this build).
- **PCSX-Redux**: full interactive BRMON session over its SIO1 TCP server —
  `help`, `ram`, `cpu`, `hw`, `peek`, `md`, `mem` all answered correctly, with
  per-character echo. `ram` reports `RAM_SIZE = 0x0888 [2 MB setting]`,
  `mem` reports 512 pages / 233 free.
- Gotcha worth remembering: Redux's `SIO1Mode` must be **1 (Raw)**. On the
  default 0 (Protobuf) the monitor's output still arrives but every byte is
  wrapped and **input is silently dropped** — it looks exactly like a hung
  machine. BIOS choice was irrelevant (SCPH1001 and SCPH5502 both fine).
- Added `tools/host/redux-sio1.py` — terminal for BRMON over the Redux SIO1
  socket, interactive or scripted (`-c ram -c cpu`).

**Not yet tested on real hardware.**

### 2026-08-20 — Bootable CD-ROM images

**New:** `iso/` — a mkpsxiso project that turns the existing prebuilt PS-EXEs
into PS1-bootable discs. First boot path that needs no host PC at all.

**Added:**
- `iso/build-iso.sh` — builds both images, prints PS-EXE load maps first
- `iso/blackroo_cd.xml` + `iso/SYSTEM.CNF` — BIOS boots `BLACKROO.EXE` (kloader menu)
- `iso/blackroo_direct_cd.xml` + `iso/SYSTEM.DIRECT.CNF` — BIOS boots `KERNEL.EXE`
  (Linux + initrd) directly; 8 MB console only, 2.8 MB payload
- `iso/license.dat` — zero-filled 28,032-byte licence area placeholder
- `docs/19-BOOTABLE-CD.md` — full writeup

**Built and verified structurally** (`dumpsxiso` round-trip: SYSTEM.CNF,
BLACKROO.EXE, KERNEL.EXE, LINUX.EXE all extract intact, 4.2 MB Mode2/2352
image, valid `.cue`). **Not yet booted** in an emulator or on hardware.

**Finding — kloader "Boot from CD-ROM" overwrites itself:**
`bootloader/src/kernel.c:cdrom_boot()` copies the kernel to `hdr.t_addr`, but
kloader (`0x80010000..0x8001E800`) and both kernel PS-EXEs (`t_addr 0x80010000`)
occupy the same address. The copy loop overwrites its own code. Never noticed
because the menu entry has never been run. Fix designed but not implemented:
relink kloader high (`0x80700000` for 8 MB, `0x801F0000` for 2 MB) plus a ~1 KB
stage-1 PS-EXE at `0x80010000` that sets `RAM_SIZE` and `LoadExec`s the right
flavour. Direct-boot disc is unaffected — the BIOS does the copying there.

**Blocker for that fix:** `bootloader/build.sh` builds kloader through the
`blackroo-psn00bsdk` Docker image, and **Docker is no longer installed on this
machine**. Needs Docker reinstalled or a native PSn00bSDK install.

**Superseded:** `bootloader/iso/blackroo_cd.xml` (referenced output filenames
that were never produced, no SYSTEM.CNF — could not make a bootable disc).

**No PS1 source changes.** Build-system and documentation entry.

### 2026-06-23 — Cart-Resident Boot Research (kloader from the expansion port)

**Research milestone:** Documented how to make the PlayStation boot the kloader
**from a GameShark / Action Replay cartridge at power-on**, instead of uploading
it into RAM via UniROM/FreePSXBoot. This is the path to a self-contained Blackroo
cart with no host PC and no memory-card exploit.

**Key findings:**
- The PS1 BIOS runs a **pre-boot expansion-ROM scan** of EXP1 (`0x1F000000`)
  early in reset: it checks `0x1F000084` for the ASCII string
  `"Licensed by Sony Computer Entertainment Inc."` and, if present, calls the
  entry at `0x1F000080`. Same hook UniROM/Caetla/n00brom use.
- At pre-boot the **`A0/B0/C0` BIOS vectors are not yet installed** — cart code
  can call no BIOS functions. Caetla works around this by checksumming
  `0xBFC06000..0xBFC07FFF` to identify the BIOS version and using hard-coded
  per-version addresses; our stub must be equally self-contained.
- **Size fits easily:** kloader is **58 KB** (built today); smallest supported
  cart flash is 128 KB (SST39SF010/AM29F010), so header+stub+payload leaves the
  cart >half free.
- **Reuse:** `pioflash.c` already has the erase/program/verify path — the
  self-flash ("Install to cart") mode is mostly wiring, not new flash code.

**Work identified (not yet implemented):** EXP1 header+stub at `0x1F000080`,
cart-resident linker layout emitting a flashable `.bin` (not a PS-EXE), self-flash
wiring into `pioflash_program()`, and brick-safety (dump-original + verify).

**Exact header bytes flagged VERIFY** — to be lifted from the n00brom source at
implementation time, not transcribed.

**Documentation created:**
- `docs/18-CART-RESIDENT-BOOT-RESEARCH.md` — full writeup with sources

**Build note:** kloader rebuilt clean via the `blackroo-psn00bsdk` Docker image —
`bootloader.exe` = 59,392 bytes (58 KB), entry `0x8001A178`. (The
`bootloader/README.md` "Not yet started" status is stale and should be updated.)

**No kernel/bootloader code changes.** Research/documentation entry.

### 2026-04-16 — PS2 Microkernel OS Research Investigation

**Major milestone:** Full technical investigation into building a microkernel
operating system for the PlayStation 2. This is the foundational research for
"BlackrooOS" — a custom OS that would run natively on PS2 hardware with
networking, DOS compatibility, and game support.

**Research scope (7 parallel investigations + 1 bonus):**

1. PS2 hardware architecture (EE R5900, IOP R3000A, GS, VU, DMA, interrupts)
2. ps2sdk development toolchain (GCC 15.2.0, newlib, IRX modules, build system)
3. PS2 networking capabilities (SMAP driver, lwIP 2.0.0, OPL, ps2link)
4. Existing PS2 OS projects (PS2 Linux, kernelloader, NetBSD, U-Boot)
5. Microkernel design for MIPS (L4/MIPS, HelenOS, IPC, scheduling, TLB)
6. PS2 boot process and FreeMCBoot (boot chain, IOP reset, ELF loading)
7. Legal and licensing (AFL 2.0, Sony trademarks, distribution precedent)
8. 8086/DOS emulation feasibility (8086tiny, FreeDOS, DOSBox-on-PS2 precedent)

**Key findings:**
- PS2 homebrew networking is **production-quality** — myth busted
  - OPL loads games over SMB daily, lwIP 2.0.0 with DHCP/DNS/TCP/UDP
  - Source: ps2sdk NETMAN.txt, OPL Ethernet docs, ps2-home.com benchmarks
- R5900 has a **48-entry TLB** — memory protection IS possible
  - Stock BIOS wastes it on cache control; a custom OS reclaims all entries
  - Source: PCSX2 MMU Mini-Series, ps2tek COP0 Memory Management
- **L4/MIPS achieved 86-cycle IPC** on R4700 — microkernel on MIPS is proven
  - Source: UNSW L4/MIPS project, Inside L4/MIPS paper
- **DOSBox already runs on PS2** (belek666 port) — 8086 emulation is proven
  - Source: psx-place.com, VOGONS forums, ps2dev forums
- **No microkernel has ever been built for a game console** — this would be first
- ps2sdk uses AFL 2.0 — **commercial use explicitly permitted**
  - Source: ps2sdk LICENSE on GitHub
- **No confirmed cases** of Sony suing PS2 homebrew distributors
  - Source: legal research across case law databases

**Documentation created:**
- `docs/16-PS2-MICROKERNEL-RESEARCH.md` — full research document (all findings)
- `docs/16-PS2-MICROKERNEL-RESEARCH/refs/00-SOURCE-INDEX.md` — 100+ source URLs

**Key reference projects identified:**
- kernelloader/TGE — open-source SBIOS, hardware init
  - Source: https://github.com/rickgaiser/kernelloader
- Fredrik Noring's 120-patch Linux series — best modern R5900 kernel reference
  - Source: https://lore.kernel.org/linux-mips/
- L4/MIPS — MIPS microkernel with 86-cycle IPC
  - Source: http://www.cse.unsw.edu.au/~disy/L4/MIPS/
- HelenOS/SPARTAN — modern microkernel with MIPS32 port
  - Source: https://github.com/HelenOS/helenos
- 8086tiny — MIT-licensed single-file 8086 emulator (25KB source)
  - Source: https://github.com/adriancable/8086tiny

**No code changes.** This is a pure research/documentation entry.

### 2026-04-14 — Kloader v0.0.1 Verified on Real Hardware

**Major milestone:** Blackroo kloader running on real PlayStation 1 hardware
with bidirectional serial communication confirmed. First time our own code
(not UniROM) is handling serial I/O on the actual console.

**Verified on real PS1 (via FTDI FT232RL):**
- `sendexe` uploaded bootloader.exe to UniROM via SEXE V2 protocol (9.6 KB/s)
- Kloader booted, menu displayed on TV, controller input working
- Serial Shell entered, BK>> beacons received by host
- `ping` → `PONG` confirmed — full bidirectional SIO1 communication
- Upload workflow confirmed: sendexe → shell → upload → boot

**Critical hardware bugs fixed:**
- `SIO1_MODE`: was 0x004D (MUL1, baud 16x too fast), fixed to 0x004E (MUL16)
  - Source: psx-spx.consoledev.net/serialinterfacessio/ (MODE register bits 0-1)
  - Source: PSn00bSDK psxsio.h SIO_MODE_BAUD_MUL16 = 0x0002
- `SIO1_CTRL`: was 0x07 (no RTS), fixed to 0x27 (RTS asserted)
  - Without RTS, FTDI doesn't drive CTS → PS1 SIO1 hardware blocks all TX
  - Source: psx-spx.consoledev.net/serialinterfacessio/ (STAT bit 0 depends on CTS)
  - Source: psx-spx.consoledev.net/konamisystem573/ (CTS/RTS tie requirement)
  - Source: NOTPSXSerial sets DtrEnable=true, RtsEnable=true
- Host tool: added `ser.rts = True` and `ser.dtr = True` after port open
  - Source: NOTPSXSerial serial port initialization

**Why it worked in DuckStation but not real hardware:**
- Emulators don't enforce CTS requirement on SIO1 TX
- Emulators don't validate MODE register baud multiplier bits
- Real PS1 silicon enforces both strictly

**Known issues on real hardware (not yet fixed):**
- Memory card detection fails (SIO0 issue, separate from SIO1)
- Memory cards show as "Empty" despite being present in both slots
- Works fine in DuckStation — needs SIO0 timing investigation

**Files changed (targeted fixes only):**
- `bootloader/src/serial.c` — SIO1_MODE and SIO1_CTRL register values
- `tools/host/blackroo-serial.py` — RTS/DTR assertion in open_serial()

**Research documented:**
- `docs/13-SIO1-HARDWARE-RESEARCH.md` — full investigation with register decodes and sources

### 2026-04-13 — Blackroo Kloader v0.0.1 "Kloader"

**Major milestone:** Custom bootloader with serial shell protocol replaces
dependency on UniROM for serial operations. The PS1 now runs a command server
over SIO1 that supports file upload, memory inspection, kernel launch, and
baud rate negotiation.

**Bootloader (PS1 side):**
- New serial shell protocol (Blackroo Shell Protocol) with 4-byte command tags
- Commands: PING, UEXE, UBIN, EXEC, BOOT, DUMP, PEEK, POKE, FAST, SLOW, REST
- BK>> beacon advertises shell readiness to host
- 2048-byte chunked transfers with byte-sum checksums
- Baud negotiation: 115200 (standard) and 518400 (fast mode)
- Kernel launch with configurable command line (from settings or host override)
- Version display on menu screen ("BLACKROO LINUX v0.0.1")
- PIO flash manager for expansion port cartridges (AM29F010, SST39SF010, etc)
- Persistent settings on memory card (boot source, root device, init, console)
- Hardware detection: CPU, RAM, video mode, memory cards, PIO

**Host tool (blackroo-serial.py):**
- Complete rewrite for Blackroo Shell Protocol
- `sendexe` — upload PS-EXE to UniROM or compatible loader (SEXE protocol)
- `upload` — upload PS-EXE via shell protocol, auto-boot, enter Linux console
- `bin` / `dump` / `peek` / `poke` / `exec` — memory operations
- `ping` — connection test
- `console` — interactive serial terminal (Ctrl+] to exit)
- `monitor` — raw hex dump of serial traffic
- `help` — full usage guide
- `--version` flag, `--fast` for 518400 baud, `--cmdline` override

**Build system:**
- Clean build script: removes all artifacts before compiling
- `build.sh version` to query version
- Build number tracking and validation

**Documentation:**
- `docs/12-SERIAL-SHELL-GUIDE.md` — complete user guide with protocol reference
- `docs/13-SIO1-HARDWARE-RESEARCH.md` — real hardware investigation with sources

**Critical bugs found (real hardware vs emulator):**
- SIO1_MODE was 0x004D (MUL1) instead of 0x004E (MUL16) — wrong baud rate
- SIO1_CTRL was 0x07 (no RTS) instead of 0x27 (RTS asserted) — TX blocked by CTS
- Host tool not asserting RTS/DTR on FTDI adapter
- Source: psx-spx.consoledev.net, PSn00bSDK psxsio.h, NOTPSXSerial source

**New files:**
- `bootloader/src/shell.h` / `shell.c` — serial shell command server
- `bootloader/src/version.h` — version constants

**Changed files:**
- `bootloader/src/serial.h` / `serial.c` — cleaned to pure SIO1 driver
- `bootloader/src/main.c` — shell integration, version display
- `bootloader/src/menu.c` — added missing psxgpu.h include
- `bootloader/build.sh` — clean build with artifact removal
- `bootloader/CMakeLists.txt` — added shell.c
- `tools/host/blackroo-serial.py` — full rewrite

### 2026-04-02 — First Boot: Kernel Running on PlayStation 1

**Major milestone:** Linux kernel boots on PlayStation 1 (DuckStation emulator), displaying
full boot messages on GPU console, detecting memory cards, and reaching the root filesystem
mount stage. This is the first successful boot of Blackroo Linux.

**Boot achievements (confirmed on screen):**
- R3000 CPU detected, MMU routines loaded
- 2048 KB RAM configured (auto-detect working)
- initrd found at 0x000f1000 (65536 bytes)
- GPU console initialized (37x21 mono PSXGPU)
- 32.76 BogoMIPS calibrated
- Memory: 980k/2048k available
- Memory card detected: 127 Kbytes in slot 1
- RAMDISK driver: 16 disks of 128K (fixed from stale 4096K)
- Kernel command line: root=/dev/ram0 init=/linuxrc console=ttyS0,115200 console=tty0

**Kernel fixes (critical):**
- `CONFIG_BLK_DEV_INITRD` was NEVER enabled — entire initrd boot path was compiled out.
  This was the primary reason the kernel could not boot since the Runix era.
- `CONFIG_BLK_DEV_RAM` was disabled — RAM disk driver not compiled
- `CONFIG_BLK_DEV_RAM_SIZE` was stale at 4096 (4MB ramdisks in 2MB RAM) — fixed to 128
- `autoconf.h` was not regenerated from .config — stale values from previous builds
- Kernel command line had `init=/init` but initrd creates `/linuxrc` — fixed
- `memory.c` rewritten with 2/4/8MB auto-detection via RAM probing
- `HOSTCC` in Makefile: use `env -u GCC_EXEC_PREFIX` to prevent cross-compiler interference
- Drivers Makefile: stripped to PS1-relevant only (block/char/misc/video)
- Top-level Makefile: removed dead driver references (net/media)
- `CONFIG_PSX_LARGE_CARD=y` enabled for memory card RAID joining

**Tools created:**
- `mkmemcard` — creates Blackroo-formatted PS1 memory card images (.mcd)
- `mkinit` — generates minimal FLAT binary /linuxrc for initrd testing
- `addpsexe_initrd` — embeds initrd in PS-EXE (accepts _end address for correct placement)
- `go.sh` — launches latest build in DuckStation
- ISO creation via mkpsxiso with SYSTEM.CNF

**Project infrastructure:**
- Local git repository initialized with tagged snapshots at each milestone
- 134MB → 41MB kernel tree cleanup (removed 14 architectures, 32 filesystems, 31 drivers)
- Professional project structure: Makefile, Docker, configs, scripts, docs
- CHANGELOG.md, SOURCE-ATTRIBUTION.md, MANIFEST.md, CONTRIBUTING.md

**Remaining for shell prompt:**
- Initrd placement shifts when kernel size changes (_end moves) — needs linker-based embedding
- Ramdisk driver reports "Couldn't find valid RAM disk image" — ext2 identification issue
- Need FLAT-format BusyBox (ELF won't work on uClinux without MMU)
- Once ramdisk mounts, /linuxrc executes, shell should appear

**Git tags:**
- `v0.2.1-build-verified` — toolchain works, kernel compiles
- `v0.2.2-kernel-cleaned` — 134MB → 41MB
- `v0.2.3-ram-support` — 2/4/8MB auto-detection
- `v0.2.4-initrd-enabled` — CONFIG_BLK_DEV_INITRD fix
- `v0.2.5-bootable-image` — first PS-EXE with embedded initrd
- `v0.2.6-memcard-tools` — memory card image creation
- `v0.2.7-first-boot` — kernel boots, GPU console, memory card detected

### 2026-04-02 — Documentation Foundation

**Added:**
- `docs/` directory with 10 comprehensive technical documents:
  - `00-PROJECT-STATUS.md` — Current state vs. planned features inventory
  - `01-ARCHITECTURE.md` — PS1 R3000A hardware reference (registers, memory map, IRQs)
  - `02-MEMORY-SUBSYSTEM.md` — RAM configurations (2/4/8MB), auto-detection design
  - `03-MEMORY-CARD-STORAGE.md` — SIO0 protocol, bu.c driver analysis, RAID design
  - `04-MULTITAP-WHITEPAPER.md` — Technical whitepaper on 8-card multi-tap expansion
  - `05-PS2-MEMCARD-ANALYSIS.md` — PS2 card compatibility analysis (conclusion: incompatible)
  - `06-BOOTLOADER-DESIGN.md` — PSn00bSDK bootloader design (inspired by PS2 kernelloader)
  - `07-RAMDISK-ROOTFS.md` — InitRD configurations for 2/4/8MB systems
  - `08-BUILD-SYSTEM.md` — Docker build containers and PSn00bSDK integration
  - `09-SERIAL-UPLOAD.md` — UniROM serial upload process and troubleshooting
- `CHANGELOG.md` — This file
- `SOURCE-ATTRIBUTION.md` — Origin and licensing of all code in the project

**Analysis completed:**
- PS2 IOP (R3000A) is CPU-compatible but hardware registers are at completely different addresses — kernel cannot run on PS2 without a full hardware abstraction rewrite
- PS2 memory cards use MagicGate encryption and SIO2 protocol — incompatible with PS1 SIO0
- PicoMemcard RP2040 can simultaneously act as memory card emulator (Core 0 / PIO0) AND serial bridge (Core 1 / UART) — single device for both storage and serial access
- PS2 homebrew can be used as a memory card formatting tool for real PS1 cards

---

## [0.2] - December 2024

> This revision represents the state of the project when active development resumed.
> The kernel compiles and converts to PS-EXE format, but does not successfully boot
> to an interactive shell on hardware.

### Inherited from Runix (pre-2007)
- Linux 2.4 kernel source with MIPS R3000A (no-MMU) architecture support
- PlayStation-specific machine code in `arch/mipsnommu/ps/`
- Memory card block driver (`drivers/block/bu.c`, `bu.h`)
- Serial console driver (`arch/mipsnommu/ps/siocon.c`)
- Interrupt handler (`arch/mipsnommu/ps/irq.c`)
- System timer (`arch/mipsnommu/ps/time.c`)
- FPU emulator (`arch/mipsnommu/ps/math-emu/`)
- Boot entry point (`arch/mipsnommu/ps/kernel/head.S`)
- Kernel configuration (`Config`)

### Added by Blackroo (2024)
- `build_simple.sh` — Modern build script with toolchain detection
- `build.sh` — Comprehensive build script with multiple targets
- `scripts/make_initrd.sh` — InitRD creation (requires root)
- `scripts/make_initrd_noroot.sh` — InitRD creation (uses genext2fs)
- `scripts/device_table.txt` — Device node definitions for genext2fs
- `tools/elf2psx.c` — ELF to PS-EXE converter (new, not from Runix)
- `tools/addpsexe_initrd.c` — Add initrd to PS-EXE (new)
- `readme.md` — Project documentation
- `roadmap.md` — Technical roadmap and specifications
- Pre-built output files in `output/`

### Known issues at 0.2
- RAM hardcoded to 2MB in `prom/memory.c` — no 4MB/8MB support
- Memory card driver limited to 2 direct slots (`BU_MINORS=2`)
- Multi-tap addressing not implemented (floor always 0)
- InitRD boot sequence does not complete to shell
- PS-EXE padding tools (`exefixup.exe`) are Windows-only
- BusyBox binary format (ELF vs FLAT) unverified for uClinux

---

## [0.1] - Runix Original (circa 2001-2007)

> The original Runix project. Most source code and design decisions come from this era.
> The project was abandoned around 2007 with an incomplete root filesystem.

- Initial Linux 2.4 port to PlayStation 1
- MIPS R3000A no-MMU (uClinux) kernel
- Memory card block driver with RAID support (`CONFIG_PSX_LARGE_CARD`)
- Serial console over SIO1
- GPU console output
- EGCS 2.91.66 cross-compiler toolchain
- elf2ecoff and addinitrd tools

---

## Format

Each entry follows:
```
### YYYY-MM-DD — Brief Description

**Added/Changed/Fixed/Removed:**
- What changed
- Why it changed (if not obvious)
- Source: [where the code/idea came from]
```

### 2026-08-23 — 8 MB RAM sourcing resolved

**Findings:**
- Twelve candidate parts/modules screened. Two chips confirmed correct and
  pin-for-pin identical: `KM48V2104AJ-6` (+ALJ/ALLJ/ASLJ) and `M5M4V17805CJ-6`.
- Confirmed donor module: Kingston **KTM2MX72S**, 8x `M5M4V17805CJ-6S`.
- Spec verified from the Mitsubishi datasheet: 2M x 8, 16 Mbit, EDO, 3.3 V,
  **2048 refresh cycles / 32 ms (A0~A10)**, 28-pin SOJ, 60 ns.
- Derived four screening rules needing no datasheet: 28 leads; only 8 MB/4-chip
  or 16 MB/8-chip modules; the `V` = 3.3 V letter; A0-A10 address pins.
- Root cause of the failed first attempt: `M5M417805CJ-6` is the **5 V** sibling
  of `M5M4V17805CJ-6`. One character.

**Corrected:**
- `docs/01` — added the buy list, the verified pinout equivalence with
  KM48V2104, package/suffix rules, donor-module research and the 5 V trap.
- `docs/02` — `RAM_SIZE 0b111` was marked "dev kit only"; wrong. psx-spx lists
  DTL-H2000/H2500 as 8 MB **single**-bank. Dual-bank came from arcade hardware,
  and 16 MB has now been done on retail by TunerTom.
- `docs/03` — memory-card protocol ceiling is **8 MB** (16-bit frame address),
  not 128 KB; but ~22 KB/s makes it bulk storage, not a root filesystem.
- `docs/18` — the PIO bus is **16-bit** and the connector is **3.3 V**, not
  8-bit/5 V as previously written. See new `docs/21`.

**Added:**
- `docs/21-PIO-PORT-REFERENCE.md` — full 68-pin pinout, ~80 ns timing, DMA
  channel 5, the I2S audio input, both boot hooks.
- `docs/22-WIRELESS-LINK-AND-CLUSTERING.md` — wireless link play, matchmaking
  architecture, the link-cable game list ranked by latency tolerance, the
  SIO1->SIO0 redirect idea, and an honest assessment of console clustering.

**Decision:** two consoles at 8 MB rather than one at 16 MB.
