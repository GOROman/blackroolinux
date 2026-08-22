# Blackroo Linux - Bootloader Design

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: UniROM as the upload path, the 8 MB mod.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> First-stage bootloader for PlayStation 1, inspired by PS2 kernelloader

---

## Overview

The Blackroo bootloader is a PlayStation 1 executable (PS-EXE) that provides a menu-driven interface for loading and launching the Linux kernel. It is inspired by the [PS2 kernelloader](https://sourceforge.net/p/kernelloader/kernelloader/ci/master/tree/) project, adapted for PS1 hardware constraints.

### Design Goals

1. **Load Linux kernel** from multiple sources (serial, memory card, CD)
2. **Configure hardware** (RAM size, serial baud rate)
3. **Provide a boot menu** via GPU console output
4. **Handle initrd** loading and placement
5. **Be small** — must leave maximum RAM for the kernel
6. **Build with PSn00bSDK** — modern, open-source PS1 SDK

---

## Boot Chain

### Full Boot Sequence

```
┌─────────────────────┐
│ 1. Power On          │
│    BIOS ROM          │
│    (0xBFC00000)      │
└──────────┬──────────┘
           │
           ▼
┌─────────────────────┐
│ 2. Bootstrap         │    Method A: FreePSXBoot (memory card exploit)
│    (one of these)    │    Method B: UniROM (cheat cartridge)
│                      │    Method C: Mod chip + CD boot
└──────────┬──────────┘
           │
           ▼
┌─────────────────────┐
│ 3. Blackroo          │    PS-EXE application built with PSn00bSDK
│    Bootloader        │    Loaded to RAM at 0x80010000 (PS-EXE default)
│    (~32-64KB)        │    Displays boot menu on TV via GPU
└──────────┬──────────┘
           │  User selects boot source
           ▼
┌─────────────────────┐
│ 4. Kernel Load       │    Serial: Receive via SIO1 from host
│                      │    MemCard: Read from card RAID
│                      │    CD: Read from disc data track
└──────────┬──────────┘
           │
           ▼
┌─────────────────────┐
│ 5. Hardware Setup    │    Configure RAM_SIZE register
│                      │    Disable interrupts
│                      │    Flush cache
│                      │    Set up kernel arguments
└──────────┬──────────┘
           │
           ▼
┌─────────────────────┐
│ 6. Kernel Entry      │    Jump to kernel entry point
│    Linux boots       │    head.S takes over
│                      │    Serial console active
└─────────────────────┘
```

### Comparison with PS2 Kernelloader

| Feature | PS2 Kernelloader | Blackroo Bootloader |
|---------|-----------------|---------------------|
| Bootstrap | FreeMcBoot (PS2 exploit) | FreePSXBoot / UniROM |
| SDK | PS2SDK | PSn00bSDK |
| GPU output | GS (Graphics Synthesizer) | GPU (PlayStation GPU) |
| Kernel source | HDD, USB, memory card, network | Serial, memory card, CD |
| RAM setup | Configure EE RAM + IOP | Configure RAM_SIZE register |
| Kernel format | ELF (compressed) | ECOFF or raw binary |
| InitRD | Loaded separately | Embedded in ECOFF or loaded separately |
| Resolution | 640x448 (interlaced) | 320x240 (NTSC) or 320x256 (PAL) |

---

## Bootloader Architecture

### Memory Layout During Boot

```
0x80000000 ┌──────────────────────────────────────────┐
           │ Exception vectors (kernel will set these) │
0x80001000 ├──────────────────────────────────────────┤
           │ (reserved for kernel)                     │
0x80010000 ├──────────────────────────────────────────┤
           │ Bootloader code + data (~32-64KB)        │  ← PS-EXE loads here
           │ ├── Code (.text)                         │
           │ ├── Data (.data, .rodata)                │
           │ ├── BSS (.bss)                           │
           │ ├── Stack (grows down from end)           │
           │ └── Serial receive buffer                │
0x80020000 ├──────────────────────────────────────────┤ (approximate)
           │ Free RAM for kernel loading               │
           │                                           │
           │ Kernel loaded here temporarily            │
           │ (then relocated to final address)         │
           │                                           │
0x801FFFFF └──────────────────────────────────────────┘ (2MB boundary)
           │ (extended RAM if 8MB mod)                 │
0x807FFFFF └──────────────────────────────────────────┘ (8MB boundary)
```

### Component Diagram

```
┌─────────────────────────────────────────┐
│              Blackroo Bootloader         │
│                                         │
│  ┌──────────────┐  ┌────────────────┐  │
│  │  Boot Menu    │  │  GPU Renderer  │  │
│  │  (main loop)  │  │  (text output) │  │
│  └──────┬───────┘  └────────────────┘  │
│         │                               │
│  ┌──────┴───────────────────────────┐  │
│  │        Kernel Loader              │  │
│  │  ┌──────────┐ ┌──────────────┐   │  │
│  │  │ Serial   │ │ Memory Card  │   │  │
│  │  │ Loader   │ │ Loader       │   │  │
│  │  └──────────┘ └──────────────┘   │  │
│  │  ┌──────────┐ ┌──────────────┐   │  │
│  │  │ CD-ROM   │ │ ECOFF/Binary │   │  │
│  │  │ Loader   │ │ Parser       │   │  │
│  │  └──────────┘ └──────────────┘   │  │
│  └──────────────────────────────────┘  │
│                                         │
│  ┌──────────────────────────────────┐  │
│  │  Hardware Configuration           │  │
│  │  ├── RAM detection/config         │  │
│  │  ├── Cache flush                  │  │
│  │  ├── Interrupt disable            │  │
│  │  └── Kernel argument setup        │  │
│  └──────────────────────────────────┘  │
└─────────────────────────────────────────┘
```

---

## Boot Menu Design

### Screen Layout (320x240 NTSC)

```
┌──────────────────────────────────────────────┐
│                                              │
│     BLACKROO LINUX BOOTLOADER v1.0           │
│     ──────────────────────────               │
│                                              │
│     RAM: 8192 KB detected                    │
│     Serial: 115200 baud (SIO1)               │
│     Cards: 4 found (512 KB total)            │
│                                              │
│     ─── Boot Source ───                      │
│                                              │
│  >  [1] Serial Upload (115200)               │
│     [2] Serial Upload (518400 fast)          │
│     [3] Memory Card RAID                     │
│     [4] CD-ROM                               │
│     [5] Hardware Info                        │
│     [6] Memory Card Format                   │
│                                              │
│     Use D-pad to select, X to confirm        │
│                                              │
│     Status: Ready                            │
└──────────────────────────────────────────────┘
```

### Menu Options

| Option | Function | Description |
|--------|----------|-------------|
| Serial Upload (slow) | Wait for kernel via SIO1 at 115200 baud | Standard nops upload |
| Serial Upload (fast) | Wait for kernel via SIO1 at 518400 baud | UniROM fast mode |
| Memory Card RAID | Read kernel from memory card storage | Requires pre-loaded kernel |
| CD-ROM | Read kernel from CD data track | Standard disc boot |
| Hardware Info | Display detected hardware | RAM, cards, board revision |
| Memory Card Format | Format cards for Blackroo | Write headers, create filesystem |

---

## Serial Kernel Loading Protocol

### Protocol Design

The bootloader implements a simple protocol over SIO1 for receiving the kernel from a host computer:

```
Phase 1: Handshake
  Bootloader → Host:  "BLKR" (4 bytes, magic)
  Host → Bootloader:  "BLKR" (echo confirms connection)

Phase 2: Header
  Host → Bootloader:  [4 bytes] Kernel size (little-endian)
  Host → Bootloader:  [4 bytes] Entry point address
  Host → Bootloader:  [4 bytes] Load address
  Host → Bootloader:  [4 bytes] InitRD size (0 if none)
  Host → Bootloader:  [4 bytes] InitRD load address
  Host → Bootloader:  [4 bytes] Flags (bit 0: compressed, bit 1: has initrd)
  Host → Bootloader:  [4 bytes] CRC32 of header
  Bootloader → Host:  "OK" or "ER" (header valid/invalid)

Phase 3: Kernel Data
  Host → Bootloader:  [N bytes] Kernel binary data
  Bootloader → Host:  "OK" (after each 4KB block received)
  Bootloader → Host:  [4 bytes] CRC32 of received data

Phase 4: InitRD Data (if present)
  Host → Bootloader:  [M bytes] InitRD data
  Bootloader → Host:  "OK" (after each 4KB block)
  Bootloader → Host:  [4 bytes] CRC32 of received data

Phase 5: Launch
  Host → Bootloader:  "GO" (launch command)
  Bootloader:         Configure hardware, jump to kernel
```

### Host-Side Upload Tool

```bash
#!/bin/bash
# blackroo_upload.sh - Upload kernel to Blackroo bootloader
# Usage: ./blackroo_upload.sh /dev/ttyUSB0 kernel.bin [initrd.img]

PORT=$1
KERNEL=$2
INITRD=${3:-}
BAUD=115200

# Configure serial port
stty -F $PORT $BAUD cs8 -parenb -cstopb raw

# Send magic + header
python3 -c "
import struct, sys, zlib

kernel = open('$KERNEL', 'rb').read()
initrd = open('$INITRD', 'rb').read() if '$INITRD' else b''

flags = 0
if initrd: flags |= 0x02

header = struct.pack('<IIIIII',
    len(kernel),        # Kernel size
    0x80001000,         # Entry point (typical)
    0x80001000,         # Load address
    len(initrd),        # InitRD size
    0x80100000,         # InitRD load address (1MB offset)
    flags               # Flags
)
header += struct.pack('<I', zlib.crc32(header) & 0xFFFFFFFF)

sys.stdout.buffer.write(b'BLKR')
sys.stdout.buffer.write(header)
sys.stdout.buffer.write(kernel)
if initrd:
    sys.stdout.buffer.write(initrd)
sys.stdout.buffer.write(b'GO')
" > $PORT

echo "Upload complete. Kernel: $(wc -c < $KERNEL) bytes"
```

### Compatibility with nops

The bootloader should also support direct PS-EXE upload via the existing `nops` tool for backward compatibility. When the bootloader detects a standard PS-EXE header instead of the "BLKR" magic, it falls back to raw PS-EXE execution.

---

## Memory Card Kernel Storage

### Card Layout for Bootable Kernel

```
Card 0 (128KB):
  Block 0:      Blackroo header (BU_ID, size, serial, sequence)
  Block 1:      Boot config block
                  bytes 0-3:   Magic ("BOOT")
                  bytes 4-7:   Kernel size
                  bytes 8-11:  Kernel blocks (start)
                  bytes 12-15: InitRD size
                  bytes 16-19: InitRD blocks (start)
                  bytes 20-23: Entry point
                  bytes 24-27: Load address
                  bytes 28-31: Checksum
                  bytes 32-127: Reserved / boot args string
  Blocks 2-1023: Kernel data (start)

Cards 1-7: Continuation of kernel + initrd data
```

### Kernel Compression

To fit a kernel + initrd on memory cards, compression is essential:

| Data | Uncompressed | gzip -9 | Ratio |
|------|-------------|---------|-------|
| linux.elf (kernel) | ~1,200 KB | ~400 KB | 33% |
| initrd.img (minimal) | ~500 KB | ~200 KB | 40% |
| **Total** | **~1,700 KB** | **~600 KB** | **35%** |

600KB compressed fits in 5 memory cards (5 x 128KB = 640KB). With 8 cards (1MB), there's room for a more complete initrd.

**Decompression in bootloader:** The bootloader must include a gzip decompressor. PSn00bSDK or the kernel's own inflate code can be used. Decompression of 600KB takes ~2-5 seconds on the R3000A at 33MHz.

---

## Hardware Configuration

### Pre-Kernel Setup

Before jumping to the kernel entry point, the bootloader must:

```c
void prepare_for_kernel(uint32_t ram_mb) {
    /* 1. Disable all interrupts */
    uint32_t sr;
    asm volatile("mfc0 %0, $12" : "=r"(sr));
    sr &= ~0x0000FF01;  /* Clear IM bits and IEc */
    asm volatile("mtc0 %0, $12" : : "r"(sr));

    /* 2. Acknowledge all pending interrupts */
    *(volatile uint16_t *)0x1F801070 = 0x0000;

    /* 3. Configure RAM size */
    volatile uint32_t *ram_reg = (volatile uint32_t *)0x1F801060;
    switch (ram_mb) {
        case 8:  *ram_reg = 0x0B88; break;
        case 4:  *ram_reg = 0x0988; break;
        default: *ram_reg = 0x0888; break;  /* 2MB */
    }

    /* 4. Stop all DMA channels */
    *(volatile uint32_t *)0x1F8010F0 = 0x07654321;  /* DPCR: all off */

    /* 5. Flush instruction cache */
    /* R3000 cache flush: isolate cache, write zeros, de-isolate */
    asm volatile(
        "mfc0 $t0, $12\n"
        "ori  $t0, 0x10000\n"   /* Set Isc bit (isolate cache) */
        "mtc0 $t0, $12\n"
        "nop\n"
        /* Write to cache lines to invalidate */
        "li   $t1, 0\n"
        "li   $t2, 4096\n"     /* 4KB I-cache */
        "1: sw $zero, 0($t1)\n"
        "addiu $t1, $t1, 16\n"
        "bne  $t1, $t2, 1b\n"
        "nop\n"
        /* De-isolate */
        "mfc0 $t0, $12\n"
        "li   $t1, ~0x10000\n"
        "and  $t0, $t0, $t1\n"
        "mtc0 $t0, $12\n"
        "nop\n"
        ::: "$t0", "$t1", "$t2", "memory"
    );

    /* 6. Jump to kernel */
    typedef void (*kernel_entry_t)(void);
    kernel_entry_t entry = (kernel_entry_t)KERNEL_ENTRY_ADDR;
    entry();

    /* Never returns */
    __builtin_unreachable();
}
```

### Kernel Arguments

The Linux kernel on MIPS expects arguments passed via registers or a fixed memory location:

```c
/* Place kernel command line at a known address */
#define KERNEL_ARGS_ADDR 0x80000400

void set_kernel_args(const char *cmdline) {
    char *args = (char *)KERNEL_ARGS_ADDR;
    strncpy(args, cmdline, 256);

    /* Example command lines: */
    /* "console=ttyS0,115200 root=/dev/ram0 rw" */
    /* "console=ttyS0,115200 root=/dev/bu0 ro" */
}
```

---

## PSn00bSDK Build Integration

### Project Structure

```
bootloader/
├── src/
│   ├── main.c              # Entry point, main menu
│   ├── gpu.c               # GPU text rendering
│   ├── gpu.h
│   ├── serial.c            # SIO1 serial communication
│   ├── serial.h
│   ├── memcard.c           # Memory card access (SIO0)
│   ├── memcard.h
│   ├── cdrom.c             # CD-ROM access
│   ├── cdrom.h
│   ├── inflate.c           # gzip decompression
│   ├── inflate.h
│   ├── hardware.c          # RAM detection, cache, interrupts
│   ├── hardware.h
│   └── config.h            # Build configuration
├── Makefile                # PSn00bSDK Makefile
├── PSn00b.cmake            # CMake configuration
└── iso/                    # CD image resources (if building CD)
```

### Makefile (PSn00bSDK)

```makefile
# Blackroo Bootloader - PSn00bSDK Makefile

TARGET = bootloader
TYPE = ps-exe

SRCS = src/main.c src/gpu.c src/serial.c src/memcard.c \
       src/cdrom.c src/inflate.c src/hardware.c

include $(PSN00BSDK)/share/psn00bsdk/template.mk
```

See `docs/08-BUILD-SYSTEM.md` for the Docker environment that provides PSn00bSDK.

---

## RAM Auto-Detection (Bootloader Phase)

The bootloader can detect installed RAM before the kernel boots:

```c
uint32_t detect_ram_size(void) {
    volatile uint32_t *ram_reg = (volatile uint32_t *)0x1F801060;

    /* Configure for 8MB (maximum) */
    *ram_reg = 0x0B88;

    /* Use uncached addresses to avoid cache effects */
    volatile uint32_t *base = (volatile uint32_t *)0xA0000000;
    volatile uint32_t *at_2mb = (volatile uint32_t *)0xA0200000;
    volatile uint32_t *at_4mb = (volatile uint32_t *)0xA0400000;

    /* Write unique patterns */
    *base = 0xDEADBEEF;
    *at_2mb = 0xCAFEBABE;
    *at_4mb = 0x12345678;

    /* Read back and check for mirroring */
    if (*base != 0xDEADBEEF) {
        /* Something is very wrong */
        *ram_reg = 0x0888;
        return 2;
    }

    if (*at_4mb == 0x12345678 && *at_2mb == 0xCAFEBABE) {
        /* 4MB and 2MB are distinct from base — 8MB */
        return 8;
    }

    if (*at_2mb == 0xCAFEBABE && *base == 0xDEADBEEF) {
        /* 2MB is distinct, 4MB mirrors — 4MB */
        *ram_reg = 0x0988;
        return 4;
    }

    /* Default: 2MB */
    *ram_reg = 0x0888;
    return 2;
}
```

---

## CD Boot Support

### Bootable CD Layout

```
ISO 9660 Mode 2/XA:
  SYSTEM.CNF          # PlayStation boot config
  PSX.EXE             # Blackroo bootloader (PS-EXE)
  KERNEL.BIN          # Compressed Linux kernel
  INITRD.IMG          # Compressed initrd
```

### SYSTEM.CNF

```
BOOT = cdrom:\PSX.EXE;1
TCB = 4
EVENT = 10
STACK = 801FFFF0
```

### CD Read for Kernel Loading

```c
/* Using PSn00bSDK CD-ROM functions */
#include <psxcd.h>

int load_kernel_from_cd(void *dest, const char *filename) {
    CdlFILE file;

    if (!CdSearchFile(&file, filename)) {
        return -1;  /* File not found */
    }

    /* Calculate sectors */
    int sectors = (file.size + 2047) / 2048;

    /* Set read position */
    CdControl(CdlSetloc, (u_char *)&file.pos, 0);

    /* Read data */
    CdRead(sectors, (u_long *)dest, CdlModeSpeed);

    /* Wait for completion */
    CdReadSync(0, 0);

    return file.size;
}
```

---

## Error Handling

### Boot Failure Recovery

```
If serial upload fails:
  → Display error on GPU
  → Return to boot menu

If memory card read fails:
  → Show which card/block failed
  → Offer retry or return to menu

If kernel is corrupt (bad CRC):
  → Display "Kernel checksum mismatch"
  → Return to boot menu

If RAM detection fails:
  → Default to 2MB (safe mode)
  → Display warning
  → Allow manual override from menu

If no boot source available:
  → Display "No kernel found"
  → Wait for serial connection
```

---

## Future Extensions

| Feature | Priority | Description |
|---------|----------|-------------|
| TFTP boot | Low | If network adapter existed |
| Kernel on PicoMemcard | Medium | Boot from large virtual card |
| Dual kernel support | Low | Boot menu with kernel A/B selection |
| Boot counter | Low | Track successful/failed boots |
| Kernel command line editor | Medium | Edit boot args from menu |
| Automatic boot timeout | Medium | Auto-boot after 5 seconds if default source exists |

---

*Blackroo Linux Bootloader Design Document*
