# Blackroo Linux - Memory Card Storage and RAID

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: UniROM as the upload path.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> PS1 memory card driver, SIO protocol, RAID concatenation, and filesystem design

---

## PS1 Memory Card Hardware

### Physical Specifications

| Parameter | Value |
|-----------|-------|
| Capacity | 128 KB (1 Mbit) per card |
| Technology | Serial EEPROM |
| Block size | 128 bytes (hardware minimum addressable unit) |
| Total blocks | 1,024 (0x000 - 0x3FF) |
| Total frames | 16,384 (64 frames per block, 8 KB per block) |
| Interface | Serial via SIO0 controller port |
| Clock speed | ~250 kHz |
| Supply voltage | 3.3V (from controller port) |
| Endurance | ~100,000 write cycles per sector |
| Data retention | ~10 years (manufacturer spec) |

### Standard PS1 Memory Card Layout

The standard Sony filesystem uses 16 blocks of 8KB each (128KB total):

```
Block 0:  Directory / System Block
  Frame 0:      Header (signature "MC", 0x4D43)
  Frames 1-15:  Directory entries (15 save slots)
  Frames 16-35: Broken sector list
  Frames 36-55: Broken sector replacement data
  Frames 56-62: Unused
  Frame 63:     Write test frame

Blocks 1-15: Data Blocks (15 blocks x 8KB = 120KB usable)
  Frame 0:      Title frame (Shift-JIS name, first block of save)
  Frames 1-3:   Icon data (first block of save)
  Frames 4-63:  User data
```

**For Blackroo Linux, we bypass the Sony filesystem entirely** and treat the card as a raw block device. The driver accesses the card at the 128-byte block level, not the 8KB frame level.

---

## SIO0 Protocol (Controller/Memory Card Bus)

### Hardware Interface

Memory cards share the controller port's SIO0 bus:

```
Controller Port Pinout:
  Pin 1: DATA (bidirectional, directly to SIO0_TX/RX_DATA)
  Pin 2: CMD  (directly to SIO0_TX/RX_DATA)
  Pin 3: +7.6V (motor power, unused for memory cards)
  Pin 4: GND
  Pin 5: +3.3V (card power)
  Pin 6: /CS (directly from SIO0 DTR output)
  Pin 7: CLOCK (from SIO0 baud generator)
  Pin 8: /ACK (directly to SIO0 DSR input, triggers IRQ7 / CONTROLLER)
  Pin 9: /ACK (directly to SIO0 DSR input)
```

### Device Addressing

| Address Byte | Device |
|-------------|--------|
| 0x01 | Controller (pad) in selected port |
| 0x81 | Memory card in selected port (direct slot) |
| 0x82 | Multi-tap slot A (if multi-tap present) |
| 0x83 | Multi-tap slot B |
| 0x84 | Multi-tap slot C |
| 0x85 | Multi-tap slot D |

Port selection is controlled by SIO0_CTRL bit 13:
- Bit 13 = 0: Port 1 (slot 1)
- Bit 13 = 1: Port 2 (slot 2)

### Read Command (0x52 = 'R')

```
Step  TX (host sends)      RX (card returns)    Notes
────  ──────────────────   ──────────────────   ──────────────────
  0   0x81                 0xFF                 Address byte
  1   0x52 ('R')           0xFF                 Read command
  2   0x00                 0x5A                 Card ID byte 1
  3   0x00                 0x5D                 Card ID byte 2
  4   MSB (block addr)     0x00                 Block address high
  5   LSB (block addr)     0x00                 Block address low
  6   0x00                 0x5C                 Command ACK byte 1
  7   0x00                 0x5D                 Command ACK byte 2
  8   0x00                 MSB (echo)           Block addr high echo
  9   0x00                 LSB (echo)           Block addr low echo
10-137 0x00 x128           DATA x128            128 bytes of data
138  0x00                  CHECKSUM             XOR of MSB+LSB+DATA
139  0x00                  0x47 ('G')           Good / end of data
```

**Checksum:** XOR of block address MSB, LSB, and all 128 data bytes.

### Write Command (0x57 = 'W')

