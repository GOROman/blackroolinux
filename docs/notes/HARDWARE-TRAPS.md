# Guardrails

> Append-only lessons learned file. Check before repeating work.

---

## Format

```
### GR-NNN: [Title]
- Date: YYYY-MM-DD
- Domain: [domain tag]
- What happened: [description]
- Cost: [time/tokens/other impact]
- Rule: IF [condition] THEN [action]
```

---

## Entries

### GR-023: a fix applied to a GENERATED file is not a fix
- Date: 2026-08-26
- Domain: kernel / build
- What happened: on 2026-08-21 the kernel was relinked from `0x80010000` to
  `0x80090000` to stop it overwriting kloader during a serial upload (GR-008).
  That fix was made by editing `arch/mipsnommu/ld.script`. But `ld.script` is
  **generated** from `ld.script.in` by a rule in `arch/mipsnommu/Makefile`, and
  the `clean` target in that same Makefile **deletes it**. `LOADADDR` in the
  Makefile still said `0x10000`. So the first `make clean` after the fix
  silently reverted it - which happened during the 2026-08-25 size pass, four
  days later. Every kernel built that night linked at `0x80010000` and would
  have died at upload offset 6144, exactly as GR-008 describes.
- Cost: caught only because a failed upload made me read the PS-EXE header. The
  size pass would otherwise have been blamed for a broken kernel.
- Rule: IF you edit a file THEN check it is not generated - `git status`
  showing it as modified is not proof, and `grep -rn '<filename>' Makefile`
  takes a second. Fix the *input*, never the output. `LOADADDR` now lives in
  `arch/mipsnommu/Makefile` where `clean` cannot reach it, and
  `iso/build-iso.sh` prints the load address of every PS-EXE it packs.

### GR-024: a tool that exits 0 on failure makes every script that drives it lie
- Date: 2026-08-26
- Domain: tooling / serial
- What happened: `tools/host/blackroo-serial.py` discarded `cmd_upload()`'s
  return value, so `upload` exited 0 after printing
  "ERROR: No beacon received" and sending nothing. A watcher script that
  checked the exit code reported "UPLOADED - kernel is running" for an upload
  that never happened.
- Cost: one false milestone, reported out loud before it was checked.
- Rule: IF a script decides something happened from an exit code THEN verify
  the tool actually sets it. Related and just as easy to get wrong: `$?` after
  a pipeline is the **last** command's status, so `cmd | tail -3; echo $?`
  reports `tail`'s success and hides the failure. Check the tool's own exit
  code, or check its output text, and prefer both.

### GR-025: a long build path inflates the kernel
- Date: 2026-08-26
- Domain: build / measurement
- What happened: the source tarball, extracted to a scratch directory and
  built, produced a kernel **31,232 bytes larger** than the same source in the
  project tree - from byte-identical `.c` and `.h` files, identical `.config`,
  and identical compiler command lines. It looked like the source distribution
  did not correspond to the shipped binary, which would be a GPL problem.
  It was not: GCC embeds `__FILE__` as an **absolute path**, this 2.4 tree has
  485 of them in the linked image, and the scratch path was 74 characters
  deeper. 485 x 74 accounts for the whole difference.
- Cost: a long detour, and a wrong claim made out loud before it was checked.
- Rule: IF two builds of identical source differ in size THEN compare the build
  *paths* before suspecting the source. Any size comparison must be made at the
  same path depth. `strings vmlinux | grep <path prefix> | wc -l` shows how
  many are embedded.

### GR-019: a second plain declaration silently defeats a static inline
- Date: 2026-08-25
- Domain: kernel / headers
- What happened: turning `add_blkdev_randomness()` into an empty
  `static inline` in `<linux/random.h>` (so `random.o` need not be linked) did
  not take for `bu.c`. `<linux/blk.h>` re-declared the same function itself -
  `void add_blkdev_randomness(int major);` - a few lines below where it should
  have included the header. That plain declaration wins, so the call stayed
  external and only that one file failed to link. Harmless for as long as
  `random.o` was always linked, which is why it had sat there for years.
