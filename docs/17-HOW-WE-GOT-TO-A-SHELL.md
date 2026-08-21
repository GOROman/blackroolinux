# How We Got Blackroo Linux Booting to a Serial Shell — Down to the Letter

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: a BusyBox shell - the shell is brsh, written for this machine.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> **Audience:** PS1/Linux homebrew community members who want to reproduce, verify,
> or build on our work. This document explains **exactly** what was different
> between the old Runix-era tree (which would *not* boot to a shell) and the
> current Blackroo tree (which boots to an interactive `/bin/sh` over the
> PlayStation's serial port). It also includes a **beginner build guide** for
> the initrd, memory cards, and the upload/console flow.
>
> Nothing here is hand-wavy — every claim is backed by a file, a config flag,
> or a register value you can check yourself.

---

## TL;DR — The Three Things That Mattered

The old tree had **all the right source code already present** — the serial
driver, the ramdisk driver, the initrd loader. It just **never compiled any of
it in**, and the kernel command line told it to do the wrong thing. The fix was
**configuration, not new drivers**:

| # | What was wrong (old tree) | What we changed (current tree) |
|---|---------------------------|-------------------------------|
| 1 | `CONFIG_SERIAL_PSX` and `CONFIG_SERIAL_PSX_CONSOLE` **undefined** → the SIO1 serial console driver (`siocon.c`) was **never compiled**. No serial output, no serial shell — ever. | Both set to `=y`. The *identical* driver source now actually gets built and registered as `/dev/console`. |
| 2 | `CONFIG_BLK_DEV_INITRD` and `CONFIG_BLK_DEV_RAM` **undefined** → the entire "load a root filesystem into RAM and run init from it" code path was compiled out. The kernel could not see an embedded ramdisk even when one was present. | Both set to `=y`. Kernel now detects the embedded initrd, loads it into `/dev/ram0`, mounts it, and execs the init program. |
| 3 | Kernel command line was `root=/dev/bu0 console=ttyS0` — tried to root from a raw memory card, no baud rate, no `init=`. | `root=/dev/bul init=/bin/sh console=ttyS0,115200 console=tty0` — explicit shell, explicit baud, serial-first so panics are visible. |

> **The single most important insight:** the serial console driver `siocon.c` is
> **byte-for-byte identical** between the old and new trees. We did not write a
> new serial driver. We *enabled* one that was sitting dead in the tree since the
> Runix days. Same for the ramdisk and initrd loaders. **The project was never
> broken in C — it was broken in `.config`.**

---

## 0. The Research That Made This Possible

We did not guess our way to a shell. Each fix above came from a specific piece of
primary-source research. This is the trail other people can follow and verify —
the "how did you even know to do that" part.

### A. Knowing the SIO1 registers were *real* and how to drive them

The serial console is useless if the **hardware** won't transmit. The defining
research here was reconciling **emulator behaviour vs. real silicon** —
documented in full in **`docs/13-SIO1-HARDWARE-RESEARCH.md`**. The driver
compiled fine and worked in DuckStation, but a real PS1 stayed silent. The
research found three hardware truths the emulator hid:

1. **CTS/RTS handshake is mandatory on real SIO1.** The PS1's `SIO1_STAT` "TX
   ready" bit depends on CTS. If the FTDI doesn't assert it (and the PS1 doesn't
   assert RTS), the hardware **refuses to transmit** — silently. Emulators skip
   this check. Fix: `SIO1_CTRL = 0x27` (RTS asserted) on the PS1 side, and
   `rts=True, dtr=True` on the host.
   - Source: psx-spx Serial Interfaces — <https://psx-spx.consoledev.net/serialinterfacessio/>
   - Source: psx-spx Konami System 573 (CTS/RTS tie requirement) — <https://psx-spx.consoledev.net/konamisystem573/>
   - Source: NOTPSXSerial sets `DtrEnable`/`RtsEnable` — <https://github.com/JonathanDotCel/NOTPSXSerial>