```
Step  TX (host sends)      RX (card returns)    Notes
────  ──────────────────   ──────────────────   ──────────────────
  0   0x81                 0xFF                 Address byte
  1   0x57 ('W')           0xFF                 Write command
  2   0x00                 0x5A                 Card ID byte 1
  3   0x00                 0x5D                 Card ID byte 2
  4   MSB (block addr)     0x00                 Block address high
  5   LSB (block addr)     0x00                 Block address low
 6-133 DATA x128           0x00                 128 bytes of data
134  CHECKSUM              0x00                 XOR checksum
135  0x00                  0x5C                 ACK byte 1
136  0x00                  0x5D                 ACK byte 2
137  0x00                  STATUS               47=Good, 4E=Bad CRC, FF=Bad sector
```

### Get ID Command (0x53 = 'S')

```
Step  TX (host sends)      RX (card returns)
────  ──────────────────   ──────────────────
  0   0x81                 0xFF
  1   0x53 ('S')           0xFF
  2   0x00                 0x5A
  3   0x00                 0x5D
  4   0x00                 0x5C
  5   0x00                 0x5D
  6   0x00                 0x04 (block MSB)
  7   0x00                 0x00 (block LSB)
  8   0x00                 0x00 (block size MSB)
  9   0x00                 0x80 (block size LSB = 128)
 10   0x00                 0x00 (sector count MSB)
 11   0x00                 0x04 (sector count LSB = 1024)
```

### Transfer Speed Analysis

```
Clock: ~250 kHz
Bits per byte: 8 (+ start/stop/parity overhead ≈ 10-11 bits effective)
Effective byte rate: ~22-25 KB/s raw

Per 128-byte block read:
  Protocol overhead: ~12 bytes (address, command, ACK, checksum)
  Data: 128 bytes
  Total: ~140 bytes
  Time per block: ~5.6ms

Per 128-byte block write:
  Protocol overhead: ~12 bytes
  Data: 128 bytes
  Write time: additional ~2-5ms (EEPROM program time)
  Total: ~140 bytes + write delay
  Time per block: ~8-10ms

Throughput:
  Sequential read:  ~22 KB/s (128 bytes / 5.6ms)
  Sequential write: ~13-16 KB/s (128 bytes / 8-10ms)

Full card read (1024 blocks): ~5.7 seconds
Full card write (1024 blocks): ~8-10 seconds
```

---

## Current Driver Analysis (bu.c)

### Architecture

The memory card driver (`drivers/block/bu.c`) is a Linux block device driver using interrupt-driven SIO0 communication:

```
Block Device Layer (VFS)
    │
    ▼
do_bu_request()          ← Request queue handler
    │
    ▼
bu_sw_init() + bu_hw_init()  ← Initialize transfer state machine
    │
    ▼
bu_rd_routine()          ← State machine step (called per interrupt)
    │
    ▼
bu_interrupt()           ← IRQ handler (CONTROLLER interrupt)
    │                        Called when card sends /ACK signal
    ▼
bu_rd_states[]           ← State function table (13 states for read,
                            11 states for write)
```

### State Machine

The driver uses a state machine with function pointers:

**Read states (0-12):**
```
State 0:  Select port + send address byte (0x81+floor)
State 1:  Send command byte ('R' = 0x52)
State 2:  Receive first ID byte, send 0x00
State 3:  Verify second ID byte (0x5A)
State 4:  Verify third byte (0x5D), send block addr MSB
State 5:  Send block addr LSB
State 6:  Receive echo of block addr MSB
State 7:  Verify ACK byte 1 (0x5C)
State 8:  Verify ACK byte 2 (0x5D)
State 9:  Verify block addr MSB echo
State 10: Verify block addr LSB echo
State 11: Read 128 data bytes (bu_rd_data, loops 128 times)
State 12: Verify checksum + status byte (0x47 = Good)
```

**Write states (0x10-0x1A):**
```
State 0x10: Select port + send address byte
State 0x11: Send command byte ('W' = 0x57)
State 0x12-0x15: Same ID verification as read
State 0x16: Write 128 data bytes (bu_wr_data, loops 128 times)
State 0x17: Send checksum
State 0x18: Receive echo/verify
State 0x19: Verify ACK
State 0x1A: Verify status (0x47 = Good)
```

### Card Selection (Current — 2 cards only)

