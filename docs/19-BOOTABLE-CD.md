# Bootable CD-ROM for Blackroo Linux

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: UniROM as the upload path.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> Status: **disc images build and are structurally valid; boot not yet verified
> in an emulator or on hardware.** Written 2026-08-20.

Until now every way of getting Blackroo onto a PlayStation needed a host PC in
the loop: UniROM + serial upload, or the FreePSXBoot memory-card exploit. A boot
CD removes the host entirely — put the disc in, the BIOS runs our code.

---

## 1. What the PS1 BIOS actually needs from a disc

| Requirement | How we satisfy it |
|---|---|
| ISO9660 filesystem in **CD-XA Mode 2/2352** (not a plain 2048-byte ISO) | `mkpsxiso` 2.20, `<track type="data">` |
| **Licence area** — first 16 sectors of the track | `iso/license.dat` (see §5) |
| **`SYSTEM.CNF`** in the root directory naming the boot executable | `iso/SYSTEM.CNF` |
| A **PS-EXE** (2048-byte header + payload) at that path | `output/bootloader.exe`, `output/blackroo.exe` |
| 8.3 filenames with `;1` version suffix | `BLACKROO.EXE;1`, `KERNEL.EXE;1`, `LINUX.EXE;1` |

`SYSTEM.CNF` we emit (CRLF line endings, as retail discs use):

```
BOOT=cdrom:\BLACKROO.EXE;1
TCB=4
EVENT=10
STACK=801FFF00
```

`STACK` matters here: `bootloader.exe` has `sp = 0x00000000` in its PS-EXE
header, so the BIOS takes the stack pointer from `SYSTEM.CNF` instead. The
kernel PS-EXEs carry their own `sp = 0x807FFF00`.

---

## 2. The two disc images

`./iso/build-iso.sh` produces both:

| Image | `SYSTEM.CNF` boots | Purpose | RAM needed |
|---|---|---|---|
| `output/blackroo.bin` + `.cue` | `BLACKROO.EXE` (kloader) | Boot menu, serial shell, memcard/PIO tools — straight off the disc, no UniROM, no exploit | 2 MB |
| `output/blackroo-direct.bin` + `.cue` | `KERNEL.EXE` (Linux + initrd) | Shortest path to "Linux booted from CD": the BIOS itself streams the kernel in and jumps to it | **8 MB** |

Both discs carry the same three payloads, so one burn covers both experiments if
you rebuild with the other `SYSTEM.CNF`:

```
SYSTEM.CNF        61 B
BLACKROO.EXE      60 KB   kloader v0.0.1        entry 0x8001A178
KERNEL.EXE       2.8 MB   kernel + 2 MB initrd  entry 0x80010388
LINUX.EXE        784 KB   kernel, no initrd     entry 0x80010388
```

---

## 3. Finding: kloader's "Boot from CD-ROM" overwrites itself

`bootloader/src/kernel.c:cdrom_boot()` reads the PS-EXE header off the disc and
then `memcpy`s the payload to `hdr.t_addr` sector by sector. Both binaries are
linked at the same address:

```
kloader        load 0x80010000 .. 0x8001E800   (58 KB, running)
KERNEL.EXE     load 0x80010000 .. 0x802D4000   (2832 KB, being written)
LINUX.EXE      load 0x80010000 .. 0x800D3800   (782 KB, being written)
```

The first ~58 KB written land on top of the loop that is doing the writing. The
menu entry has never been exercised on hardware, so this has not shown up before
— it will crash as soon as it is.

This is *not* a problem for the direct-boot disc: there the BIOS's own loader
(which lives below 0x80010000 and in the BIOS ROM) does the copying, and nothing
of ours is resident.

### Fix options

**A. Relink kloader out of the kernel's way (recommended).**
PSn00bSDK's `exe.ld` takes the load address from a linker defsym, so a
CD-boot build can be linked high:

```cmake
target_link_options(bootloader PRIVATE -Wl,--defsym,TLOAD_ADDR=0x80700000)
```

`TLOAD_ADDR` spelling **VERIFY** against the installed `exe.ld` before relying
on it. Two flavours are needed because the safe "high" address depends on RAM:

| Console | kloader link address | Kernel headroom below it |
|---|---|---|
| 8 MB modded | `0x80700000` | 7 MB — fits `KERNEL.EXE` |
| 2 MB stock | `0x801F0000` | 1.9 MB — fits `LINUX.EXE` + a small initrd |

Catch: the BIOS sets `RAM_SIZE` (0x1F801060) to the 2 MB value (`0x0888`) at
reset, so a disc whose boot PS-EXE loads at `0x80700000` would be written into
mirrored low RAM on a stock BIOS. Hence:

**B. Tiny stage-1 selector at 0x80010000.** A ~1 KB PS-EXE that is the disc's
`BOOT=`: probe RAM, write the right `RAM_SIZE` value (`0x0888`/`0x0988`/`0x0B88`,
see `docs/02-MEMORY-SUBSYSTEM.md`), then hand off with the BIOS call
`LoadExec("cdrom:\\KLOAD8M.EXE;1", ...)` — the BIOS loads the high-linked kloader
to its own `t_addr` and runs it. No CD code of our own needed in stage 1.