- Cost: one confusing link failure that pointed at the wrong file.
- Rule: IF you convert a kernel function to a `static inline` in its header
  THEN `grep -rn '<name>' include/` for a second declaration BEFORE building.
  One `undefined reference` in an unexpected file is the signature.

### GR-020: mke2fs writes a short file; the superblock does not know
- Date: 2026-08-25
- Domain: initrd / filesystem
- What happened: `scripts/make-userspace-initrd.sh` asked for a 64-block
  filesystem and got a 42 KB *file* - `mke2fs` and `debugfs` stop writing at the
  last used block. `e2fsck -fn` says so plainly: "The filesystem size (according
  to the superblock) is 64 blocks / The physical size of the device is 42
  blocks". After `rd_load` the ramdisk is only as big as the image, so any block
  the filesystem allocates past 42 is past the end of the device. Invisible for
  as long as `brsh` had only `help`/`echo`/`exit`; reachable the moment it grew
  `mkdir` and `cp`.
- Cost: caught before hardware test, by running `e2fsck` on an image that had
  already shipped several times.
- Rule: IF you build a filesystem image THEN `truncate -s` it to the size its
  superblock claims, and `e2fsck -fn` it. A tool that "succeeded" is not
  evidence; the physical-size complaint is.

### GR-021: one number in three files is not one number
- Date: 2026-08-25
- Domain: build / userspace
- What happened: the fixed userspace window was `#define`d in
  `fs/binfmt_fixed.c` and in `arch/mipsnommu/ps/prom/memory.c`, each with a
  comment asking the reader to keep them in step by hand, plus an unmentioned
  third copy in `userland/blackroo.ld`. They had already drifted: the header
  comment in `userland/build.sh` documented `0x801c0000, 256 KB` for code that
  actually used `0x001f0000, 64 KB`. Nothing detected it because nothing could.
- Cost: none yet - found while growing the window. A mismatch would have shown
  as `binfmt_fixed` refusing a binary, or worse, accepting one that overlaps
  memory the allocator still hands out.
- Rule: IF a constant has to agree across the kernel, a link script and a build
  script THEN put it in a header and make the build FAIL on disagreement - do
  not write a comment asking the next person to remember.
  `userland/build.sh` now checks `blackroo.ld` against the defconfig.

### GR-022: verify a size estimate before ordering the plan by it
- Date: 2026-08-25
- Domain: process / build
- What happened: `docs/28` listed compiler flags as the first lever of the
  kernel size pass, estimating `-Os` at "5-15% of text - 35-100 KB". Measured on
  a full rebuild: **2,400 bytes, 0.32%** - out by a factor of thirty. `-O1` is
  23 KB *worse* than `-O2`. The whole 96 KB that the size pass eventually found
  came from lever 2, not linking unused subsystems.
- Cost: none - the document did say "test this before assuming it", and the test
  took nine seconds. The cost would have been ordering weeks of work by it.
- Rule: IF a plan is ordered by estimated payoff THEN measure the cheapest
  estimate first and re-order. A full kernel rebuild in this tree is ~9 seconds;
  there is no excuse for guessing.

### GR-017: a primitive drawn without terminating its DMA list hangs the GPU
- Date: 2026-08-22
- Domain: kloader / GPU
- What happened: adding the Linux logo to kloader's menu made the burned disc
  hang after the PlayStation logo. The GPU walks a DMA linked list - each
  primitive's tag holds a 24-bit "next" address, terminated by 0xffffff - and
  `setPolyFT4()` / `setDrawTPage()` fill in the length and command code but NOT
  that pointer. The primitives lived in .bss, so the GPU followed whatever
  bytes were there and never came back.
- Cost: one CD-R, plus a long detour verifying the licence area, the burn, the
  media and the write speed - every one of which was fine.
- Rule: IF drawing a primitive outside an ordering table THEN chain it with
  `catPrim()`, end the list with `termPrim()`, and draw with `DrawOTag()`. Note
  where this surfaces: `logo_draw()` returned normally and the hang appeared in
  `FntFlush()`, the next unrelated function to touch the GPU. A wedged GPU
  blames the wrong code.