```c
// bu.c line 232: Port selection
outw((((bu->bu_request->card) & 1) << 13) | 0x1003, BU_CONTROL);
//     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//     card & 1: only bit 0 matters = 2 cards max
//     << 13: shifts to SIO0_CTRL bit 13 (port select)

// bu.c line 234: Device address
outb(0x81 + bu->bu_request->floor, BU_DATA);
//   floor is always 0 in current code = direct slot only
```

### LARGE_CARD Mode (Existing RAID Support)

The driver already has `CONFIG_PSX_LARGE_CARD` support that joins multiple cards into a single logical device:

```c
// In do_bu_request() when CONFIG_PSX_LARGE_CARD is set:
for (j = 0; j < BU_MINORS; j++) {
    if (bu_devices[j].first_block.size > 0) {
        bu_curr_request.block += BU_FIRST_BLOCKS;
        if (bu_curr_request.block < bu_devices[j].first_block.size) break;
        bu_curr_request.block -= bu_devices[j].first_block.size;
    }
}
bu_curr_request.card = j;  // Routes to correct physical card
```

This implements **linear concatenation** (JBOD, not striping):
- Blocks 0-1023: Card 0
- Blocks 1024-2047: Card 1
- Blocks 2048-3071: Card 2
- etc.

### Card Identification (First Block)

Each card in a RAID set must have a header in block 0:

```c
typedef struct {
    __u32 id;       // Must be 0x1234 (BU_ID)
    __u32 size;     // Card size in 128-byte blocks
    __u32 serial;   // Card serial number (unique per card)
    __u32 number;   // Card sequence number (0, 1, 2, ...) for ordering
} bu_first_block_t;
```

The driver verifies:
1. `id == 0x1234` (valid Blackroo card)
2. `number` matches expected sequence (0, 1, 2, ...)
3. Cards must be in consecutive slots

---

## Multi-Card RAID Expansion

### Configuration Options

| Config | Cards | Slots | Total Storage | Notes |
|--------|-------|-------|---------------|-------|
| Stock, no RAID | 2 | Direct only | 256 KB (2x 128KB) | Current driver supports this |
| LARGE_CARD, 2 slots | 2 | Direct only | 256 KB joined | Existing code, tested |
| Single multi-tap | 5 | 1 port multi-tap + 1 direct | 640 KB | Needs multi-tap addressing |
| Dual multi-tap | 8 | 2 port multi-taps | 1,024 KB (1 MB) | Needs full expansion |

### Required Driver Modifications

#### 1. Expand BU_MINORS (bu.h)

```c
#ifdef CONFIG_PSX_MULTITAP
#define BU_MINORS (8)    /* 4 per port x 2 ports */
#else
#define BU_MINORS (2)    /* Direct slots only */
#endif
```

#### 2. Card-to-Port Mapping (bu.c)

```c
/*
 * Card mapping for multi-tap configuration:
 *
 * Card 0: Port 1, Multi-tap slot A (address 0x81)
 * Card 1: Port 1, Multi-tap slot B (address 0x82)
 * Card 2: Port 1, Multi-tap slot C (address 0x83)
 * Card 3: Port 1, Multi-tap slot D (address 0x84)
 * Card 4: Port 2, Multi-tap slot A (address 0x81)
 * Card 5: Port 2, Multi-tap slot B (address 0x82)
 * Card 6: Port 2, Multi-tap slot C (address 0x83)
 * Card 7: Port 2, Multi-tap slot D (address 0x84)
 *
 * Without multi-tap:
 * Card 0: Port 1, Direct slot (address 0x81)
 * Card 1: Port 2, Direct slot (address 0x81)
 */

static inline int bu_card_to_port(int card) {
    return (card >= 4) ? 1 : 0;
}

static inline int bu_card_to_address(int card) {
    return 0x81 + (card % 4);
}
```

#### 3. Modified State 0 (Port + Address Selection)

```c
static int bu_rd_state0(bu_t *bu) {
    int port = bu_card_to_port(bu->bu_request->card);
    int addr = bu_card_to_address(bu->bu_request->card);

    /* Select port via bit 13 of SIO0_CTRL */
    outw((port << 13) | 0x1003, BU_CONTROL);
    bu->byte = inb(BU_DATA);

    /* Send device address (0x81-0x84 for multi-tap slots) */
    outb(addr, BU_DATA);

    return 1;
}
```