A + B together are the intended shape: stage-1 picks 2 MB or 8 MB, the matching
kloader lives above the kernel, and `cdrom_boot()` then copies downward into
empty RAM exactly as written.

**Blocker:** rebuilding kloader needs PSn00bSDK, and `bootloader/build.sh` drives
it through a `blackroo-psn00bsdk` **Docker image — Docker is no longer installed
on this machine.** Either reinstall Docker or install PSn00bSDK natively
(prebuilt release + `mipsel-none-elf` GCC) before doing any of the above.

---

## 4. Testing

Nothing here has been booted yet. Suggested order:

1. **Emulator, kloader disc** — `blackroo.cue`. Expect the Blackroo menu on
   screen. This proves SYSTEM.CNF, the licence area, and PS-EXE layout are all
   acceptable to the BIOS.
2. **Emulator, direct disc** — `blackroo-direct.cue` with **8 MB RAM enabled**
   (DuckStation: Console → "Enable 8MB RAM (Dev Console)"). Expect the kernel's
   GPU console, then the initrd mount and `/bin/sh`.
   With 8 MB *off* this disc is expected to fail — 2.8 MB of payload wraps.
3. **Serial** — same kernel command line as the upload path
   (`console=ttyS0,115200`), so the shell is reachable over the FTDI cable.

Burn to CD-R:

```
cdrecord -v -dao dev=/dev/sr0 cuefile=output/blackroo.cue
```

---

## 5. Licence data and real hardware

> **Canonical cross-project note:** the full treatment of PS1 disc licensing —
> the licence-area layout, the region files, the Form 2 / EOR / EOF submode
> requirements on sectors 12-17, and the legal position — now lives in
> `~/projects/psx-video/docs/PS1-DISC-LICENSING.md`. Read that first; the
> summary below is consistent with it.
>
> Added 2026-08-22 after a `psx-video` disc failed to boot: the licence area was
> zero-filled **and** sectors 12-15 were written as Form 1 instead of Form 2,
> with the PVD/terminator missing their EOR/EOF bits. A structurally wrong
> licence area kills the disc even on a modchipped console — verify sectors 0-17
> against a known-good image before burning.

`iso/license.dat` is 28,032 zero bytes — a structurally correct but empty
licence area. That is fine for emulators and, in practice, for modchipped
consoles; it is *not* a region-stamped Sony licence, which we cannot ship.
If you own a retail disc, `dumpsxiso` will extract its `license_data.dat` and
dropping that file in place restores the proper region string and boot logo.

A burned CD-R has no wobble groove, so an unmodified console will reject it
regardless of licence data. Working paths on real hardware:

| Path | Notes |
|---|---|
| Modchip | Injects the SCEx string; disc boots normally |
| tonyhax International | Swap trick with an original disc, no soldering |
| FreePSXBoot → tonyhax/UniROM | Memory-card exploit, then boot the CD-R |

The CD is therefore not a replacement for the cart-resident boot work in
`docs/18-CART-RESIDENT-BOOT-RESEARCH.md` — a flashed GameShark still gives the
only fully self-contained, unmodified-console boot.

---

## 6. Files added

| File | Purpose |
|---|---|
| `iso/build-iso.sh` | Builds both disc images, sanity-checks payload headers |
| `iso/blackroo_cd.xml` | mkpsxiso project — kloader boot disc |
| `iso/blackroo_direct_cd.xml` | mkpsxiso project — direct kernel boot disc |
| `iso/SYSTEM.CNF` | `BOOT=cdrom:\BLACKROO.EXE;1` |
| `iso/SYSTEM.DIRECT.CNF` | `BOOT=cdrom:\KERNEL.EXE;1` |
| `iso/license.dat` | Zero-filled licence area placeholder |

`bootloader/iso/blackroo_cd.xml` is the earlier draft of this — it references
`output/` filenames that were never produced (`kernel+initrd.exe`,
`kernel_mmu.exe`) and has no `SYSTEM.CNF`, so it cannot make a bootable disc.
Superseded by `iso/`.

---

## Sources

- psx-spx, CD-ROM File Formats / ISO Volume Descriptors — <https://psx-spx.consoledev.net/cdromfileformats/>
- psx-spx, Memory Control (`RAM_SIZE` 0x1F801060) — <https://psx-spx.consoledev.net/memorycontrol/>
- mkpsxiso 2.20 XML schema — <https://github.com/Lameguy64/mkpsxiso>
- PSn00bSDK `LoadExec`/`Exec` BIOS wrappers, `exe.ld` load address — <https://github.com/Lameguy64/PSn00bSDK>
- tonyhax International — <https://github.com/socram8888/tonyhax>
- FreePSXBoot — <https://github.com/brad-lin/FreePSXBoot>

---