2. **Baud multiplier must be MUL16.** `SIO1_MODE` was `0x004D` (MUL1) → baud 16×
   too fast → garbage on real hardware, fine in emulator. Fix: `0x004E` (MUL16).
   - Source: psx-spx Serial Interfaces (MODE register bits 0–1)
   - Source: PSn00bSDK `psxsio.h` (`SIO_MODE_BAUD_MUL16 = 0x0002`) — <https://github.com/Lameguy64/PSn00bSDK/blob/master/libpsn00b/include/psxsio.h>
   - Cross-check: PSn00bSDK `sio.c` — <https://github.com/Lameguy64/PSn00bSDK/blob/master/libpsn00b/psxsio/sio.c>
3. **Host must assert RTS/DTR after opening the port** (pyserial defaults leave
   them low). Source: pyserial docs — <https://pyserial.readthedocs.io/>

> This is why "it works in DuckStation but not on hardware" was such a long
> chase, and why the write-up exists: the **kernel serial driver** (`siocon.c`)
> and the **bootloader serial code** (`serial.c`) needed the *same* register
> truths. The kernel-console path benefits directly from this hardware research.

### B. Knowing the cable and how to get code onto the console at all

- **Serial cable pinout (3-wire SIO1):** UniROM serial cable guide —
  <https://unirom.github.io/serial_psx_cable/>; pin reference cross-checked
  against psx-spx Pinouts — <https://psx-spx.consoledev.net/pinouts/>.
- **Getting our PS-EXE onto the machine without a modchip:** UniROM + NOTPSXSerial
  (the SEXE upload protocol we reimplemented as `sendexe`), and FreePSXBoot for
  the no-hardware-mod memory-card exploit path. Documented in
  **`docs/09-SERIAL-UPLOAD.md`** and **`docs/12-SERIAL-SHELL-GUIDE.md`**.
  - UniROM — <https://github.com/JonathanDotCel/unirom8_bootdisc_and_firmware_for_ps1>
  - NOTPSXSerial (`nops`) — <https://github.com/JonathanDotCel/NOTPSXSerial>
  - FreePSXBoot — <https://github.com/brad-lin/FreePSXBoot>

### C. Knowing the initrd/ramdisk path and how to feed it a root fs

- **RAM-disk + initrd sizing and the BusyBox-must-be-built-for-this-target
  constraint:** **`docs/07-RAMDISK-ROOTFS.md`**. BusyBox source reference used
  for the userland — <https://busybox.net/downloads/busybox-1.1.1.tar.bz2>.
- **The Linux 2.4 initrd boot contract itself** (magic-after-`_end` → `/dev/ram0`
  → mount ext2 → exec init) came from reading this kernel's own
  `init/main.c` / `drivers/block/rd.c` / `fs/super.c` once `CONFIG_BLK_DEV_INITRD`
  was on — see commit `cbfc5e9`, which lists every symbol it re-enabled.

### D. Knowing the RAM register values

- **`RAM_SIZE` register (`0x1F801060`) bit layout and the 2/4/8 MB values**
  (`0x0888` / `0x0988` / `0x0B88`): psx-spx Memory Control —
  <https://psx-spx.consoledev.net/memorycontrol/>. Captured in
  **`docs/02-MEMORY-SUBSYSTEM.md`** and applied in `prom/memory.c`.

### Primary research index

| Source | What it gave us |
|--------|-----------------|
| **psx-spx** (consoledev.net) — Serial Interfaces, Memory Control, Pinouts, System 573 | The authoritative SIO1/RAM register semantics; the CTS requirement |
| **PSn00bSDK** `psxsio.h` / `sio.c` (Lameguy64) | Correct MODE/CTRL bit values to cross-check our driver |
| **NOTPSXSerial** (JonathanDotCel) | The serial upload protocol + RTS/DTR handling we mirrored |
| **UniROM** (JonathanDotCel) | The on-console bootstrap that receives our PS-EXE |
| **FreePSXBoot** (brad-lin) | No-modchip boot path via the memory-card exploit |
| **pyserial** docs | Correct host-side line-control (RTS/DTR) |
| **BusyBox** | The userland binary that becomes our shell |
| **This kernel's own `init/`, `drivers/block/`, `fs/`** | The initrd boot contract, once compiled back in |

Our own research write-ups (`docs/09`, `docs/12`, `docs/13`, plus the
`CHANGELOG.md` entries dated 2026-04-02 and 2026-04-13/14) document each of these
in depth with the exact register decodes and before/after values.