---

## Filesystem Design for Memory Cards

### Option 1: ext2 (Current Choice)

**Pros:** Native Linux support, well-tested, writable
**Cons:** Minimum practical size ~128KB, significant metadata overhead at small sizes

```
For a 1MB RAID (8 cards):
  Superblock + group descriptors: ~2 KB
  Block bitmap: ~1 KB
  Inode bitmap: ~1 KB
  Inode table: ~16 KB (128 inodes)
  Usable data: ~980 KB

Block size: 1024 bytes (ext2 minimum)
  Blocks per card read: 8 reads per ext2 block (128 bytes per read)
  Time per ext2 block: ~45ms

For a single 128KB card:
  Overhead: ~20 KB
  Usable: ~100 KB (78% efficiency)
```

### Option 2: ROMFS (Read-Only)

**Pros:** Very compact, low overhead, fast mount
**Cons:** Read-only, can't modify on-device

Good for initial rootfs that never changes.

### Option 3: Custom Blackroo FS (Proposed)

A minimal filesystem optimized for 128-byte blocks:

```
Block 0: Superblock
  bytes 0-3:   Magic (0x424C4B52 = "BLKR")
  bytes 4-7:   Total blocks
  bytes 8-11:  Free blocks
  bytes 12-15: Root inode block
  bytes 16-19: Block bitmap start
  bytes 20-127: Reserved

Block 1-N: Block bitmap (1 bit per block)
  For 1024 blocks: 128 bytes = 1 block

Block N+1...: Inode table
  Each inode: 32 bytes (4 per block)
    bytes 0-1:   Mode (permissions, type)
    bytes 2-3:   Size (high 16 bits)
    bytes 4-7:   Size (low 32 bits) — max 4GB
    bytes 8-9:   Link count
    bytes 10-11: Block count
    bytes 12-15: Direct block pointer 0
    bytes 16-19: Direct block pointer 1
    bytes 20-23: Direct block pointer 2
    bytes 24-27: Direct block pointer 3
    bytes 28-31: Indirect block pointer

Data blocks: File/directory data

Directory entry: Variable length
    bytes 0-3:   Inode number
    byte 4:      Name length
    bytes 5-N:   Name (no null terminator needed)
```

**Advantages over ext2:**
- Native 128-byte block alignment (no wasted reads)
- Minimal metadata overhead
- Designed for extreme space constraints

**Disadvantage:** Would need a new filesystem driver in the kernel.

### Recommended Approach: ext2 with Hybrid Boot

1. Boot from initrd in RAM (contains kernel + BusyBox + init)
2. Mount memory card RAID as ext2 on `/mnt/storage`
3. Use memory cards for persistent data only (configs, scripts, user data)
4. Keep rootfs read-only in RAM

---

## Memory Card Formatting Tool

A PSX-side tool (or host-side tool) to prepare memory cards for Blackroo:

### Host-Side Format (via serial + UniROM)

```bash
#!/bin/bash
# format_memcard.sh - Prepare memory cards for Blackroo Linux
# Requires: nops (serial tool), card in PS1 slot

CARD_NUM=$1     # 0-7
TOTAL_CARDS=$2  # Total cards in RAID set

# Generate header block (128 bytes)
python3 -c "
import struct
header = struct.pack('<III I',
    0x1234,           # id = BU_ID
    1024,             # size = 1024 blocks
    0x${RANDOM},      # serial = unique
    ${CARD_NUM}       # number = sequence position
)
header += b'\x00' * (128 - len(header))
print(header.hex())
"

# Upload and write via serial protocol
# (requires custom tool or modification to nops)
```

### PSX-Side Format Tool (PSn00bSDK)

A PS1 application that:
1. Detects all connected memory cards (direct + multi-tap)
2. Writes Blackroo headers (block 0) with correct sequence numbers
3. Optionally writes an ext2 filesystem across the RAID set
4. Verifies read-back of all blocks

---

## Performance Estimates

### Sequential Operations

| Operation | Per Block | Per KB | Per 128KB Card | Per 1MB RAID |
|-----------|-----------|--------|----------------|--------------|
| Read | ~5.6ms | ~44ms | ~5.7s | ~45s |
| Write | ~8ms | ~64ms | ~8.2s | ~65s |

