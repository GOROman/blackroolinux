# Decisions Register

> Every key decision in one place. One line each.

---

## Format

```
DECIDED: [what] — WHY: [one-line reason] — SEE: [source]
```

## Architecture Decisions

```
DECIDED: interactive access is an in-kernel monitor (BRMON) on SIO1, not a userspace shell
  — WHY: CONFIG_BINFMT_FLAT only + no binfmt_elf built, so the ELF BusyBox can never exec;
    a kernel-context shell needs no initrd and fits a stock 2 MB console
  — SEE: docs/20-SERIAL-MONITOR.md
DECIDED: the CD boots the kernel directly (BIOS -> KERNEL.EXE), kloader menu is the second disc
  — WHY: the BIOS loader does the copying, so nothing of ours is resident and there is no
    self-overwrite; kloader's own CD path is broken until it is relinked
  — SEE: docs/19-BOOTABLE-CD.md
DECIDED: kernel command line may come from 0x80000180 tagged "BRCL", else compiled-in default
  — WHY: BIOS CD boot passes no arguments; kloader can pass one, but raw RAM must not be
    mistaken for a command line
  — SEE: arch/mipsnommu/ps/prom/cmdline.c
```

## Technology Choices

```
DECIDED: default build target is a stock 2 MB console — WHY: modchipped stock hardware is the
  "mass use" case; 8 MB stays a build option — SEE: configs/kernel/blackroo_2mb_defconfig
DECIDED: FPU emulator off by default — WHY: 47 KB of text for something no planned userspace
  needs (soft-float); softfp.o now builds unconditionally for the fallback path
  — SEE: GR-002, arch/mipsnommu/kernel/Makefile
DECIDED: 32-bit EGCS toolchain runs via sdk/toolchain-local + sdk/i386-runtime — WHY: this host
  has no 32-bit loader and no root; Docker is gone — SEE: sdk/setup-local-toolchain.sh
DECIDED: keyboard input comes from BlueRetro (ESP32) emulating the Lightspan device 0x96, not
  from the Pico — WHY: all routes need the SAME kernel driver, so BlueRetro's value is that it
  makes that driver the ONLY variable; a new driver against new firmware is two unknowns with
  no way to bisect — SEE: docs/27-KEYBOARD-BRINGUP.md
DECIDED: the CD-ROM block driver ships stage 1 synchronous/polled before the interrupt-driven
  streaming machine — WHY: it is the same path BRMON already proved on hardware, so it can be
  shown correct; docs/24 5.6's state machine is a second unknown stacked on a first
  — SEE: drivers/block/psxcd.c header
DECIDED: Pause only on a discontinuity, never per request — WHY: a Pause costs ~32 ms at 2x and
  PSn00bSDK reports the controller ignores commands for ~1 s afterwards, so pausing per request
  would be slower than the drive — SEE: psxcd_hw_read()
DECIDED: the CD is the read-only system disk and the memory cards are the writable volume
  — WHY: a ramdisk root is a fixed cost that never shrinks; a CD root is served by the elastic
  buffer cache. 700 MB read-only / plus 381 KB of /dev/bul as /home — SEE: docs/28
DECIDED: userspace grows as OUR monolithic binary first, BusyBox second — WHY: they are
  different products (System+Finder vs a Unix userland), and ours is blocked on nothing while
  BusyBox needs a MIPS I uClibc toolchain that prebuilts cannot supply — SEE: docs/28
DECIDED: the desktop follows Mac System 1 — WHY: identical constraints (no protection, one
  program at a time, Finder->app->Finder) and QuickDraw's primitive model maps onto the PS1
  GPU's display lists, which no framebuffer toolkit does — SEE: docs/28
DECIDED: kloader video modes are walked live with L2/R2 and boot in whatever is current
  — WHY: copied from the PS2's own loader; it is self-recovering, so a mode the television
  cannot display needs no dialog to escape - press again. No confirm, no revert timer
  — SEE: bootloader/src/video.h
DECIDED: the menu logo is the kernel's own Tux, converted from include/linux/linux_logo.h
  — WHY: it is the logo of the kernel kloader boots, and no artwork had to be invented;
  the flood-fill that lifts it off its grey swirl is in tools/mklogo.py
  — SEE: bootloader/src/logo.h
DECIDED: kloader is debugged by linking a test build at 0x80090000 and uploading it from the
  running kloader — WHY: at its normal 0x80010000 the upload overwrites the loader doing the
  uploading (GR-008), so the alternative is a CD-R per attempt; this is 7 seconds
  — SEE: -DBLACKROO_TEST_HIGH in bootloader/CMakeLists.txt
DECIDED: BlueRetro's dev_mode is set by patching its flash, not via the Web Bluetooth app
  — WHY: the config app needs Chrome (not installed) and the adapter does not advertise over
  BLE while a keyboard is connected; /fs/config.bin has a magic and no checksum, so
  out_cfg[0].dev_mode is two bytes at magic+8 in the storage partition @0x310000
  — SEE: hardware/blueretro/README.md, GR-012
DECIDED: BlueRetro flashed with v25.04 hw1 UNIVERSAL, not the playstation build — WHY: keeps the
  adapter usable on other consoles; cost is that it auto-detects and so must be attached to a
  powered console to settle on a system — SEE: hardware/blueretro/README.md
DECIDED: userspace output goes to /dev/console (the VT, hence the GPU) with do_con_write()
  mirroring to SIO1; input stays on /dev/brcon until a keyboard exists — WHY: /dev/console has
  no input side without a keyboard driver, so a read there would never return
  — SEE: GR-011, drivers/char/console.c
```

## Process Decisions

