# Technical White Paper: PS2 Memory Card Analysis for Blackroo Linux

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: UniROM as the upload path.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> Investigating PlayStation 2 memory card hardware, protocols, and potential compatibility with the PS1 SIO0 bus

---

## Abstract

The PlayStation 2 memory card offers 8MB of storage — 64x the capacity of a PS1 card. If PS2 cards could be used with the PS1 hardware (either natively or through an adapter), it would dramatically change the storage landscape for Blackroo Linux. This paper analyzes the PS2 memory card's hardware, protocol, filesystem, and evaluates feasibility of cross-generation use.

---

## 1. PS2 Memory Card Specifications

### 1.1 Hardware Comparison

| Parameter | PS1 Memory Card | PS2 Memory Card |
|-----------|----------------|-----------------|
| Capacity | 128 KB (1 Mbit) | 8 MB (64 Mbit) |
| Page size | 128 bytes | 512 bytes |
| Pages per cluster | 2 (256 bytes) | 2 (1024 bytes) |
| Erase block size | 128 bytes | 16 KB (16 pages) |
| Interface | SIO0 (serial, 250kHz) | SIO2 (serial, faster) |
| Protocol | Simple command/response | MagicGate encrypted |
| Supply voltage | 3.3V | 3.3V |
| Physical connector | 10-pin | 10-pin (different keying) |
| Encryption | None | MagicGate (DES-based) |
| Filesystem | Flat directory (15 slots) | Sony's custom FS (FAT-like) |
| Write endurance | ~100,000 cycles | ~100,000 cycles |

### 1.2 Physical Connector Difference

```
PS1 Memory Card Connector (10-pin):
  ┌───────────────────┐
  │ 1  2  3  4  5     │
  │ 6  7  8  9  10    │
  └───────────────────┘
  Pin 1: DATA    Pin 6: /CS1
  Pin 2: CMD     Pin 7: CLOCK
  Pin 3: +7.6V   Pin 8: /CS2 (unused)
  Pin 4: GND     Pin 9: /ACK
  Pin 5: +3.3V   Pin 10: GND

PS2 Memory Card Connector (different physical shape):
  ┌───────────────────┐
  │ Same signals but  │
  │ different physical │
  │ card dimensions   │
  └───────────────────┘
  Signals are similar but card slot is physically incompatible.
  PS2 cards are physically larger and keyed differently.
```

**Physical incompatibility:** PS2 memory cards do not fit in PS1 memory card slots. An adapter or custom hardware would be required.

---

## 2. PS2 Memory Card Protocol (SIO2)

### 2.1 Communication Interface

