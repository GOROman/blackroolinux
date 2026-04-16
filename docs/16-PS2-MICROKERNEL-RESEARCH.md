# PS2 Microkernel OS — Research Investigation

> Full technical investigation into building a microkernel operating system
> that runs natively on the PlayStation 2, with networking, DOS compatibility
> layer, and game support. Codename: **BlackrooOS**.

**Date:** 2026-04-16
**Researcher:** Chelson Aitcheson
**Project:** Blackroo Linux / BlackrooOS
**Status:** Research complete — ready for prototyping

All claims in this document are sourced. See `refs/00-SOURCE-INDEX.md` for
the complete source list with URLs.

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [PS2 Hardware Architecture](#2-ps2-hardware-architecture)
3. [Development Toolchain](#3-development-toolchain)
4. [PS2 Boot Process](#4-ps2-boot-process)
5. [Existing PS2 OS Projects](#5-existing-ps2-os-projects)
6. [Microkernel Design](#6-microkernel-design)
7. [Networking](#7-networking)
8. [DOS Compatibility Layer](#8-dos-compatibility-layer)
9. [Legal and Licensing](#9-legal-and-licensing)
10. [Implementation Roadmap](#10-implementation-roadmap)
11. [Open Questions](#11-open-questions)

---

## 1. Executive Summary

### The Idea

Build a microkernel OS ("BlackrooOS") that runs natively on PlayStation 2
hardware. The OS would:

- Boot on unmodified PS2 hardware via FreeMCBoot
- Run Dead Seas as a native process
- Provide TCP/IP networking over ethernet
- Include a DOS compatibility layer (8086 emulator + FreeDOS)
- Be distributed as a "MIPS R5900" application, avoiding Sony trademarks

### Key Findings

**This is feasible.** Every component has precedent:

| Component | Precedent | Status |
|-----------|-----------|--------|
| Native PS2 networking | OPL loads games over SMB daily | Production-quality |
| Microkernel on MIPS | L4/MIPS achieved 86-cycle IPC (1997) | Proven |
| TLB-based memory protection | R5900 has 48-entry TLB (currently wasted) | Hardware exists |
| DOS on PS2 | DOSBox already runs on PS2 (belek666) | Proven |
| ps2sdk for development | GCC 15.2.0, active community | Production-quality |
| Legal distribution | AFL 2.0 license, no enforcement precedent | Low risk |

**Nobody has built a microkernel for any game console.** This would be a first.

### Why the PS2 Is Suited for This

The PS2's split EE/IOP architecture is **already a microkernel-style design**
in hardware. The IOP runs driver modules (IRX) that the EE communicates with
via message passing (SIF RPC). We're not fighting the hardware — we're
formalizing what the hardware already does.

---

## 2. PS2 Hardware Architecture

### System Overview

| Component | Specification |
|-----------|--------------|
| Main CPU (EE) | MIPS R5900, 294.912 MHz, 2-issue superscalar |
| I/O Processor (IOP) | MIPS R3000A, 36.864 MHz (same as PS1!) |
| Graphics (GS) | 147.456 MHz, 16 pixel pipelines, 4MB VRAM |
| Vector Unit 0 | 4KB micro + 4KB data, macro mode (COP2) |
| Vector Unit 1 | 16KB micro + 16KB data, micro mode only |
| Sound (SPU2) | 48 ADPCM voices, 2MB RAM |
| Main RAM | 32MB RDRAM, 3.2 GB/s bandwidth |
| IOP RAM | 2MB |
| Scratchpad | 16KB at 0x70000000 |

Source: Emotion Engine Wikipedia, PS2 Technical Specifications Wikipedia,
Copetti Architecture Analysis

### Emotion Engine (R5900) — Key Details for OS Development

**ISA:** MIPS-III with subset of MIPS-IV, plus 107 proprietary MMI (Multimedia
Instructions) for 128-bit SIMD. Registers are 128-bit wide but used as SIMD
lanes, not 128-bit integers.

**Critical missing instructions:** LL (Load Linked) and SC (Store Conditional)
are **not implemented**. These are the standard MIPS atomic operations — they
must be emulated or worked around for an OS kernel. Fredrik Noring's Linux
patches document this.

Source: Linux for PS2 patch series (lore.kernel.org), TX79 Architecture Manual

**FPU:** 32-bit single-precision only. **NOT IEEE 754 compliant** — only
rounding-to-zero, no NaN or infinity support. This matters for porting
existing code.

Source: PCSX2 FPU Analysis

**Cache:** 16KB I-cache (2-way), 8KB D-cache (2-way), 16KB scratchpad.

**TLB:** 48 entries, joint I/D, supporting page sizes 4KB to 16MB.
The stock PS2 BIOS allocates 39 entries for OS use (cache control), leaving
only 9 for applications. A custom OS reclaims all 48. The `SYNC.P` barrier
is required after COP0 writes due to R5900 pipeline hazards.

Source: PCSX2 MMU Mini-Series, PS2Tek COP0 Memory Management

### EE Memory Map

```
Physical Address Map:
00000000 - 01FFFFFF   32 MB  Main RDRAM
10000000 - 10001FFF          EE Timer registers (T0-T3)
10002000 - 10002FFF          IPU registers
10003000 - 100030FF          GIF registers
10003800                     VIF0 registers
10003C00                     VIF1 registers
10008000 - 1000DFFF          DMAC channel registers (10 channels)
1000E000 - 1000EFFF          DMAC control (D_CTRL, D_STAT, D_PCR)
1000F000                     INTC_STAT
1000F010                     INTC_MASK
1000F200                     SIF_MSCOM (EE-writable)
1000F210                     SIF_SMCOM (IOP-writable)
1000F220                     SIF_MSFLG
1000F230                     SIF_SMFLG
11000000                     VU0 Micro Memory (4KB)
11004000                     VU0 Data Memory (4KB)
11008000                     VU1 Micro Memory (16KB)
1100C000                     VU1 Data Memory (16KB)
12000000 - 12001FFF          GS Privileged Registers
1FC00000                     BIOS ROM (4MB)
70000000 - 70003FFF          Scratchpad RAM (16KB)

Virtual Address Segments (standard MIPS):
KUSEG  00000000-7FFFFFFF     TLB-mapped, user mode
KSEG0  80000000-9FFFFFFF     Cached, unmapped (phys = addr & 0x1FFFFFFF)
KSEG1  A0000000-BFFFFFFF     Uncached, unmapped
KSSEG  C0000000-DFFFFFFF     TLB-mapped, supervisor mode
KSEG3  E0000000-FFFFFFFF     TLB-mapped, kernel mode
```

Source: ps2tek, PS2 Developer Wiki Memory Map, PCSX2 Hw.h

### IOP — The Second Computer

The IOP is a full MIPS R3000A at 36.864 MHz with 2MB RAM. It is the **same
CPU as the PS1** — your PS1 bootloader experience transfers directly.

**IOP Memory Map:**
```
00000000   2MB   Main IOP RAM (first 64K reserved for BIOS)
1F000000   8MB   Expansion Region 1
1F800000   1KB   Scratchpad
1F801000   8KB   Hardware I/O registers
1FC00000  512KB  BIOS ROM
1D000000         SIF registers (IOP-side view)
```

**IRX Modules:** IOP drivers are IRX (IOP Relocatable eXecutable) files —
ELF-format binaries that run on the IOP. They are loaded dynamically and
communicate with the EE via SIF RPC. Each module exports functions and
imports from other modules.

**Deckard Warning:** SCPH-75000+ (late slim models) replaced the IOP with a
**PowerPC 405GP** running MIPS R3000A emulation. This introduces timing
differences. Any OS must handle both real IOP and Deckard.

Source: Copetti Architecture, PS2 Technical Specifications, IOP/Deckard wiki

### SIF — The Inter-Processor Bus

The SIF (Subsystem Interface) connects EE and IOP via DMA:

| Channel | Direction | Purpose |
|---------|-----------|---------|
| SIF0 | IOP → EE | Data from IOP |
| SIF1 | EE → IOP | Data to IOP |
| SIF2 | Bidirectional | PS1 compat / debug |

Three protocol layers:
1. **SIF DMA** — raw hardware register access
2. **SIF CMD** — command/interrupt protocol
3. **SIF RPC** — client/server model (EE calls IOP functions)

This is already a message-passing architecture — the microkernel's IPC maps
naturally onto SIF RPC.

Source: ps2tek, Linux PS2 SIF patches

### EE Interrupt System

**INTC Sources (15 sources):**

| Bit | Source | Description |
|-----|--------|-------------|
| 0 | GS | Graphics Synthesizer |
| 1 | SBUS | Sub-bus |
| 2 | VBLANK_S | VBLANK Start |
| 3 | VBLANK_E | VBLANK End |
| 4-5 | VIF0/VIF1 | Vector interfaces |
| 6-7 | VU0/VU1 | Vector units |
| 8 | IPU | Image processing |
| 9-12 | TIM0-TIM3 | Hardware timers |
| 13 | SFIFO | Transfer error |
| 14 | VU0WD | VU0 watchdog |

Registers: INTC_STAT at 0x1000F000, INTC_MASK at 0x1000F010.
Write-1-to-clear for STAT, write-1-to-toggle for MASK.

Source: ps2tek, PS2SDK kernel.h

### DMA System — 10 EE Channels

| Ch | Name | Direction | Target |
|----|------|-----------|--------|
| 0 | VIF0 | To | VU0 |
| 1 | VIF1 | To/From | VU1 |
| 2 | GIF | To | GS |
| 3-4 | IPU | From/To | Image processor |
| 5 | SIF0 | From | IOP → EE |
| 6 | SIF1 | To | EE → IOP |
| 7 | SIF2 | Bidir | Debug |
| 8-9 | SPR | From/To | Scratchpad |

All data must be 128-bit (quadword) aligned.

Source: PS2 DMAC Basics (Fobes), ps2tek

### Hardware Timers

Four 16-bit timers, all functionally equivalent:

| Timer | COUNT | MODE | COMP | HOLD |
|-------|-------|------|------|------|
| T0 | 0x10000000 | 0x10000010 | 0x10000020 | 0x10000030 |
| T1 | 0x10000800 | 0x10000810 | 0x10000820 | 0x10000830 |
| T2 | 0x10001000 | 0x10001010 | 0x10001020 | N/A |
| T3 | 0x10001800 | 0x10001810 | 0x10001820 | N/A |

T3 is reserved by BIOS for alarms — a custom OS reclaims it.
Clock sources: bus clock, bus/16, bus/256, HBLANK.

Source: PS2SDK timer.h, ps2tek

---

## 3. Development Toolchain

### ps2sdk — The Core SDK

- **Repository:** https://github.com/ps2dev/ps2sdk
- **License:** Academic Free License 2.0 (permissive, commercial use allowed)
- **Language:** 94.8% C, 3% Makefile, 2.1% Assembly
- **C Library:** newlib 4.5.0
- **Active:** Yes — commits through 2026, active issue tracker

**Provides:** EE/IOP headers, libraries, IRX modules, samples, tools (bin2c,
bin2s), CMake support.

Source: ps2sdk GitHub, ps2sdk README

### Cross-Compiler Toolchain

| Target | Triplet | GCC | Binutils |
|--------|---------|-----|----------|
| EE (R5900) | mips64r5900el-ps2-elf | 15.2.0 | 2.45.1 |
| IOP (R3000A) | mipsel-ps2-irx | 15.2.0 | 2.45.1 |
| DVP (VU asm) | DVP-specific | — | 2.45.1 |

Source: ps2toolchain-ee, ps2toolchain-iop, ps2toolchain-dvp

### Installation (Ubuntu)

```bash
sudo apt -y install gcc g++ make cmake patch git texinfo flex bison \
  gettext libgsl-dev libgmp3-dev libmpfr-dev libmpc-dev zlib1g-dev autopoint

export PS2DEV=/usr/local/ps2dev
export PS2SDK=$PS2DEV/ps2sdk
export PATH=$PATH:$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/dvp/bin

git clone https://github.com/ps2dev/ps2dev.git && cd ps2dev && ./build-all.sh
```

Docker alternative: `ps2dev/ps2dev:latest`

Source: ps2dev GitHub

### Key Libraries

| Library | Purpose |
|---------|---------|
| gsKit | GS graphics (framebuffer, primitives, fonts) |
| libpad | Controller input via SIO2 RPC |
| libmc | Memory card access |
| lwIP 2.0.0 | TCP/IP stack (DHCP, DNS, TCP, UDP) |
| audsrv | Audio server (SPU2) |
| fileXio | Extended filesystem I/O |
| ps2http | HTTP client (IOP module) |
| SDL 1.2 | Via ps2sdk-ports |

Source: ps2sdk README, gsKit GitHub, ps2sdk-ports

### Development Workflow

1. Install FMCB + ps2link on PS2, connect via ethernet
2. Compile ELF with ps2sdk
3. `ps2client -h <ps2-ip> execee host:path/to/your.elf`
4. Debug output via `ps2client listen`
5. Iterate: edit → compile → reset → execee

This is the network equivalent of your PS1 serial workflow.

Source: ps2link GitHub, ps2client GitHub

---

## 4. PS2 Boot Process

### Power-On Sequence

1. Both EE and IOP start executing from BIOS ROM at **0xBFC00000**
2. BIOS checks COP0.PRid — branches to EE code (≥0x59) or IOP code
3. **EE:** RDRAM init → kernel copied to 0x80000000 → TLB init → SIF init → EELOAD at 0x00082000 → loads OSDSYS
4. **IOP:** SYSMEM + LOADCORE → remaining modules from IOPBTCONF → EESYNC signals EE

Source: Writing PS2 BIOS in Rust, ps2tek EE Boot

### Hardware State When Your Code Starts

After FMCB launches your ELF:

| Component | State |
|-----------|-------|
| EE | Running, kernel at 0x80000000, syscalls installed |
| IOP | Default modules loaded, SIF operational |
| GS | Initialized by OSDSYS (needs reinit for your use) |
| VU0/VU1 | Available, unconfigured |
| SPU2 | Available via IOP, not active |
| Timers | T3 reserved by BIOS; T0-T2 available |
| RAM | 32MB, kernel in low KSEG0, EELOAD around 0x00082000 |

Source: ps2tek, OSD-Initialization-Libraries

### IOP Reset — Critical for OS Control

The IOP boots with whatever modules FMCB left behind. For a clean OS:

```c
// 1. Tear down existing services
SifExitIopHeap();
LoadFileExit();
SifExitRpc();

// 2. Reset IOP (empty string = default module set)
SifIopReset("", 0);

// 3. Wait for reset to complete
while (!SifIopSync()) ;

// 4. Reinitialize SIF
SifInitRpc(0);

// 5. Load YOUR modules
SifLoadModule("rom0:SIO2MAN", 0, NULL);
SifLoadModule("rom0:PADMAN", 0, NULL);
// ... etc
```

For full control, pass a custom IOPRP image to `SifIopReset()` to replace
the default module set entirely. OPL does this to redirect disc I/O.

Source: ps2sdk iopcontrol.c, OPL IOPCORE

### FreeMCBoot Compatibility

| Model | Compatible |
|-------|-----------|
| All Fat PS2 | Yes |
| Slim (up to BIOS v2.20) | Yes |
| Slim (BIOS v2.30+, date 8c+) | No — Sony removed update feature |

Alternatives for incompatible models: FreeDVDBoot (DVD player exploit),
MechaPwn (Mechacon EEPROM patch for disc boot).

Source: FMCB Compatible Models Chart, ConsoleMods Wiki

---

## 5. Existing PS2 OS Projects

### What Has Been Done

| Project | Type | Status | Key Lesson |
|---------|------|--------|------------|
| PS2 Linux (Sony) | Monolithic Linux 2.2 | Discontinued ~2003 | Used closed-source RTE blob for IOP |
| kernelloader/TGE | Linux bootloader | Working | Open-source SBIOS replacement |
| NetBSD/playstation2 | BSD port | Abandoned 2009 | GCC couldn't handle R5900 (now fixed) |
| U-Boot PS2 | Bootloader | Very limited | IDE works, USB/network broken |
| ps2link | Network loader | Production | Minimal bare-metal init with networking |
| Fredrik Noring patches | Modern Linux | 120-patch series (2019) | Best modern R5900 kernel reference |

Source: Linux for PS2 Wikipedia, kernelloader GitHub, NetBSD blog, ps2-u-boot

### What Has NOT Been Done

- **No microkernel** has been built for any game console
- **No RTOS** (FreeRTOS, Zephyr) has been ported to PS2
- **No custom OS** beyond Linux/NetBSD has been attempted
- ps2sdk is a bare-metal library, not an OS — homebrew runs as single applications

**This is the gap we would fill.**

### Known BIOS Kernel Bugs

The stock EE kernel has confirmed bugs:
- `iWakeupThread` fails when target thread is already running
- `iSuspendThread` doesn't select a new thread → potential deadlock
- Thread-switching can be disabled if interrupt fires before DI
- Every interrupt handler must call `ExitHandler()` immediately before return

Source: PS2 Kernel Patches (PS2-HOME), PS2Tek EE Syscalls

A custom microkernel replaces this entirely.

---

## 6. Microkernel Design

### Why Microkernel, Not Monolithic

The PS2's hardware is **already designed as a microkernel system:**

- The IOP runs independent driver modules (IRX) = servers
- The SIF is a hardware message-passing bus = IPC
- The EE runs application code = client processes
- Each IOP module exports functions via SIF RPC = server endpoints

A microkernel formalizes what the hardware already does.

### Core Kernel Responsibilities (~5,000-10,000 lines)

1. **IPC** — message passing between processes (synchronous, L4-style)
2. **Scheduler** — preemptive priority-based, timer-driven
3. **Memory management** — TLB management, page allocation in 32MB
4. **Interrupt dispatch** — convert hardware interrupts to IPC messages

Everything else runs as userspace server processes.

### L4/MIPS — The Proven Reference

L4/MIPS was built at UNSW in 1997 on the R4700 (MIPS with software TLB):

- **86 cycles / 0.86 us** one-way IPC at 100 MHz
- ~6,000 lines assembly + ~6,500 lines C
- **7 system calls** (vs Mach's 140)
- **12KB binary** (vs Mach's 330KB)
- 48-entry TLB with 8-bit ASID — same as R5900

At 294 MHz, the R5900 should achieve **sub-microsecond IPC** (~200-300 cycles
including R5900-specific 128-bit register save overhead).

Source: L4/MIPS UNSW, Inside L4/MIPS paper, L4 Microkernel Family Wikipedia

### HelenOS/SPARTAN — Modern MIPS Microkernel

HelenOS is an actively maintained microkernel OS with a **MIPS32 port**. Its
SPARTAN kernel supports multitasking, virtual memory, and SMP on MIPS.
This is the most directly relevant existing codebase to study.

Source: HelenOS website, HelenOS GitHub

### Context Switch Cost on R5900

The R5900 has **128-bit GPRs** (32 registers), making context switch heavier
than standard MIPS:

- 32 GPRs × 128 bits = 512 bytes to save/restore
- Plus HI/LO (128-bit each), SA register (R5900-specific)
- Plus COP0 state: Status, EPC, Cause
- Plus FPU: 32 × 32-bit registers

Estimated: **200-300 cycles** (~0.7-1.0 us at 294 MHz). Fast enough for a
microkernel.

Source: frno7/linux R5900 patches, R5900 LinuxMIPS Wiki

### Memory Protection with the TLB

The R5900's 48-entry TLB is functional but used poorly by the stock BIOS
(95% for cache control). A custom OS can use it for real process isolation:

- 48 entries with 8-bit ASID = up to 256 address spaces without flushing
- Page sizes: 4KB to 16MB
- KSEG0/KSEG1 bypass TLB entirely — kernel runs from these
- Software TLB refill handler (standard MIPS, not PS2-specific)

With only 32MB RAM and a small number of server processes, 48 entries is
workable. L4/MIPS was designed for exactly this kind of TLB.

Source: PCSX2 MMU Mini-Series, PS2Tek COP0 Memory Management

### DMA Isolation — The Hard Problem

The PS2 has no IOMMU. DMA writes bypass the TLB and can target any physical
address. A userspace driver with DMA access can corrupt anything.

**Practical solution:** Trusted DMA servers + memory region grants.
- GS Server and VU Server run with elevated privileges
- Kernel pre-allocates physical memory regions for each DMA-capable server
- If a DMA server crashes, the reincarnation server restarts it (MINIX 3 pattern)

Full kernel-mediated DMA validation would add too much latency for 60fps
rendering.

Source: DMA Attack Wikipedia, seL4 Whitepaper

### Proposed Server Architecture

```
┌──────────────────── EE (Kernel Space) ────────────────────────┐
│  BlackrooOS Microkernel                                        │
│  IPC + Scheduler + TLB Management + Interrupt Dispatch         │
│  Target: ~5,000-10,000 lines C+asm                            │
├────────────────────────────────────────────────────────────────┤
│  EE Userspace Servers                                          │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌─────────────────┐  │
│  │ GS Server│ │ VU Server│ │  Shell   │ │ DOS Subsystem   │  │
│  │ (video)  │ │ (render) │ │ (console)│ │ (8086 emulator) │  │
│  └──────────┘ └──────────┘ └──────────┘ └─────────────────┘  │
│  ┌──────────┐ ┌──────────┐                                    │
│  │Dead Seas │ │Reincarn. │                                    │
│  │ (game)   │ │ Server   │                                    │
│  └──────────┘ └──────────┘                                    │
├──────────────────── SIF Bridge ────────────────────────────────┤
│  IOP Servers (IRX modules — already exist in ps2sdk)           │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────────┐ │
│  │ SMAP │ │ PAD  │ │  MC  │ │ CDVD │ │ USB  │ │  AUDSRV  │ │
│  │(net) │ │(ctrl)│ │(card)│ │(disc)│ │(mass)│ │ (sound)  │ │
│  └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────────┘ │
└────────────────────────────────────────────────────────────────┘
```

---

## 7. Networking

### Verdict: PS2 Networking Works — The Myth Is Busted

"PS2 native apps can't access the network" is **false**. PS2 homebrew
networking is mature and production-quality.

### Hardware

| Model | Network Hardware | Status |
|-------|-----------------|--------|
| Fat PS2 | Network Adapter (SCPH-10350) via expansion bay | Working |
| Slim PS2 (SCPH-70000) | Built-in ethernet (SPEED chip) | Working |
| Slim PS2 (SCPH-75000+) | Built-in ethernet (integrated SMAP) | Working |

MAC controller: Sony SPEED chip (CXD9624GG/CXD9731GP).
PHY: National Semiconductor DP83846A/DP83847, STMicro STE100P/S, or Broadcom.
Speed: 10/100 Mbps, full duplex.

Source: Network Adaptor Wiki, SPEED Wiki

### Software Stack

```
EE Application
  └── BSD Socket API (socket, connect, send, recv)
       └── ps2ip (lwIP 2.0.0 wrapper)
            └── SIF RPC
                 └── NETMAN.IRX (network manager)
                      └── SMAP.IRX (ethernet driver)
                           └── DEV9 → SPEED → PHY → RJ45
```

IOP modules to load: `DEV9.IRX`, `NETMAN.IRX`, `SMAP.IRX`, plus either
`ps2ip-nm.irx` (IOP-side stack) or EE-side lwIP.

Source: NETMAN.txt, ps2sdk tcpip_dhcp sample

### Protocol Support (All Confirmed Working)

| Protocol | Status | Used By |
|----------|--------|---------|
| TCP | Production | ps2link, wLaunchELF FTP |
| UDP | Production | ps2link, OPL UDPBD |
| DHCP | Production | ps2sdk sample |
| DNS | Production | ps2sdk built-in |
| HTTP client | Production | ps2http.irx |
| SMBv1 | Production | OPL |
| SMBv2/v3 | Production | smbLaunchELF (libsmb2) |
| NFSv3/v4 | Production | smbLaunchELF (libnfs) |
| FTP server | Production | wLaunchELF |
| NBD server | Production | lwNBD/OPL |

### Throughput

| Configuration | Speed |
|---------------|-------|
| TCP on IOP | ~2.3 MB/s |
| TCP on EE | ~2.8 MB/s |
| UDP on IOP | ~4.4 MB/s |
| SMB (OPL) | ~2.0-2.8 MB/s |
| Theoretical max | 12.5 MB/s (100 Mbps) |

Bottleneck is DEV9/SPEED SSBUS interface, not CPU or link.
For shell access, HTTP, telnet — more than adequate.

Source: PS2-HOME network benchmarks, PSX-Place OPL speed threads

---

## 8. DOS Compatibility Layer

### The Idea

Run an **8086 CPU emulator** as a process on the microkernel, with a BIOS
layer mapping PC INT calls to PS2 hardware. FreeDOS provides the kernel,
COMMAND.COM provides the shell.

### Definitive Proof: DOSBox Already Runs on PS2

Developer belek666 ported DOSBox to PS2 with a **MIPS dynamic recompiler**.
Commander Keen runs at full speed. Wolf3D, Prince of Persia, SimCity 2000
are playable. This is a far heavier emulator than what we need.

Source: PSX-Place DOSBox PS2, VOGONS DOSBox PS2, ps2dev forums

### Recommended Approach: 8086tiny + FreeDOS

**8086tiny:**
- Single C file, under 25KB source
- MIT license (no restrictions)
- Runs MS-DOS, Windows 3.0, AutoCAD
- Custom 6KB BIOS (NASM source included)
- Proven on embedded ARM, MCUs, Raspberry Pi
- Uses 4 trap opcodes (0F 00-03) for host I/O — map these to PS2 SDK calls

Source: 8086tiny GitHub, 8086tiny documentation

**FreeDOS:**
- GPL v2
- 8086-compatible kernel (~45KB binary)
- FreeCOM (COMMAND.COM replacement)
- Explicitly designed for real and emulated 8086

Source: FreeDOS kernel GitHub, FreeDOS boot sequence

### BIOS INT Mapping to PS2 Hardware

| PC INT | Service | PS2 Mapping |
|--------|---------|-------------|
| INT 10h | Video | GS framebuffer (80x25 text grid) |
| INT 13h | Disk | MC, HDD, USB, or RAM disk image |
| INT 14h | Serial | SIO (your existing serial driver!) |
| INT 16h | Keyboard | USB keyboard via IOP, or Lightspan protocol |
| INT 1Ah | Clock | EE timer |
| INT 08h | Timer | PS2 timer interrupt (18.2 Hz) |

INT 21h (DOS API) is handled by FreeDOS kernel internally — the BIOS
does not need to implement it.

### Minimum BIOS for COMMAND.COM Boot

Only 5 INT handlers needed to boot COMMAND.COM:
1. INT 13h AH=02 (read disk sectors)
2. INT 10h AH=0E (teletype output)
3. INT 16h AH=00 (read keystroke)
4. INT 12h (memory size)
5. INT 1Ah (clock/tick count)

Everything else can be stubbed and added incrementally.

### Performance Estimate

| Factor | Value |
|--------|-------|
| R5900 clock | 294 MHz |
| Emulation overhead | ~30x per instruction |
| Emulated speed | ~10 MHz equivalent |
| Original 8086 | 4.77 MHz |
| Result | **~2x faster than real 8086** |

For COMMAND.COM and text-mode DOS apps, this is far more than sufficient.
COMMAND.COM spends most of its time waiting for keyboard input.

### Memory Footprint

| Component | Size |
|-----------|------|
| 8086 emulator code | ~50-100 KB compiled |
| BIOS | ~6 KB |
| Emulated PC RAM | 1 MB |
| FreeDOS kernel | ~45 KB (in emulated RAM) |
| COMMAND.COM | ~64 KB (in emulated RAM) |
| **Total PS2 memory** | **~1.2 MB of 32 MB** |

### Architecture (as a Microkernel Process)

```
┌─────────── DOS Process (runs on EE) ──────────────┐
│                                                     │
│  COMMAND.COM / DOS Programs (.COM / .EXE)          │
│       ↕ INT 21h                                    │
│  FreeDOS Kernel (kernel.sys, ~45KB)                │
│       ↕ INT 10h/13h/16h/1Ah                       │
│  PC BIOS Layer (maps INTs to PS2 hardware)         │
│       ↕ trap opcodes (0F 00-03)                    │
│  8086tiny CPU Emulator (~25KB source)              │
│       ↕ IPC messages to kernel                     │
│  [GS Server] [FS Server] [Pad Server]             │
│                                                     │
└─────────────────────────────────────────────────────┘
```

---

## 9. Legal and Licensing

**Disclaimer:** This is research, not legal advice.

### ps2sdk License: Academic Free License 2.0

- **Commercial use explicitly permitted** — you can sell Dead Seas
- Not copyleft — derivative works can use any license
- Attribution required (retain copyright notices)

Source: ps2sdk LICENSE, ps2sdk README

### Sony Trademark Rules

**Cannot use:** "PlayStation", "PS2", PS logos, controller symbols in branding.

**Can use:** "Compatible with PS2 hardware" or "runs on PS2" with disclaimer
under nominative fair use doctrine. Include: *"PlayStation is a registered
trademark of Sony Interactive Entertainment Inc. This product is not
affiliated with, endorsed, or sponsored by Sony."*

**"MIPS R5900" label:** Defensible as descriptive technical use. MIPS is
trademarked (by MIPS Technologies/Wave Computing) but using it to describe
CPU compatibility is standard nominative use. The R5900 is more accurately
described as "Toshiba R5900" or "Emotion Engine" since Sony/Toshiba, not
MIPS Technologies, designed it.

Source: Sony Trademark Notice, MIPS Trademark Justia, R5900 LinuxMIPS Wiki

### Legal Precedent

| Case | Ruling | Relevance |
|------|--------|-----------|
| Sony v. Connectix (2000) | RE of PlayStation BIOS is fair use | Protects our use of RE'd interfaces |
| Sega v. Accolade (1992) | RE for interoperability is fair use | Foundational precedent |
| Bleem! (2000) | Won all counts but bankrupted by litigation | Pyrrhic victory warning |

**No confirmed cases** of Sony suing PS2 homebrew distributors. Enforcement
has focused on modchips, emulators, and piracy — not original homebrew.

Source: Sony v. Connectix Wikipedia, Sega v. Accolade Wikipedia

### Distribution Risk Assessment

| Activity | Risk |
|----------|------|
| Building OS with ps2sdk | Low |
| Free download distribution | Low |
| Selling Dead Seas (digital) | Low-Medium |
| Selling physical media | Medium |
| Using "PS2" in descriptive text + disclaimer | Low |
| Labeling as "MIPS R5900" application | Low |
| Crowdfunding development | Low |
| Including PS2 BIOS in distribution | **High — never do this** |
| Selling pre-modded consoles | **High — clear DMCA risk** |

### Recommended Licensing

- **BlackrooOS kernel:** MIT or BSD (compatible with AFL 2.0, allows proprietary games)
- **Dead Seas game:** Proprietary / commercial (runs as independent process)
- **Shared libraries:** MIT or BSD for simplicity

Games running *on* a GPL OS are NOT required to be GPL (same principle as
proprietary software on Linux).

Source: GPL FAQ, Academic Free License Wikipedia

---

## 10. Implementation Roadmap

### Phase 0: Toolchain Setup (Week 1)

- [ ] Install ps2dev toolchain on Ubuntu
- [ ] Build and run hello world ELF on PS2 (via FMCB + wLaunchELF)
- [ ] Set up ps2link + ps2client for network development workflow
- [ ] Study ps2sdk samples: pad, memcard, networking, gsKit
- [ ] Read kernelloader and ps2link source for hardware init reference

### Phase 1: Bare-Metal Boot (Weeks 2-3)

- [ ] Write custom crt0/startup assembly for R5900
- [ ] Initialize SIF, reset IOP with clean module set
- [ ] Initialize GS for text console output (80x25)
- [ ] Set up one hardware timer for scheduling
- [ ] Install custom exception vectors (TLB refill, general, interrupt)
- [ ] Implement kprintf to GS framebuffer + serial (if available)
- [ ] Load pad, memcard IOP modules — verify controller input

### Phase 2: Kernel Core (Weeks 4-8)

- [ ] Implement IPC: synchronous message passing (L4-style, register-based)
- [ ] Implement scheduler: preemptive, priority-based, timer-driven
- [ ] Implement memory allocator: page-based, 32MB carve-up
- [ ] Implement TLB management: process address spaces, refill handler
- [ ] Context switch: save/restore 128-bit GPRs, COP0 state
- [ ] System calls: send, receive, yield, alloc, map
- [ ] Two processes running: kernel + shell

### Phase 3: Servers (Weeks 8-14)

- [ ] GS Server: framebuffer management, text rendering, mode setting
- [ ] Pad Server: controller input via IOP padman, event distribution
- [ ] FS Server: memory card, USB, HDD access via IOP modules
- [ ] Network Server: lwIP stack, TCP/IP, DHCP, DNS
- [ ] Shell process: command-line interface (port from PS1 shell.c)

### Phase 4: DOS Subsystem (Weeks 14-18)

- [ ] Port 8086tiny to R5900 (compile with ee-gcc)
- [ ] Map trap opcodes to PS2 SDK calls
- [ ] Implement minimal BIOS: INT 10h, 13h, 16h, 12h, 1Ah
- [ ] Create FAT16 disk image with FreeDOS kernel.sys + COMMAND.COM
- [ ] Boot COMMAND.COM on PS2
- [ ] Map keyboard input (USB keyboard or controller → scancode)

### Phase 5: Dead Seas Integration (Weeks 18-22)

- [ ] Port Dead Seas to run as a BlackrooOS process
- [ ] Request GS framebuffer via IPC
- [ ] Request pad input via IPC
- [ ] Audio via IOP audsrv module
- [ ] Game loop with yield() for other processes

### Phase 6: Distribution (Weeks 22-26)

- [ ] Create installer/boot media
- [ ] Documentation for end users
- [ ] Release as "MIPS R5900" application
- [ ] Package: mips32/ (PS1), mips64/ (PS2/BlackrooOS), linux-mips/ (Linux)

### What Transfers from PS1 Bootloader

| PS1 Component | PS2 Equivalent |
|---------------|----------------|
| main.c boot sequence | Same pattern, add SIF init |
| shell.c command parser | Becomes shell server process |
| serial.c SIO driver | EE has SIO too, nearly identical |
| memcard.c | IOP module handles it, send IPC |
| menu.c GPU output | GS is more powerful but same concept |
| MIPS R3000 knowledge | R5900 is a superset, IOP IS an R3000 |

---

## 11. Open Questions

### Technical

1. **LL/SC emulation:** How does Linux handle the missing atomic instructions
   on R5900? Study Fredrik Noring's patches.

2. **VU context save:** If multiple processes use VU0 (macro mode), what's
   the save/restore cost? Is it worth supporting?

3. **Deckard compatibility:** How much does the PPC-based IOP on late slims
   break? Test early with both fat and slim models.

4. **Scratchpad for kernel:** The 16KB SPR at 0x70000000 is fast — should the
   kernel's hot data structures (ready queue, TLB refill data) live there?

5. **IOP as protection boundary:** The IOP is physically separate from the
   EE. Is this sufficient isolation for driver servers, or do we need TLB
   protection on the EE side too?

### Project

6. **PS1 vs PS2 priority:** Does the PS2 microkernel become the main project,
   or does Blackroo Linux (PS1) remain the focus?

7. **PCBIOS:** The user mentioned "we have a pcbios" — needs clarification
   on what this refers to.

8. **Keyboard input:** For DOS compatibility, USB keyboard support is needed.
   ps2sdk has USB keyboard drivers. Alternatively, the Lightspan keyboard
   protocol could be used (already researched for PS1 SuperCard).

---

## Key Reference Projects to Study

In priority order:

1. **kernelloader/TGE** — open-source SBIOS, hardware init
   https://github.com/rickgaiser/kernelloader

2. **Fredrik Noring's Linux patches** — R5900 kernel development reference
   https://lore.kernel.org/linux-mips/cover.1567326213.git.noring@nocrew.org/

3. **L4/MIPS source** — MIPS microkernel IPC + TLB design
   https://l4mips.sourceforge.net/

4. **HelenOS/SPARTAN** — modern MIPS32 microkernel
   https://github.com/HelenOS/helenos

5. **ps2link** — minimal bare-metal PS2 init with networking
   https://github.com/ps2dev/ps2link

6. **Open PS2 Loader** — IOP management, networking, syscall hooking
   https://github.com/ps2homebrew/Open-PS2-Loader

7. **8086tiny** — embeddable 8086 emulator
   https://github.com/adriancable/8086tiny

8. **DOSBox PS2** — proof that x86 emulation works on R5900
   https://www.psx-place.com/threads/dosbox-by-belek666.19030/

---

*Last updated: 2026-04-16*
*BlackrooOS — Because microkernels like to play too*