```
DECIDED: the disc ships the complete source, not a written offer — WHY: GPL v2 §3 allows either,
  but (b) is a promise and (a) is a fact; the disc had 675 MB spare and the source is 7.6 MB.
  Verified by extracting SOURCE.TGZ back OUT of the disc image and building a kernel from it
  — SEE: scripts/make-source-dist.sh, iso/README.TXT
DECIDED: the source archive refuses to ship if it contains any ELF binary — WHY: excludes are
  easy to get subtly wrong (an anchored ./build does not match bootloader/build), and the first
  run leaked a third-party BusyBox binary, which would have created a GPL obligation this
  project has no reason to take on — SEE: scripts/make-source-dist.sh
DECIDED: the EGCS toolchain is NOT on the disc — WHY: it is a build tool, not part of this work,
  and shipping GCC binaries would oblige us to supply GCC's source too. PSn00bSDK takes the same
  position about its own bundled toolchain — SEE: SOURCE-ATTRIBUTION.md
DECIDED: each kloader CD-boot entry hard-codes its own root=/init=/console= and ignores System
  Settings — WHY: the three have to agree with WHICH kernel is booting and getting it wrong
  fails silently (KERNEL.EXE's built-in ramdisk just wins over root=/dev/psxcd). The menu text
  is now the truth, not a hint — SEE: CDBOOT_PROFILE_* in bootloader/src/kernel.h
DECIDED: psxcd probes the disc only when it is the root device — WHY: the lookup is polled CD
  reads inside an initcall; every loop is bounded, but a wrong bound would hang EVERY boot from
  the disc including the ramdisk one. No root=/dev/psxcd, no probe — SEE: psxcd_init()
DECIDED: the userspace window is ONE number, in asm/blackroo-user.h — WHY: it was three
  (prom/memory.c, binfmt_fixed.c, blackroo.ld), two of them carrying comments asking the reader
  to keep them in step by hand, and they HAD already drifted — userland/build.sh's header
  documented 0x801c0000/256 KB for code using 0x001f0000/64 KB. build.sh now fails on drift
  — SEE: include/asm-mipsnommu/blackroo-user.h, GR-021
DECIDED: measure the lever before ordering the plan by it — WHY: docs/28 put compiler flags
  first on a 5-15% estimate that was out by a factor of thirty, and the test that disproved it
  took nine seconds. The 96 KB all came from lever 2 — SEE: docs/28, logs/sizepass/SUMMARY.md
DECIDED: a milestone is not done until a dated capture sits in docs/captures/ naming where it
  ran — WHY: the pid-1 shell was reported done with no evidence on file, in a tree that has a
  guardrail about stages that "succeeded" while doing nothing — SEE: GR-010
DECIDED: prove new SIO0 hardware in BRMON before writing a driver — WHY: the monitor is the only
  place on this machine where SIO0 has no other users (no interrupts, no bu.c, no scheduler), so
  a failure there is wiring, not software — SEE: docs/27, brmon.c cmd_kbd
```

## Rejected Alternatives

```
REJECTED: -Os (and every other flag) as the kernel size lever — WHY: MEASURED at 2,400 bytes,
  0.32%, against docs/28's estimate of 35-100 KB. EGCS 2.91.66's cc1 does carry optimize_size,
  so the flag works; the compiler simply predates the passes that make it matter. -O1 is 23 KB
  WORSE than -O2. Kept anyway because it is free — SEE: logs/sizepass/SUMMARY.md
REJECTED: stubbing /dev/random rather than removing it — WHY: a /dev/random that returns
  predictable bytes is worse than one that does not exist; anything wanting entropy should fail
  to link, not quietly succeed. The entropy CONTRIBUTORS became empty inlines because they sit
  on hot paths; the CONSUMERS were left undeclared on purpose — SEE: include/linux/random.h
REJECTED: removing mmnommu/vmscan.o for 10 KB — WHY: docs/28's entire case for a CD root is
  that the buffer cache is elastic, and vmscan is the code that shrinks it. Removing it would
  save 10 KB and break the plan — SEE: logs/sizepass/SUMMARY.md
REJECTED: removing fs/locks.o for 14 KB — WHY: needs ~12 stubs and changes what fcntl MEANS.
  A lock that silently succeeds is a worse bug than 14 KB is a cost — SEE: logs/sizepass/SUMMARY.md
REJECTED: an emulator as the smoke test for this kernel — WHY: loading the PS-EXE with
  -loadexe under OpenBIOS and no disc dies instantly on garbage, which says nothing about the
  kernel. This tree's verification is hardware over serial; see GR-010 on captures
REJECTED: Spectrum adaptor v2 keyboard protocol (device ID 0xE8) — WHY: documented to answer ANY
  0x01 byte on the bus rather than just the first, which would collide with the memory cards
  — SEE: docs/14 Finding 1c
REJECTED: homebrew mouse-keyboard protocol (device ID 0x12) — WHY: one scancode per poll caps at
  ~60 keys/sec and drops keys for fast typists — SEE: docs/14 Finding 3
REJECTED: hand-writing userspace in MIPS assembly to save space — WHY: fixed 32-bit
  instructions and no compressed encoding (MIPS16 postdates the R3000), so the density is not
  there; -Os and not linking unused subsystems are worth 10x the saving for a fraction of the
  effort — SEE: docs/28
REJECTED: cloning BusyBox — WHY: a clone is only ever a worse BusyBox; our own binary is a
  different product (an integrated System+Finder), not a competing one — SEE: docs/28
REJECTED: rewriting serial_psx.c with interrupt-driven TX to "fix the tty layer" — WHY: the TX
  path was never the fault; /dev/console was resolving to the serial tty because
  vt_console_driver was misnamed "ttyS" — SEE: GR-011
```

---

*Last updated: 2026-08-26*