The PS2 uses a different I/O controller called **SIO2** (not the PS1's SIO0):

- **SIO2** is a more advanced serial controller found only on the PS2's IOP (I/O Processor)
- Uses a different register set (0x1F808200-0x1F80827F on IOP)
- Supports higher clock speeds
- Includes DMA support for bulk transfers
- Handles MagicGate encryption handshake

### 2.2 MagicGate Encryption

PS2 memory cards implement MagicGate copy protection:

1. **Authentication:** Card and console exchange encrypted challenges
2. **Session key:** A unique session key is derived per connection
3. **Data encryption:** Save data can optionally be encrypted
4. **Key storage:** Console and card each have secret keys burned into silicon

**Impact for Linux:** MagicGate is mandatory for PS2 card access. Without completing the authentication handshake, the card will not respond to read/write commands. This is fundamentally different from PS1 cards, which have no encryption.

### 2.3 PS2 Memory Card Commands

| Command | Code | Description |
|---------|------|-------------|
| Probe | 0x11 | Check card presence |
| Auth | 0x12-0x14 | MagicGate authentication (multi-step) |
| Read Page | 0x23 | Read 512-byte page |
| Write Page | 0x24 | Write 512-byte page |
| Erase Block | 0x25 | Erase 16KB block |
| Get Specs | 0x26 | Read card specifications |
| Set Terminator | 0x27 | Set end-of-command byte |

### 2.4 Read Sequence (PS2)

```
1. Host sends: [0x81] [0x23] (read page command)
2. Card responds with ACK
3. Host sends: page address (4 bytes, LSB first)
4. Card sends: 512 bytes of page data
5. Card sends: ECC bytes (16 bytes)
6. Card sends: status byte (0x2B = success)
```

This is significantly different from the PS1 protocol (which uses 0x52 for read, 128-byte blocks, no ECC).

---

## 3. PS2 Memory Card Filesystem

### 3.1 Structure Overview

The PS2 memory card uses a custom filesystem developed by Sony:

```
Superblock (page 0 of erase block 0):
  bytes 0-27:   "Sony PS2 Memory Card File System"
  bytes 28-29:  Version (1.2 typical)
  bytes 30-31:  Page size (512)
  bytes 32-33:  Pages per cluster (2)
  bytes 34-35:  Pages per erase block (32)
  bytes 36-39:  Total clusters
  bytes 40-43:  First allocatable cluster
  bytes 44-47:  Last allocatable cluster
  bytes 48-51:  Root directory cluster
  bytes 52-55:  Backup block 1
  bytes 56-59:  Backup block 2
  bytes 64-71:  Card type / flags
  bytes 128-191: Indirect FAT cluster list

Cluster size: 1024 bytes (2 x 512-byte pages)
Erase block: 16 KB (32 pages = 16 clusters)
Usable space: ~7.6 MB after filesystem overhead
```

### 3.2 FAT (File Allocation Table)

The PS2 card uses a FAT-like allocation system:

- **Indirect FAT:** A table of cluster pointers to FAT pages
- **FAT entries:** 32-bit cluster numbers (unallocated = 0xFFFFFFFF, end = 0xFFFFFFFF)
- **Cluster chain:** Files are stored as linked lists of clusters
- **Free space tracking:** Via FAT scan (no free space bitmap)

### 3.3 Directory Structure

```
Directory Entry (64 bytes):
  bytes 0-3:    Mode (permissions, file type)
                  0x0427 = directory
                  0x0417 = file
  bytes 4-7:    Length (file size in bytes)
  bytes 8-11:   Created timestamp
  bytes 12-15:  Sector (first cluster)
  bytes 16-19:  Modified timestamp
  bytes 20-23:  Attributes
  bytes 24-27:  (reserved)
  bytes 28-31:  (padding)
  bytes 32-63:  Name (32 bytes, null-terminated)
```

### 3.4 Filesystem Comparison

| Feature | PS1 Card FS | PS2 Card FS | ext2 (Linux) |
|---------|------------|-------------|--------------|
| Max files | 15 | ~thousands | ~millions |
| Max file size | 8 KB x 15 = 120KB | ~7.6 MB | 2 GB+ |
| Directories | No (flat) | Yes (hierarchical) | Yes |
| Permissions | No | Basic (mode bits) | Full POSIX |
| Timestamps | No | Created + modified | atime/mtime/ctime |
| Block allocation | Fixed slot | FAT chain | Bitmap + inode |
| Fragmentation | None (fixed) | Can fragment | Can fragment |
| Journaling | No | No | No (ext3 would) |

---

## 4. Feasibility Analysis: PS2 Cards on PS1

### 4.1 Direct Connection: NOT POSSIBLE

| Barrier | Severity | Details |
|---------|----------|---------|
| Physical connector | Hard | Different slot dimensions, keying |
| Protocol difference | Hard | SIO2 vs SIO0, different command set |
| MagicGate auth | Hard | PS1 has no MagicGate hardware |
| Clock speed | Medium | PS2 cards may require higher clock |
| Page size mismatch | Medium | 512 vs 128 bytes per transfer |

**Verdict:** PS2 memory cards **cannot** be used directly in PS1 memory card slots. The protocol, encryption, and physical form factor are all incompatible.

### 4.2 Adapter/Bridge: THEORETICALLY POSSIBLE but IMPRACTICAL

An adapter would need to:
1. Physically accept a PS2 card
2. Implement MagicGate authentication in hardware/firmware
3. Translate PS1 SIO0 protocol to PS2 SIO2 protocol
4. Present a PS1-compatible interface to the PS1

**Complexity:** This is essentially building a PS2 IOP controller in a small form factor. The MagicGate key requirement alone makes this extremely difficult — the encryption keys are proprietary.

**Cost/effort:** Far exceeds the benefit. Building such an adapter would require reverse-engineering MagicGate and implementing a complex protocol translator.

### 4.3 PicoMemcard Approach: THE PRACTICAL ALTERNATIVE

Instead of trying to use PS2 cards on PS1 hardware, use a microcontroller-based card emulator:

```
┌───────────────────────────────────────────────────────┐
│                PicoMemcard (RP2040)                    │
│                                                       │
│  PS1 Slot Interface ←──→ RP2040 MCU ←──→ SD Card     │
│  (SIO0, 250kHz)          (133MHz)        (SPI, 25MHz) │
│                                                       │
│  Emulates standard PS1 memory card protocol           │
│  Stores data on SD card (GB+ capacity)                │
│  Can present multiple virtual card images              │
│  Could present very large cards (beyond 128KB)         │
└───────────────────────────────────────────────────────┘
```

**Advantages:**
- Plugs into standard PS1 memory card slot
- Speaks native PS1 SIO0 protocol (no adapter needed)
- Storage capacity limited only by SD card (GB+)
- Could present 8MB+ of virtual storage per slot
- Open source firmware (modifiable for Blackroo Linux)

**Limitations:**
- Throughput still limited by SIO0 bus speed (~250kHz)
- Custom firmware needed for large card support
- ~$10-15 per unit (Pico board + adapter PCB)

---

## 5. PS2 Memory Card as Storage on PS2 (For Reference)

### 5.1 PS2 Linux and Memory Cards

The PS2 Linux kit (official Sony product, 2002) used a hard drive for storage, but the PS2 kernelloader project shows how memory cards can be used for boot:

```
PS2 kernelloader approach:
1. Store compressed kernel on PS2 memory card
2. Boot PS2 → memory card browser → FreeMcBoot exploit
3. FreeMcBoot loads kernelloader
4. Kernelloader reads kernel from memory card
5. Kernelloader configures hardware and jumps to kernel
6. Linux boots with initrd or HDD rootfs
```

**Relevance to Blackroo:** The PS2 kernelloader's architecture (exploit → bootloader → kernel load → Linux boot) is directly analogous to what we need for PS1:

```
PS1 Blackroo approach (inspired by PS2 kernelloader):
1. Store compressed kernel on PS1 memory card(s)
2. Boot PS1 → FreePSXBoot exploit (or UniROM cart)
3. FreePSXBoot loads Blackroo bootloader
4. Bootloader reads kernel from memory card RAID (or serial)
5. Bootloader configures RAM, copies kernel, jumps to entry
6. Linux boots with initrd
```

### 5.2 PS2 Memory Card I/O via IOP

On the PS2, memory card access goes through the IOP (I/O Processor, which is essentially a PS1 CPU):

```
EE (Emotion Engine) → SIF (Sub-cpu Interface) → IOP → SIO2 → Memory Card
```

The IOP has its own firmware (IRX modules) including:
- `mcman.irx` — Memory card manager
- `mcserv.irx` — Memory card server (handles EE requests)
- `sio2man.irx` — SIO2 controller driver

This multi-layered architecture is far more complex than the PS1's direct SIO0 access.

---

## 6. Recommendations for Blackroo Linux

### 6.1 Do NOT Pursue PS2 Card Compatibility

The effort required to use PS2 memory cards on PS1 hardware is prohibitive:
- MagicGate encryption requires proprietary keys
- Protocol translation is complex
- Physical adapter would cost more than alternatives
- No existing open-source implementations

### 6.2 DO Pursue These Alternatives

| Approach | Capacity | Effort | Cost |
|----------|----------|--------|------|
| **Multi-Tap + 8x PS1 cards** | 1 MB | Low (driver changes) | ~$30-50 |
| **PicoMemcard (standard)** | 128 KB per slot | None (works today) | ~$15/each |
| **PicoMemcard (custom firmware)** | MB+ per slot | Medium | ~$15/each |
| **PicoMemcard + Multi-Tap** | MB+ x 8 slots | Medium | ~$150 |

### 6.3 Future: Large Virtual Cards via PicoMemcard

The most promising path to large storage on PS1:

1. **Modify PicoMemcard firmware** to present cards larger than 128KB
2. **Extend PS1 SIO0 protocol** with custom commands for addressing beyond block 0x3FF
3. **Modify bu.c driver** to use extended addressing
4. **Result:** Each PicoMemcard slot provides megabytes of storage

**Challenge:** The PS1 SIO0 protocol uses 16-bit block addresses (max 65,536 blocks x 128 bytes = 8 MB per card). This is already supported by the bu.h `__u16 block` field. Extending to the full 16-bit address space would give 8MB per slot — matching PS2 card capacity without any PS2 hardware.

```c
/* Current bu.h block address */
typedef struct _bu_request_t {
    int   floor;     // channel number 0..f
    __u16 block;     // block number (16-bit = max 65535)
    int   card;
    __u8  buffer[BU_BLK_SIZE];
    int   mode;
} bu_request_t;

/* 16-bit block address allows:
   65536 blocks x 128 bytes = 8,388,608 bytes = 8 MB per card
   This matches PS2 memory card capacity! */
```

### 6.4 Filesystem Recommendation

Regardless of the storage hardware, use ext2 for the Linux filesystem:
- Native kernel support (already enabled)
- Well-tested, mature code
- Tunable block size (1KB minimum for small volumes)
- No journaling overhead (ext2, not ext3)
- mkfs.ext2 can create very small filesystems

**Do NOT use** the Sony PS1 or PS2 filesystem formats for Linux storage. These are proprietary formats designed for save games, not general-purpose file storage.

---

## 7. PS2 Memory Card Technical Appendix

### 7.1 MagicGate Authentication Flow

```
Step 1: Card Insert Detection
  Host: Send probe command (0x11)
  Card: Respond with card info

Step 2: Begin Authentication
  Host: Send auth command (0x12)
  Card: Send card nonce (8 bytes)

Step 3: Challenge-Response
  Host: Send host nonce (8 bytes) + encrypted challenge
  Card: Verify and send encrypted response

Step 4: Session Key Derivation
  Both: Derive shared session key from nonces + secrets
  Card: Enable read/write access

Total auth time: ~100-200ms
```

### 7.2 PS2 Card ECC (Error Correcting Code)

Each 512-byte page has 16 bytes of ECC data:
- Hamming code based
- Can detect 2-bit errors per 128-byte sub-page
- Can correct 1-bit errors per 128-byte sub-page
- 4 sub-pages per page x 4 bytes ECC per sub-page = 16 bytes

PS1 cards have no ECC — errors are detected only by checksum (XOR of all data bytes). This means PS1 cards are more vulnerable to bit rot than PS2 cards.

### 7.3 PS2 Card Erase Behavior

Unlike PS1 cards (which can write individual 128-byte blocks), PS2 cards require:
1. **Read** the entire 16KB erase block into RAM
2. **Erase** the 16KB block (all bits set to 1)
3. **Write** the modified data back (page by page)

This read-modify-write cycle is necessary because flash memory can only be erased in large blocks. PS1 cards use EEPROM technology, which supports byte-granularity writes without explicit erase.

---

*Blackroo Linux Technical White Paper Series*
*Document: BLKR-WP-002 — PS2 Memory Card Analysis*