### Random Access

Due to the sequential SIO protocol, there is no seek time — every access requires sending the full address. Random access performance equals sequential performance per-block.

### Practical Impact

| Use Case | Time | Notes |
|----------|------|-------|
| Mount ext2 (read superblock + group desc) | ~100ms | Fast — just a few blocks |
| Read a 4KB file | ~180ms | 32 blocks x 5.6ms |
| Write a 4KB file | ~256ms | 32 blocks x 8ms |
| List directory (small) | ~50ms | Read 1-2 blocks |
| Read entire 128KB card | ~5.7s | 1024 blocks |
| Format 1MB RAID | ~65s | Write all 8192 blocks |
| Boot rootfs from cards | ~10-30s | Depends on files accessed |

---

## Write Wear Management

Memory cards have ~100,000 write cycle endurance per sector. For a root filesystem:

- **Avoid:** Frequent writes to the same blocks (logs, temp files)
- **Use RAM:** Keep /tmp, /var/log, /var/run in ramdisk (tmpfs)
- **Mount read-only:** Mount card filesystem as read-only where possible
- **Batch writes:** Buffer writes in RAM, flush periodically
- **Wear leveling:** Not available in hardware — would need software implementation

### Estimated Lifetime

| Usage Pattern | Writes/Day | Lifetime (per sector) |
|---------------|------------|----------------------|
| Read-only mount | 0 | Infinite |
| Light writes (configs) | ~10 | 27+ years |
| Moderate writes | ~100 | 2.7 years |
| Heavy writes (logging) | ~1000 | 100 days |
| Swap usage | ~10,000+ | ~10 days |

**Recommendation:** Never use memory cards for swap in production. Use only for persistent config/data storage.

---

*Blackroo Linux Memory Card Storage Documentation*

---

## Addendum 2026-08-22 — how big can a Pico-backed "card" be?

### The protocol ceiling is 8 MB, not 128 KB

The memory card read/write commands carry the frame number as **two bytes, MSB
then LSB** — our own driver already emits both (`bu.c:374`, checksummed over both
at `bu.c:308`). That is a **16-bit frame address**:

```
65,536 frames x 128 bytes = 8,388,608 bytes = 8 MB
```

`BU_BSIZE (1024)` in `bu.h` is the constant for a *real* 128 KB card, not a limit
imposed by the bus. A Pico emulating a card need only accept frame addresses above
1023 and return data; raise `BU_BSIZE` on the Linux side and the existing driver
addresses the lot with **no protocol change at all**.

Beyond 8 MB requires a protocol extension — a bank-select or vendor command
carrying a third address byte. Since we own both ends (Pico firmware and `bu.c`),
that is ours to define, and an SD card behind it makes capacity effectively
unlimited.

### But capacity is not the problem — throughput is

Measured SIO0 throughput is **~22 KB/s** (`docs/11`). So:

| | Time to read |
|---|---|
| 128 KB (a real card) | ~6 seconds |
| 8 MB (protocol maximum) | **~6.4 minutes** |
| A ~800 KB busybox | **~36 seconds** |

### Why this is a poor root filesystem

Blackroo is **nommu**, so there is no demand paging — an executable must be copied
into RAM in full before it runs. Every `exec` therefore pays the full transfer
cost. Loading busybox off a card-backed drive would take the better part of a
minute, every time, on a console with 2 MB of RAM.

**Striping across both card slots does not help.** The two slots share the SIO0
clock and data lines and differ only in select line, so transfers serialise. RAID
across slots buys capacity, never bandwidth.

### The right split

| Role | Device | Why |
|---|---|---|
| **Root filesystem** | **PIO flash, XIP** | Memory-mapped at `0x1F000000`, executes in place, costs **zero RAM** and has no load time. Up to 8 MB window (`docs/21`). |
| **Bulk storage** — `/home`, data, media, captures | **Pico + SD in the card slot** | Capacity effectively unlimited; ~22 KB/s is fine for files you read once. |

Use both together. The card slot is the wrong place for anything you execute and
the right place for everything you merely read.

Cross-reference: `docs/21-PIO-PORT-REFERENCE.md`, `docs/11-PICOMEMCARD-DUAL-MODE.md`.
