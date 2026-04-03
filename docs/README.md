# Blackroo Linux — documentation

> Linux 2.4 (uClinux, no-MMU) on a stock Sony PlayStation. MIPS R3000A at
> 33 MHz, 2 MB of RAM.

**Start here:**

| | |
|---|---|
| [**00 — Project status**](00-PROJECT-STATUS.md) | What works today, what is unproven, what does not exist. Kept current. |
| [**29 — Lineage and roadmap**](29-LINEAGE-AND-ROADMAP.md) | What came from Runix, what was written here, what is next. |
| [**30 — Blockers and hardware notes**](30-BLOCKERS-AND-HARDWARE-NOTES.md) | What is in the way right now, and why 16 MB is not a release. |
| [../README.md](../README.md) | Build it, burn it, talk to it over serial. |

---

## A note on the rest

**Documents 01–28 are research and history.** They record what was known when
they were written, including approaches that were later abandoned, and they are
deliberately **not** rewritten — the record of what was tried and why it failed
is most of this project's value.

Where one of them disagrees with document 00, **document 00 is right**. The
known corrections are listed at the bottom of it: UniROM as the upload path,
`CONFIG_BINFMT_FLAT` as the userspace plan, "BusyBox shell", the 8 MB mod, and
the FPU emulator.

---

## Hardware and architecture

| # | | |
|---|---|---|
| 01 | [Hardware architecture](01-ARCHITECTURE.md) | R3000A, memory map, interrupts, I/O registers, boot sequence |
| 02 | [Memory subsystem](02-MEMORY-SUBSYSTEM.md) | RAM configurations, register values, budgets |
| 21 | [Target console](21-TARGET-CONSOLE.md) | The two PAL SCPH-750x this is developed against |
| 21 | [PIO port reference](21-PIO-PORT-REFERENCE.md) | The expansion port |

## Storage

| # | | |
|---|---|---|
| 03 | [Memory card storage](03-MEMORY-CARD-STORAGE.md) | SIO0 protocol, `bu.c`, joining cards |
| 04 | [Multitap whitepaper](04-MULTITAP-WHITEPAPER.md) | Eight cards through one port |
| 05 | [PS2 memory card analysis](05-PS2-MEMCARD-ANALYSIS.md) | Cross-generation compatibility |
| 11 | [PicoMemcard dual-mode](11-PICOMEMCARD-DUAL-MODE.md) | RP2040 as storage and console at once |
| 15 | [PicoMemcard / SuperCard](15-PICOMEMCARD-SUPERCARD-RESEARCH.md) | Bulk storage research |
| 23 | [Root filesystem plan](23-ROOT-FILESYSTEM-PLAN.md) | Where a root filesystem could live |
| 24 | [**CD-ROM driver research**](24-CDROM-DRIVER-RESEARCH.md) | The reference for `psxcd.c`. Hardware-confirmed sections marked as such. |
| 25 | [Root mount journey](25-ROOT-MOUNT-JOURNEY.md) | Getting a filesystem mounted |

## Booting

| # | | |
|---|---|---|
| 06 | [Bootloader design](06-BOOTLOADER-DESIGN.md) | kloader's design |
| 09 | [Serial upload](09-SERIAL-UPLOAD.md) | **Wiring and pinout** — still current for the cable |
| 12 | [Serial shell guide](12-SERIAL-SHELL-GUIDE.md) | Driving the machine over SIO1 |
| 13 | [SIO1 hardware research](13-SIO1-HARDWARE-RESEARCH.md) | The serial port itself |
| 18 | [Cart-resident boot](18-CART-RESIDENT-BOOT-RESEARCH.md) | Booting from the expansion port |
| 19 | [Bootable CD](19-BOOTABLE-CD.md) | **The licence area** — why a burned disc hangs at the SCE screen |
| 20 | [Serial monitor](20-SERIAL-MONITOR.md) | BRMON |

## Input and output

| # | | |
|---|---|---|
| 14 | [PS1 keyboard research](14-PS1-KEYBOARD-RESEARCH.md) | Protocols, and why the Lightspan one won |
| 27 | [Keyboard bring-up](27-KEYBOARD-BRINGUP.md) | Getting it working |

## Userspace

| # | | |
|---|---|---|
| 07 | [Ramdisk and rootfs](07-RAMDISK-ROOTFS.md) | initrd layouts |
| 17 | [How we got to a shell](17-HOW-WE-GOT-TO-A-SHELL.md) | The road to pid 1 |
| 28 | [**Userspace and desktop**](28-USERSPACE-AND-DESKTOP.md) | The 2 MB budget, and why the desktop is shaped like Mac System 1 |

## Build, and the rest

| # | | |
|---|---|---|
| 08 | [Build system](08-BUILD-SYSTEM.md) | The pipeline |
| 10 | [PS2 compatibility](10-PS2-COMPATIBILITY.md) | Why the PS2 IOP is not a target |
| 16 | [PS2 microkernel research](16-PS2-MICROKERNEL-RESEARCH.md) | A road not taken |
| 22 | [What went wrong](22-WHAT-WENT-WRONG.md) | Failures worth keeping |
| 22 | [Wireless link and clustering](22-WIRELESS-LINK-AND-CLUSTERING.md) | Two consoles, linked |
| 26 | [Next stage](26-NEXT-STAGE.md) | Superseded by document 29 |

---

| | |
|---|---|
| [CHANGELOG.md](../CHANGELOG.md) | Every change, dated, with what it cost to find |
| [SOURCE-ATTRIBUTION.md](../SOURCE-ATTRIBUTION.md) | Origin and licence of every component |
| [notes/HARDWARE-TRAPS.md](notes/HARDWARE-TRAPS.md) | Things that went wrong once and must not again |