### GR-018: a default nothing reads is a landmine, not a default
- Date: 2026-08-22
- Domain: kloader / boot
- What happened: `settings_default()` had `root_device = ROOT_DEV_CARD_RAID`,
  harmless only for as long as `cdrom_boot()` used a hard-coded
  `"root=/dev/ram0 ..."` string and never consulted settings. Changing that
  function to build its command line from settings - so an L2/R2 video mode
  could reach the kernel - silently promoted the untested default to the boot
  device, and Linux panicked mounting memory cards with no filesystem on them.
- Cost: a burned disc that ran kloader but could not boot Linux, and time spent
  suspecting the disc rather than the settings.
- Rule: IF a code path starts consuming configuration it previously ignored
  THEN audit every default it now depends on before testing. The dangerous
  fields are exactly the ones nothing has ever read: they have never been
  wrong, so nobody has ever checked them.

### GR-016: trace the protocol, do not infer it from the symptom
- Date: 2026-08-21
- Domain: process / driver bring-up
- What happened: the keyboard driver produced an unbroken column of repeated
  key-down events. The obvious reading — "the adapter re-serves its last report
  every poll" — led to de-duplicating identical consecutive frames. That
  stopped the flood and quietly broke typing: two identical frames are two
  genuine keypresses, so "hello" came out "helo". Making the driver dump raw
  frames settled it in one run: 244 `nn=0`, 237 `nn=1: 59`, 26 `nn=2: f0 59` —
  make and break codes, one event per frame, an event stream. The flood had
  been ordinary typematic autorepeat of a held key, and it only looked
  pathological because a debug printk was echoing every event to the screen.
- Cost: two hardware cycles, and a fix that traded a visible bug for a subtler
  one.
- Rule: IF two explanations of a device's behaviour imply opposite fixes THEN
  spend the cycle that distinguishes them before writing either. Print what
  arrives on the wire, not what the driver made of it — a decoded trace shows
  your interpretation, and the interpretation is the thing in question.

### GR-015: build.sh's default config silently switched the kernel to 8 MB
- Date: 2026-08-21
- Domain: build / memory
- What happened: a session that began from a previously-built 2 MB image
  (`PSX: 1984 KB RAM configured`) started producing 8 MB kernels the moment
  anything was rebuilt — `PSX: 8128 KB RAM configured (reg=0x8fbf0b88)` — on a
  console with 2 MB. `build.sh` defaults `DEFCONFIG` to
  `blackroo_8mb_defconfig`, and `setup_config()` copies it over `.config` on
  every single build, so nothing the operator had done was preserved and
  nothing announced the change. It kept booting because the kernel, initrd and
  early allocations all sit below 2 MB and PS1 RAM mirrors, which is exactly
  what makes it dangerous: the fault would surface later, as corruption, far
  from its cause.
- Cost: several hardware cycles run against a misconfigured kernel. No damage
  observed, by luck rather than design.
- Rule: IF the boot log's `RAM configured` line does not match the console in
  front of you THEN stop and fix the config before trusting anything else the
  run shows. The default is now `blackroo_2mb_defconfig` (docs/21: both
  consoles are 2 MB stock); a modded machine needs
  `DEFCONFIG=blackroo_8mb_defconfig ./build.sh kernel`. More generally: a build
  script that overwrites `.config` from a default every time makes `.config`
  a lie — read the *boot log*, not the config file, to learn what you built.

### GR-013: a device behind a multitap answers a different address
- Date: 2026-08-21
- Domain: SIO0 / multitap
- What happened: the keyboard adapter was plugged into **port B of the multitap**,
  not straight into a console port. A tap addresses its four floors by the
  address byte - 0x01..0x04 for controllers, 0x81..0x84 for memory cards, which
  is exactly what bu.c already does with `0x81 + floor`. Polling 0x01 reached
  tap port A and found the pad living there, so the keyboard looked absent.
  Worse, BRMON's own probe chose its command byte as
  `(addr == 0x01) ? 0x42 : 0x52`, so probing address 0x02 sent the *memory card
  read* command to a keyboard - a probe that could never have worked.
- Cost: a hardware cycle spent concluding "nothing on either port" when the
  device was present the whole time.
- Rule: IF something on SIO0 is missing THEN sweep 0x01..0x04 and 0x81..0x84 on
  both ports before believing it. `kbd scan` does this; psxkbd.c does it
  automatically at its idle rate and reports where it locked on. Pick the
  command byte by address CLASS, never by a single value.