---

## 1. Proof: The Serial Driver Was Always There, Just Never Built

### The Makefile gate (identical in both trees)

`blackroo/drivers/char/Makefile`, lines 112–113, in **both** the old and current
tree:

```make
obj-$(CONFIG_SERIAL_PSX)         += serial_psx.o
obj-$(CONFIG_SERIAL_PSX_CONSOLE) += ../../arch/mipsnommu/ps/siocon.o
```

In a Linux 2.4 `Makefile`, `obj-$(CONFIG_FOO)` expands to `obj-y` (built) when
`CONFIG_FOO=y`, or `obj-` (discarded) when the symbol is unset. So:

- **Old tree:** `CONFIG_SERIAL_PSX_CONSOLE` is undefined → line expands to
  `obj- += siocon.o` → **siocon.o is never linked into the kernel.** There is no
  serial console. Whatever you type, nothing comes out the COM port.
- **Current tree:** `CONFIG_SERIAL_PSX_CONSOLE=y` → `obj-y += siocon.o` →
  the console driver is linked, registers itself, and `console=ttyS0` finally
  has something to attach to.

### Verify it yourself

```bash
# In the OLD tree — these come back undefined:
grep -E 'SERIAL_PSX' "blackroolinux-main (2)/blackroo/include/linux/autoconf.h"
#   #undef  CONFIG_SERIAL_PSX

# In the CURRENT tree — these are enabled:
grep -E 'SERIAL_PSX' blackroolinux-main/blackroo/.config
#   CONFIG_SERIAL_PSX=y
#   CONFIG_SERIAL_PSX_CONSOLE=y

# The driver source is identical — diff produces NO output:
diff "blackroolinux-main (2)/blackroo/arch/mipsnommu/ps/siocon.c" \
     "blackroolinux-main/blackroo/arch/mipsnommu/ps/siocon.c"
```

---

## 2. Proof: The initrd Path Was Compiled Out (The Real Boot Killer)

This is the root cause that kept the project dead since the Runix era. From our
commit `cbfc5e9` ("Enable initrd boot path — fix root cause of boot failure"):

> **ROOT CAUSE FOUND:** `CONFIG_BLK_DEV_INITRD` was never enabled. The entire
> initrd detection, mounting, and init execution code was compiled out. The
> kernel literally could not see the embedded ramdisk, even when one was present.

Enabling it compiles back in, across the kernel:

- `initrd_start`/`initrd_end` detection in `arch/.../setup.c`
- `initrd_load()` in `drivers/block/rd.c`
- the `do_linuxrc()` / init-exec path in `init/main.c`
- the `mount_initrd` logic in `fs/super.c`

### The config delta (old → current)

```text
# OLD (autoconf.h):              # CURRENT (.config):
#undef  CONFIG_BLK_DEV_INITRD     CONFIG_BLK_DEV_INITRD=y
#undef  CONFIG_BLK_DEV_RAM        CONFIG_BLK_DEV_RAM=y
(no RAM_SIZE)                     CONFIG_BLK_DEV_RAM_SIZE=4096
#undef  CONFIG_PSX_LARGE_CARD     CONFIG_PSX_LARGE_CARD=y   (memory-card RAID join)
(none)                           CONFIG_PSX_RAM_AUTO=y      (RAM auto-detect symbol)
```

### Where this gets applied automatically