## Addendum — 2026-08-20 (later): the disc now boots to a prompt

Superseding §2 and §4 above:

| Image | Boots | Notes |
|---|---|---|
| `output/blackroo.bin` + `.cue` | `KERNEL.EXE` — the Linux kernel | **Burn this.** 742 KB kernel, stack `0x801fff00`, stops in BRMON on SIO1. Stock 2 MB console. |
| `output/blackroo-kloader.bin` + `.cue` | `BLACKROO.EXE` — kloader menu | Unchanged kloader binary; its "Boot from CD-ROM" entry is still the self-overwriting one described in §3. |

The 8 MB caveat in §2 is gone: the kernel no longer carries a 2 MB initrd, and
the RAM size is a build-time choice again (`CONFIG_PSX_2MB_RAM`, GR-004) rather
than hard-wired to 8 MB.

Test path is no longer emulator-first — with a modchip the burned CD-R boots
directly:

```bash
cdrecord -v -dao dev=/dev/sr0 cuefile=output/blackroo.cue
python3 tools/host/blackroo-serial.py /dev/ttyUSB0 console
```

Expect `PSX: 2048 KB RAM configured (reg=0x0888)` in the boot messages, then
the `blackroo>` prompt. See `docs/20-SERIAL-MONITOR.md`.


---

## Addendum 2 — 2026-08-20 (night): what it takes to boot on real hardware

kloader booted off a burned CD-R on the modchipped console. Four discs got us
there; three of them died on one thing.

### The licence area is not just "some bytes" — copy it from a disc that boots

A PlayStation rejects, or silently hangs on, a disc whose licence area is not
what it expects, **and a modchip does not help** — the chip answers the
wobble/SCEx check, the licence check is separate.

Three distinct failure modes seen in one evening:

| Licence area | Console behaviour |
|---|---|
| 28,032 zero bytes | "Please insert PlayStation CD-ROM" — clean rejection |
| Real `LICENSEE.DAT` via mkpsxiso **2.20** | SCE screen, disc spinning, PlayStation logo never drawn, hangs forever |
| First 16 sectors copied from a known-booting image | Boots |

The middle row is the trap. mkpsxiso 2.20 writes the licence file's Form-2 tail
(disc sectors 12-15) with **empty bodies**; the older mkpsxiso that built the
homebrew discs this console boots starts each of those bodies with
`00 00 08 00 00 00 08 00`. Everything else was identical — TOC control flags
(`control: 4`, data), `CD-XA001` at PVD offset 1024, all PVD identifiers, and
licence sectors 4-11 byte-for-byte. The BIOS draws the PlayStation logo from
that data; ours hung exactly there, before the logo appeared.

`iso/build-iso.sh` now transplants sectors 0-15 from `iso/license-area.bin`
after mkpsxiso runs. Sectors 0-15 are pure licence area and contain nothing of
ours, so this is a safe splice; our own filesystem starts at sector 16.

To produce that reference on another machine, take the first 37,632 bytes
(16 x 2352) of any disc image known to boot on your console:

```bash
head -c 37632 known-booting.bin > iso/license-area.bin
```

### Burning

`cdrdao` raw mode, which produced a byte-correct disc every time:

```bash
printf 'CD_ROM_XA\n\nTRACK MODE2_RAW\nDATAFILE "blackroo.bin"\n' > blackroo.toc
cdrdao write --device /dev/sr0 --driver generic-mmc-raw --speed 4 --eject -n blackroo.toc
```

`wodim`/`cdrecord` could not open the SCSI driver as a normal user on this host
("Cannot allocate memory" from the RLIMIT_MEMLOCK path), so cdrdao is the tool
here. Membership in the `cdrom` group is enough; no root needed.

### Verifying a burn

Use the kernel CD driver, **not** the drive's raw mode:

```bash
dd if=/dev/sr0 bs=2048 skip=16 count=1 | xxd   # PVD: CD001 / PLAYSTATION / <volume>
dd if=/dev/sr0 bs=2048 skip=23 count=1 | xxd   # SYSTEM.CNF
```

`cdrdao read-cd --read-raw` on this drive (TSSTcorp SN-208FB) returns 0.6%
non-zero bytes and looks like a destroyed burn. It isn't — the drive just will
not return raw Mode 2 data (GR-007).

### Before you burn anything, run the control test

**Boot a CD-R that has worked on that console before.** A retail disc proves
nothing — pressed discs need no modchip. Skipping this cost three discs and
several hours of modifying a kernel that was never at fault: kloader, proven on
this console in April, failed off CD in exactly the same way (GR-006).

### Reference disc

The disc this console boots is a third-party homebrew release, not ours:

```
volume id     MOVIE            publisher  OLD-HARD        created 2026-03-20
SYSTEM.CNF    BOOT=cdrom:\player.exe;1 / TCB=4 / EVENT=10 / STACK=801FFFF0
layout        SYSTEM.CNF LBA 23, PLAYER.EXE LBA 24, MOVIE.STR LBA 92
```