### GR-014: do not reuse a driver's own lock to exclude outsiders
- Date: 2026-08-21
- Domain: SIO0 / locking
- What happened: to stop the keyboard timer corrupting the card detection sweep,
  bu_init() took `psx_sio0_trylock()` - which sets `bu_lock`. The sweep then
  calls `bu_catch()`, which takes `bu_lock` itself, found it held, and failed
  every slot with "can't catch card". `/dev/bul` came up at 0 Kbytes: the fix
  for a lost card lost all of them.
- Cost: one hardware cycle, and a regression that looked exactly like the
  hardware fault it was meant to fix.
- Rule: IF a subsystem needs to exclude *other* drivers THEN give that its own
  flag (`bu_probing`), separate from the lock its internal code paths already
  take. A lock that keeps a subsystem out of itself is a deadlock wearing a
  different name.

### GR-012: an adapter can be healthy, present, and still the wrong device
- Date: 2026-08-21
- Domain: hardware / BlueRetro
- What happened: the BlueRetro adapter worked — Bluetooth up, keyboard paired,
  keys even navigating kloader's menu — and still returned "no keyboard on this
  port". Two independent causes, neither visible from the PS1: (1) the firmware
  was a custom Apr-2023 build **hardcoded to PS2** (`# Hardcoded system : 17:
  PS2`), and BlueRetro fixes the console at compile time while `0x96` is a PS1
  device; (2) after reflashing it answered `41 5a`, a **gamepad**, because
  BlueRetro maps keyboards onto pad buttons unless the port's `dev_mode` is
  `DEV_KB` (`main/wired/ps_spi.c:975`).
- Cost: would have been an evening of chasing wiring that was never wrong. The
  "it works in kloader!" moment was actively misleading — it was working *as a
  pad*.
- Rule: IF a smart adapter does not answer as expected THEN read back what it
  actually claims to be before touching wiring. `sio0 <slot> <addr> <n>` prints
  the raw bytes: `41 5a` gamepad, `96 5a` keyboard, `5A 5D` memory card, all
  `ff` = nothing connected on the Bluetooth side. Three different problems that
  look identical from a driver.

### GR-011: /dev/console is resolved by console *name*, not by driver
- Date: 2026-08-21
- Domain: kernel / console + tty
- What happened: userspace could not write to `/dev/console` - the process slept
  in the line discipline - and three documents recorded the cause as "the tty
  layer has no working transmit path in serial_psx.c". It was not. In
  `drivers/char/console.c`, `vt_console_driver` had been renamed
  `name: "ttyS"` (with a `//???PSX "tty"` comment). `register_console()` sets
  `CON_CONSDEV` only when the console's *name* matches the `console=` entry that
  `preferred_console` points at, so with `console=ttyS0,115200 console=tty0` the
  VT matched the serial entry, nothing got `CON_CONSDEV`, `console_drivers` kept
  the first registrant (siocons), and `tty_open()` resolved `/dev/console` to
  **ttyS0**. Renaming it back to `"tty"` moved `/dev/console` to tty 4:1 - the
  VT - and userspace output appeared on the television immediately.
- Cost: a whole subsystem (`brcon`) written as a workaround, plus "fix the tty
  layer" carried as a multi-session project in docs/26 when it was one string.
- Rule: IF `/dev/console` misbehaves THEN print what it resolved to before
  theorising about drivers - walk `console_drivers` to the first entry with a
  `->device()` and log `MAJOR`/`MINOR`. `init/main.c` now does this every boot.
  More generally: a symptom in the tty layer does not mean the bug is in a tty
  driver; console *selection* happens by string match in `printk.c`.

### GR-009: /dev/ttyUSB0 is not the console — find the port that beacons
- Date: 2026-08-21
- Domain: host tooling / serial
- What happened: every doc and script in this tree hardcodes `/dev/ttyUSB0` for
  the PS1. With kloader sitting in `Serial Shell (115200)` and beacons being
  sent, `ttyUSB0` read **zero bytes** across two attempts. The console was on
  `/dev/ttyUSB1` — the FTDI adapter — because USB serial devices are numbered in
  plug order and this box has two adapters (the other belongs to PineCore).
