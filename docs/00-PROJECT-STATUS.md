# Project status

> **Release 0.5.0 "Rootstock26" — 2026-08-26.**
> Linux 2.4 (uClinux, no-MMU) on a stock Sony PlayStation. MIPS R3000A at
> 33 MHz, 2 MB of RAM, booting from a CD-R to a shell on the television.

This file is kept current. Documents 01–28 are **research and history** — they
record what was known at the time they were written and are deliberately not
rewritten. `29-LINEAGE-AND-ROADMAP.md` is where the project came from and where
it is going.

---

## What works, on real hardware

| | Status | Evidence |
|---|---|---|
| **Boot from CD-R, no host PC** | Working | BIOS → kloader → `LINUX.EXE` → shell on the TV |
| **Root filesystem on the disc** | Working | `psxcd: found ROOT.IMG at LBA 827, 4096 KB`, ext2 mounted |
| **ISO9660 lookup** | Working | Volume descriptor + root directory walk; **no LBA compiled in** |
| **CD-ROM block device** | Working | major 209, 2048-byte sectors, DMA verified byte-identical to PIO |
| **Memory cards (read)** | Working | 8 cards through a multitap joined as `/dev/bul`, 508 KB |
| **Keyboard** | Working | Lightspan protocol on the controller bus (`psxkbd.c`) |
| **Console on the television** | Working | GPU text console, mirrored to SIO1 |
| **Userspace** | Working | `brsh` as pid 1 in a 192 KB fixed window (`binfmt_fixed`) |
| **kloader** | Working | Menu, card manager, 8 video modes, serial upload at 10.4 KB/s |
| **kloader settings persist** | Working | Card 0 block 1, magic `"BLKS"`, version-checked |
| **BRMON** | Working | In-kernel monitor on SIO1; auto-entered on panic |

## What is built but unproven on hardware

| | Note |
|---|---|
| **Memory card writes** | Only reads have run through `bu.c`. kloader writes cards, so the protocol is proven — the block driver's write path is not. |
| **`ps` / `top` / `df` / `edit` / `mount`** | Added 2026-08-26, compile clean, not yet run on the console. |
| **Self-contained CD-root boot** | CD-root was proven with a serial-uploaded kernel and the disc supplying `ROOT.IMG`. The full BIOS → kloader → `LINUX.EXE` path needs a reburn. |

## What does not exist

- **Networking.** `CONFIG_NET` is off and `net/` is not linked — 30 KB on a 2 MB
  machine for an API no hardware here can reach. See the roadmap for the ESP32
  serial bridge, which is the cheap version of "telnet".
- **`/proc`.** Removed: 34 KB. `sys_blackroo_tasks(217)` replaces the one use
  that mattered, for 484 bytes.
- **`fork()`.** No MMU. `vfork()` semantics only, one program image resident at
  a time.
- **A full-screen editor or `htop`.** The process data exists; `brsh` has no
  termios, so raw mode and cursor addressing do not.
- **BusyBox.** Blocked on a MIPS I toolchain, not on the kernel.

---

## The numbers

```
kernel text     648,888        (742,432 before the size pass)
kernel data      39,016
kernel bss      115,648
userspace window 192 KB at 0x001d0000   (was 64 KB)
brsh             28,880 bytes
RAM              2,048 KB total
serial upload    10.4 KB/s
```

## Configuration that matters

```
CONFIG_PSX_2MB_RAM=y          the target is a stock console
CONFIG_BINFMT_FIXED=y         fixed-address ELF loader
CONFIG_EXT2_FS=y              on the disc and on cards
CONFIG_PSX_CDROM=y            major 209
CONFIG_PSX_KEYB=y             replaces pc_keyb entirely - there is no 8042
CONFIG_BLACKROO_MONITOR=y     BRMON. 27 KB, and worth it
# CONFIG_PROC_FS is not set
# CONFIG_NET is not set
# CONFIG_BINFMT_FLAT is not set
# CONFIG_MODULES is not set
```

The FPU emulator is **not** built (`NO_FPU`) — it was 47 KB for an instruction
this CPU never issues.

---

## Corrections to older documents

Documents 01–28 are point-in-time. Where they disagree with this file, this file
is right:

- **UniROM and `nops`** (docs 06, 09, 13, 19) were the upload path before
  kloader existed. kloader replaced them; `tools/host/blackroo-serial.py` is
  the host side.
- **`CONFIG_BINFMT_FLAT`** (docs 01, 02, 07, 20, 22, 23, 25) was the plan for
  userspace. It never worked here — EGCS 2.91 emits absolute `R_MIPS_26` calls
  that bFLT cannot patch. `binfmt_fixed` replaced it.
- **"BusyBox shell"** (doc 17) describes the goal at the time. The shell is
  `brsh`, written for this machine; BusyBox is still blocked on a toolchain.
- **The 8 MB mod** (docs 01, 02, 06) is not required and not fitted. Everything
  targets a stock 2 MB console.
- **"about 1 MB free"** (older notes) was wrong. Measured: 436 KB at boot,
  236 KB after the ramdisk — before the size pass returned 96 KB.
- **The FPU emulator** is described as working in docs 00–02. It is not built.