You do **not** have to edit `.config` by hand. `build.sh` does it in
`setup_config()` (and patches `autoconf.h` directly, because the 2.4 config
system won't regenerate it reliably for this tree):

```bash
# build.sh, setup_config()
sed -i 's/# CONFIG_PSX_LARGE_CARD is not set/CONFIG_PSX_LARGE_CARD=y/' .config
sed -i 's/# CONFIG_BLK_DEV_RAM is not set/CONFIG_BLK_DEV_RAM=y/'       .config
echo "CONFIG_BLK_DEV_INITRD=y"   >> .config
echo "CONFIG_BLK_DEV_RAM_SIZE=4096" >> .config

# and directly into include/linux/autoconf.h (the file the compiler actually reads):
sed -i 's/#undef  CONFIG_BLK_DEV_INITRD/#define CONFIG_BLK_DEV_INITRD 1/' include/linux/autoconf.h
sed -i 's/#undef  CONFIG_BLK_DEV_RAM$/#define CONFIG_BLK_DEV_RAM 1/'      include/linux/autoconf.h
```

> **Gotcha for anyone hacking this tree:** this kernel's `.config` and
> `include/linux/autoconf.h` can drift out of sync. `autoconf.h` is what the C
> compiler actually sees. If you change `.config` and the behavior doesn't
> change, check `autoconf.h` — that was the cause of several "I enabled it but
> nothing happened" dead ends.

---

## 3. The Kernel Command Line (`cmdline.c`)

The boot string is hard-coded in
`blackroo/arch/mipsnommu/ps/prom/cmdline.c` (this tree has no bootloader-passed
cmdline, so it's compiled in).

**Old tree (one line, broken for a shell):**
```c
strcpy (arcs_cmdline, "root=/dev/bu0 console=ttyS0");
```
Problems: roots from a raw memory card (`/dev/bu0`) that has no filesystem; no
`init=`, so it falls back to defaults that don't exist; no baud rate on the
console; serial driver wasn't even compiled (see §1), so `console=ttyS0` was a
no-op.

**Current tree:**
```c
strcpy (arcs_cmdline,
        "root=/dev/bul init=/bin/sh console=ttyS0,115200 console=tty0");
```

Token by token:

| Token | Meaning |
|-------|---------|
| `root=/dev/bul` | Root filesystem = the joined ("large"/RAID) memory-card device created by `CONFIG_PSX_LARGE_CARD`. For pure-RAM boot you can instead use `root=/dev/ram0`. |
| `init=/bin/sh` | Run the BusyBox shell directly as PID 1. (Earlier we used `init=/linuxrc`; both work — `/linuxrc` is the script `make_initrd.sh` drops in.) |
| `console=ttyS0,115200` | **Serial console over SIO1 at 115200 8N1.** Listed *first* so kernel panics print over serial before any GPU init. This is the line that puts a shell on your COM port. |
| `console=tty0` | GPU/TV console as a secondary output. Both consoles receive kernel messages; the last one listed owns `/dev/console`. |

> **Why serial first?** If the GPU console is listed last it owns
> `/dev/console`, but listing serial first guarantees early boot and panic
> output still reaches the COM port. For a headless serial-only bring-up this is
> exactly what you want.

---

## 4. RAM Configuration (`prom/memory.c`)

Not strictly required for "a shell," but it's part of "the way we configured it
all," and the old code was a stub.

**Old tree:** hard-coded 2 MB and an unfinished probe:
```c
*mem_size_reg = 0x888;   // !!! we have 2 Mb memory
// ...
// !!! this is stub now - fix me !!!
return 0;
```

**Current tree:** sets the `RAM_SIZE` register (`0x1F801060`) for 8 MB and adds a
real power-of-two mirror-probe (`psx_probe_ram()`) plus a boot `printk`. The
register values used (from psx-spx Memory Control):

| RAM | `RAM_SIZE` value |
|-----|------------------|
| 2 MB (stock) | `0x0888` |
| 4 MB | `0x0988` |
| 8 MB (mod / DuckStation 8MB setting) | `0x0B88` |

The auto-detect path is present but currently `#if 0`'d, because in DuckStation
the probe mis-read as 1 MB; we hard-set `0xB88` (8 MB) for now. The probe writes
unique patterns at the 0 / 2 MB / 4 MB boundaries via **uncached KSEG1**
addresses (`0xA0000000+`) and checks for mirroring. See the comments in the file
for the full decision tree.

---

## 5. What the Kernel Actually Does at Boot (current tree)

From commit `cbfc5e9`, the sequence that now works end to end:

1. Search for the initrd magic `INRD` (`0x494E5244`) appended after the kernel's
   `_end`.
2. If found, set `initrd_start` / `initrd_end`.
3. Load the initrd image into `/dev/ram0`.
4. Mount `/dev/ram0` as an **ext2** root filesystem.
5. Exec the init program (`/bin/sh` or `/linuxrc`).
6. BusyBox `ash` shell appears — **over `console=ttyS0`, i.e. your COM port.**

---

# Beginner Build Guide

This section assumes you have just cloned the current tree and want to go from
zero to "a shell on my COM port." Commands are run from the repo root
(`blackroolinux-main/`).

## 0. Prerequisites (Ubuntu 24.04)

```bash
# Cross toolchain is bundled (EGCS 2.91.66 in Archive/) and is 32-bit,
# so you need 32-bit loader support:
sudo apt install libc6:i386

# Tools used by the initrd/memory-card steps:
sudo apt install genext2fs e2fsprogs build-essential

# For talking to the PS1 over serial:
pip install pyserial
```

> The build uses the **original EGCS 2.91.66** MIPS toolchain shipped in
> `Archive/toolchain/`. `build.sh` sets `GCC_EXEC_PREFIX` and fixes the Makefile
> include paths for you. You do **not** need a modern `gcc-mips-linux-gnu` for
> the kernel itself.

## 1. Build the Kernel

```bash
./build.sh            # full build: configures, fixes paths, builds host tools + kernel
# Output: the kernel ELF, plus host tools in tools/
```

What `build.sh` does for you (the important parts, from §2 above):
- copies `Config` → `.config` if missing,
- forces on `CONFIG_BLK_DEV_RAM`, `CONFIG_BLK_DEV_INITRD`, `CONFIG_PSX_LARGE_CARD`,
- patches `include/linux/autoconf.h` to match,
- builds the host-side converters (`elf2psexe`, `addpsexe_initrd`,
  `elf2ecoff`, `addinitrd`, `conmakehash`).

## 2. Build the initrd (root filesystem in RAM)

The initrd is a tiny **ext2** image holding **BusyBox** (which must be a MIPS
binary — for uClinux/no-MMU it should ultimately be **FLAT** format, not ELF)
plus an init script. The staging directory lives at `build/initrd_root/`.

What goes inside (already staged in this tree):

```
build/initrd_root/
├── init                 # #!/bin/sh ; mount /proc ; exec /bin/sh
├── bin/  sbin/          # busybox + symlinks (sh, ls, cat, mount, ...)
├── etc/inittab          # ::sysinit:/etc/init.d/rcS ; ::respawn:/bin/sh
├── etc/init.d/rcS       # mounts /proc + /sys, prints the Blackroo banner
└── dev/ proc/ sys/ ...  # mount points
```

`etc/inittab` is what re-spawns your shell:
```text
::sysinit:/etc/init.d/rcS
::respawn:/bin/sh
::ctrlaltdel:/sbin/reboot
```

### Create the image — pick ONE:

**No root required (recommended)** — uses `genext2fs` + a device table so the
`/dev` nodes are created without `mknod`/loop-mount:

```bash
./scripts/make_initrd_noroot.sh
# -> output/initrd.img  (2048 KB ext2)
```

The device nodes come from `scripts/device_table.txt` (console 5:1, ram0 1:0,
the PS1 memory cards `bu0..bu7` at major 240, etc.).

**Root version (loop mount)** — same result, uses `sudo mount -o loop` and
`mknod`:

```bash
sudo ./scripts/make_initrd.sh
# -> output/initrd.img
```

### Embed the initrd into the kernel PS-EXE

```bash
./build.sh convert
# elf2psexe : kernel ELF  -> PS-EXE
# addpsexe_initrd : appends output/initrd.img after _end with the INRD magic
# -> output/kernel+initrd.exe   (this is what go.sh boots as option 3)
```

## 3. Create Memory Card Images (`.mcd`)

For boot you can run entirely from the initrd in RAM, but to use the memory-card
storage / `root=/dev/bul` path you create Blackroo-formatted cards with
`mkmemcard`:

```bash
# Build the tool (one time):
gcc -Wall -o tools/host/mkmemcard tools/host/mkmemcard.c

# A single empty card (raw 128 KB / 131072 bytes, Blackroo header in block 0):
tools/host/mkmemcard output/blackroo_card0.mcd

# A card that contains a filesystem image (copied in starting at block 8):
tools/host/mkmemcard output/blackroo_card0.mcd 0 output/initrd.img

# A multi-card RAID set (1–8 cards) for CONFIG_PSX_LARGE_CARD:
tools/host/mkmemcard --raid 4 output/blackroo
#  -> blackroo_0.mcd .. blackroo_3.mcd
```

Card layout produced (matches `bu.h` in the kernel driver):
- **Block 0:** Blackroo header — `id=0x1234`, `size=1024` blocks, random serial,
  card number (its index in the RAID set).
- **Blocks 1–7:** reserved (`BU_FIRST_BLOCKS = 8`).
- **Blocks 8–1023:** filesystem data (≈127 KB usable per card).

In DuckStation: **Settings → Memory Cards → Card 1 → Browse →** select
`blackroo_card0.mcd` (and Card 2 for the second).

## 4. Run It

### In the emulator
```bash
./go.sh nommu          # boots output/kernel+initrd.exe in DuckStation
# (./go.sh with no args gives an interactive menu: bootloader / mmu / nommu)
```

### On real hardware over serial (the COM-port shell)

Wiring (3 wires, SIO1) — see `docs/13-SIO1-HARDWARE-RESEARCH.md` for the
hardware gotchas (you **must** assert RTS/DTR or the PS1 SIO1 blocks all TX):

| PSX pin | Signal | FTDI |
|--------:|--------|------|
| 2 | GND | GND |
| 5 | RX | TX |
| 8 | TX | RX |

Upload + drop into the shell with the host tool:

```bash
# 1. From a UniROM / FreePSXBoot bootstrap, push the kernel PS-EXE:
tools/host/blackroo-serial.py /dev/ttyUSB0 sendexe output/kernel+initrd.exe

#    …or via our kloader's shell protocol (auto-boots, then opens console):
tools/host/blackroo-serial.py /dev/ttyUSB0 upload output/kernel+initrd.exe

# 2. Just open the serial console (115200 8N1; Ctrl+] to exit):
tools/host/blackroo-serial.py /dev/ttyUSB0 console
```

When it works you'll see the kernel boot messages, the Blackroo banner from
`rcS`, and a `#` BusyBox prompt — **served over `console=ttyS0,115200`, i.e.
your COM port.** That's the milestone.

> Baud: standard is **115200**; the kloader/host tool also support a **518400**
> fast mode (`--fast`). The memory-card SIO0 bus is a *separate* 250 kHz serial
> link — don't confuse it with the SIO1 console.

---

## Appendix: One-Page Diff Summary (old tree → current tree)

| File / setting | Old (no shell) | Current (serial shell) |
|----------------|----------------|------------------------|
| `CONFIG_SERIAL_PSX` | `#undef` | `=y` |
| `CONFIG_SERIAL_PSX_CONSOLE` | `#undef` | `=y` |
| `CONFIG_BLK_DEV_INITRD` | `#undef` | `=y` |
| `CONFIG_BLK_DEV_RAM` | `#undef` | `=y` |
| `CONFIG_BLK_DEV_RAM_SIZE` | (unset) | `4096` |
| `CONFIG_PSX_LARGE_CARD` | (unset) | `=y` |
| `CONFIG_PSX_RAM_AUTO` | (none) | `=y` |
| `cmdline.c` boot string | `root=/dev/bu0 console=ttyS0` | `root=/dev/bul init=/bin/sh console=ttyS0,115200 console=tty0` |
| `prom/memory.c` | hard `0x888` (2 MB) + `// fix me` stub | `0xB88` (8 MB) + `psx_probe_ram()` + `printk` |
| `siocon.c` (driver) | present but **never compiled** | **identical source, now compiled** |
| initrd image | none / not loadable | `output/initrd.img` (ext2, BusyBox + init), embedded after `_end` with `INRD` magic |

**Bottom line for the community:** if you have an old Runix/PSX-Linux tree that
"compiles but does nothing on serial," check these config symbols *first*. The
drivers are almost certainly already in the tree — they were just never turned
on, and the command line was pointing at the wrong root with no `init=`.

---

*Reference docs: `09-SERIAL-UPLOAD.md`, `12-SERIAL-SHELL-GUIDE.md`,
`13-SIO1-HARDWARE-RESEARCH.md`, `07-RAMDISK-ROOTFS.md`, `02-MEMORY-SUBSYSTEM.md`.*
*Key commit: `cbfc5e9` "Enable initrd boot path — fix root cause of boot failure".*