- Cost: two dead ten-second listens and a wrong "the console isn't talking"
  hypothesis that was one step from power-cycling a perfectly healthy console.
- Rule: IF talking to the PS1 THEN identify the port by listening for a `BK>>`
  beacon on each `/dev/ttyUSB*` first, never by name. The wrong port is *silent*,
  which is indistinguishable from a dead console, a wrong baud, or a bad cable —
  so rule it out before diagnosing anything else.

### GR-010: a milestone without a capture is a claim, not a result
- Date: 2026-08-21
- Domain: process / verification
- What happened: `docs/25` documented a userspace shell running as pid 1, with a
  transcript, and `SESSION-STATE.md` reported it as done — but `docs/captures/`
  held nothing for it, and the burned CD-R predated the kernel that contained
  the feature. The work turned out to be genuine (reproduced in emulation, then
  on hardware the same day), but nothing in the tree distinguished it from the
  three earlier initrd stages that "succeeded" while doing nothing. Separately,
  three docs listed a `root_dev_names[]` fix as the next task; the fix had been
  in `init/main.c` for a day.
- Cost: an hour re-deriving a result that was already correct, and a next-stage
  plan whose first item was a no-op.
- Rule: IF a milestone is claimed THEN a dated capture goes in `docs/captures/`
  naming where it ran (`-emulator` or hardware), and any "next fix" recorded in
  a handoff doc gets re-checked against the source before it is worked on.
  Claims age; the tree is the truth.

### GR-008: kloader cannot load the kernel at all — it overwrites itself
- Date: 2026-08-21
- Domain: bootloader / memory map
- What happened: kloader's "Boot from CD-ROM" hung on hardware, and so did a
  serial upload of the kernel — the upload died at offset **6144, deterministically,
  twice**, and once more with byte-level pacing that made no difference. Both paths
  write the payload to `hdr.t_addr`, and the kernel's load address (`0x80010000`)
  is *inside kloader's own text* (`0x80010000..0x8001E800`). The first three 2 KB
  chunks overwrite startup/menu code that isn't executing; chunk 4 reaches
  `0x80011800`, something live in the receive path, and the machine dies.
- Cost: a wrong diagnosis (serial flow control) and a workaround tool that could
  never have helped, plus a frozen console per attempt.
- Rule: IF kloader must load a payload THEN the payload's load address must not
  overlap `0x80010000..0x8001E800`. Until kloader is relinked high (docs/19 §3),
  the ONLY way to run the kernel on hardware is BIOS CD boot. Note the earlier
  April uploads worked because they targeted UniROM, which lives elsewhere.

### GR-005: mkpsxiso 2.20 writes a licence area a console will not boot
- Date: 2026-08-20
- Domain: CD authoring / hardware boot
- What happened: discs built with mkpsxiso 2.20 hung a modchipped PS1 at the SCE
  screen, spinning forever, never drawing the PlayStation logo. Everything
  checked out against a known-booting disc — TOC control flags (data), CD-XA001
  in the PVD, identifiers, licence sectors 4-11 byte-identical — except **disc
  sectors 12-15**, the Form-2 tail of the licence area. 2.20 leaves those sector
  bodies empty; the older mkpsxiso that built the discs this console boots starts
  each body with `00 00 08 00 00 00 08 00`. Same result whether the licence came
  from `<license file=...>` in the XML or the `-L` command-line override.
  The BIOS draws the PlayStation logo from that data, which is exactly where it
  hung. Transplanting sectors 0-15 verbatim from a known-booting image fixed it —
  kloader booted off CD immediately after.
- Cost: 4 blank CD-Rs and about four hours, most of it chasing the kernel.
- Rule: IF a disc must boot on real hardware THEN its first 16 sectors must come
  from a known-booting image. `iso/build-iso.sh` does this automatically via
  `iso/license-area.bin`; if that file is missing, do not trust the disc.

### GR-006: verify the modchip with a known-good CD-R before burning anything
- Date: 2026-08-20
- Domain: process / hardware testing
- What happened: a retail disc (THPS2) was taken as proof the console would boot
  burned discs. It proves nothing — pressed discs need no modchip. Three discs
  were burned and a kernel was modified on the assumption that the CD-R path
  worked, before anyone booted a CD-R known to have worked before.
- Cost: 3 of the 4 wasted discs, plus kernel changes made against a phantom.
- Rule: IF hardware CD boot fails THEN boot a CD-R that has worked on that
  console before, as step one, BEFORE changing code or burning again. A retail
  disc is not that test.

### GR-007: raw Mode 2 readback on this drive returns zeroes
- Date: 2026-08-20
- Domain: tooling
- What happened: `cdrdao read-cd --read-raw` on the TSSTcorp SN-208FB returned
  0.6% non-zero data and warned "cannot determine data format" — which looked
  like a catastrophically bad burn. Reading the same disc through the kernel CD
  driver (`dd if=/dev/sr0 bs=2048`) showed a perfect PVD and SYSTEM.CNF.
- Cost: ~15 minutes chasing a nonexistent burn failure.
- Rule: IF verifying a burned PS1 disc THEN read it with `dd if=/dev/sr0 bs=2048`
  (kernel driver, Form-1 user data), not this drive's raw mode.

### GR-001: The kernel config was decorative — autoconf.h was hand-maintained
- Date: 2026-08-20
- Domain: build system / kernel config
- What happened: `configs/kernel/*_defconfig` is copied to `blackroo/.config`,
  but this stripped 2.4 tree has no working `make oldconfig`, so nothing ever
  regenerated `include/linux/autoconf.h` — and that file is what the C code
  actually reads. The checked-in copy was last touched by hand in April and had
  drifted: 31 symbols disagreed with the 2 MB defconfig, including
  `CONFIG_PSX_2MB_RAM` (config y / autoconf n) and `CONFIG_PSX_LARGE_CARD`
  (config n / autoconf y). Every "we changed the config" result before this date
  is suspect.
- Cost: unknown, potentially months of misleading test results.
- Rule: IF you change a kernel config option THEN confirm it landed in
  `blackroo/include/linux/autoconf.h`. `build.sh` now regenerates that file from
  `.config` on every build (`gen_autoconf`); never hand-edit it.

### GR-002: A config change with stale .o files links the *old* config
- Date: 2026-08-20
- Domain: build system
- What happened: after turning `CONFIG_MIPS_FPU_EMULATOR` off, the link failed
  on `fpu_emulator_cop1Handler` — from a `traps.o` compiled while it was still
  on. This tree deliberately skips `make dep`, so make cannot see that
  `autoconf.h` changed.
- Cost: ~20 minutes chasing a phantom "unguarded call site" that was correctly
  `#ifdef`-ed all along.
- Rule: IF `autoconf.h` content changes THEN delete every `*.o` and `*.a` under
  `blackroo/` before building. `build.sh` does this automatically now.

### GR-003: PS-EXE stack pointer was 8 MB-only
- Date: 2026-08-20
- Domain: boot / PS-EXE
- What happened: `tools/elf2psexe.c` hard-coded `sp_base = 0x807fff00` — the top
  of an 8 MB mod, which is not addressable on a stock 2 MB console. The BIOS
  honours this field when it loads a PS-EXE from CD, so a "2 MB build" would have
  started with a stack pointer into nothing.
- Cost: caught before hardware test.
- Rule: IF you build a PS-EXE for a stock console THEN the stack must be
  `0x801fff00`. `build.sh convert` now picks it from `CONFIG_PSX_*MB_RAM`, and
  `elf2psexe` defaults to the 2 MB value.

### GR-004: RAM size was hard-wired to 8 MB in memory.c
- Date: 2026-08-20
- Domain: kernel / memory
- What happened: `arch/mipsnommu/ps/prom/memory.c` set `*mem_size_reg = 0xB88`
  and `mem_size = 8 << 20` unconditionally, with the config-driven block sitting
  inside `#if 0`. On a stock console the kernel would hand the allocator 6 MB of
  RAM that does not exist.
- Cost: caught before hardware test.
- Rule: IF a "2 MB" image is built THEN check the boot line reads
  `PSX: 2048 KB RAM configured (reg=0x0888)` before trusting anything else.

---

*Last updated: 2026-08-25*
