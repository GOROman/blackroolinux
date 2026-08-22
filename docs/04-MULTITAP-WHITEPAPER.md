# Technical White Paper: Multi-Tap Memory Card Expansion for Blackroo Linux

> Expanding PlayStation 1 Storage from 256KB to 1MB Using Multi-Tap Adapters

---

## Abstract

The Sony PlayStation 1 provides two controller/memory card ports, each supporting one memory card (128KB), for a total of 256KB of external storage. By leveraging the PlayStation Multi-Tap adapter (SCPH-1070), each port can be expanded to support four devices, enabling up to eight memory cards (1MB total) to be connected simultaneously. This paper documents the hardware interface, SIO protocol extensions, driver modifications required for Linux kernel support, and the practical considerations for using multi-tap expanded storage as a root filesystem for Blackroo Linux.

---

## 1. Introduction

### 1.1 Problem Statement

Running Linux on the PlayStation 1 requires persistent storage for a root filesystem. The stock PlayStation provides only two memory card slots with 128KB each (256KB total). This is insufficient for even a minimal Linux filesystem, which requires at minimum 200-500KB for BusyBox and essential system files.

### 1.2 Proposed Solution

The PlayStation Multi-Tap (SCPH-1070, and compatible third-party units) multiplexes a single controller port into four sub-ports. By connecting a Multi-Tap adapter to each of the two controller ports, up to eight memory cards can be connected simultaneously, providing 1MB of addressable storage — sufficient for a minimal ext2 root filesystem.

### 1.3 Scope

This paper covers:
- Multi-Tap hardware architecture and signaling
- SIO0 protocol extensions for multi-tap addressing
- Linux kernel driver modifications (bu.c / bu.h)
- Storage topology and RAID configuration
- Performance analysis and limitations
- Alternative solutions (PicoMemcard, PSIO)

---

## 2. Multi-Tap Hardware Architecture

### 2.1 Sony SCPH-1070 Multi-Tap

The official PlayStation Multi-Tap is a passive multiplexer that:

- Plugs into one controller port
- Provides four controller/memory card sub-ports (labeled A, B, C, D)
- Uses address-based device selection (no additional control lines)
- Is electrically transparent — the SIO0 bus signals pass through directly
- Requires no special initialization sequence
- Is fully backward-compatible (port A acts as the direct slot)

```
                    ┌──────────────────────────────┐
                    │      Multi-Tap (SCPH-1070)    │
                    │                               │
PS1 Port 1 ────────┤  ┌─── Port A (address 0x01/0x81)  ──── Controller/MemCard
(SIO0, DTR0)       │  ├─── Port B (address 0x02/0x82)  ──── Controller/MemCard
                    │  ├─── Port C (address 0x03/0x83)  ──── Controller/MemCard
                    │  └─── Port D (address 0x04/0x84)  ──── Controller/MemCard
                    │                               │
                    └──────────────────────────────┘
```

### 2.2 Addressing Scheme

The Multi-Tap routes commands based on the **first byte** sent on the SIO0 bus (the address/device select byte):

| Address Byte | Device Type | Sub-Port |
|-------------|-------------|----------|
| 0x01 | Controller | Port A (= direct slot) |
| 0x02 | Controller | Port B |
| 0x03 | Controller | Port C |
| 0x04 | Controller | Port D |
| 0x81 | Memory Card | Port A (= direct slot) |
| 0x82 | Memory Card | Port B |
| 0x83 | Memory Card | Port C |
| 0x84 | Memory Card | Port D |

**Key observation:** Without a Multi-Tap, only addresses 0x01 and 0x81 are valid on a given port. The Multi-Tap intercepts the address byte and routes subsequent communication to the correct sub-port.

### 2.3 Electrical Characteristics

The Multi-Tap shares the single SIO0 data bus across all four sub-ports:

- **CLK (clock):** Directly shared — all devices see the clock
- **CMD (command/TX):** Directly shared — all devices see commands
- **DATA (response/RX):** Only the selected device drives this line
- **/CS (chip select):** The Multi-Tap generates individual /CS signals based on the address byte
- **/ACK:** Only the selected device sends /ACK pulses

**Bus contention prevention:** The Multi-Tap tristates (disconnects) non-selected devices from the DATA line. Only the addressed device is allowed to respond.

### 2.4 Dual Multi-Tap Configuration

```
┌──────────────┐     ┌──────────────────────┐
│ PlayStation 1 │     │ Multi-Tap 1 (Port 1) │
│               │     │  ├── Slot A: Card 0  │  128 KB
│  Port 1 ──────┼─────┤  ├── Slot B: Card 1  │  128 KB
│  (DTR0)       │     │  ├── Slot C: Card 2  │  128 KB
│               │     │  └── Slot D: Card 3  │  128 KB
│               │     └──────────────────────┘
│               │     ┌──────────────────────┐
│               │     │ Multi-Tap 2 (Port 2) │
│  Port 2 ──────┼─────┤  ├── Slot A: Card 4  │  128 KB
│  (DTR1)       │     │  ├── Slot B: Card 5  │  128 KB
│               │     │  ├── Slot C: Card 6  │  128 KB
│               │     │  └── Slot D: Card 7  │  128 KB
│               │     └──────────────────────┘
└──────────────┘
                      Total: 8 x 128 KB = 1,024 KB (1 MB)
```

---

## 3. SIO0 Protocol for Multi-Tap

### 3.1 Standard (No Multi-Tap) Communication

```
Host → SIO0:  [0x81] [CMD] [params...]
Card → SIO0:  [0xFF] [ID]  [response...]

Port selection: SIO0_CTRL bit 13
  0 = Port 1 (DTR0 active)
  1 = Port 2 (DTR1 active)
```

### 3.2 Multi-Tap Communication

The protocol is identical except for the address byte:

```
Host → SIO0:  [0x81+N] [CMD] [params...]    where N = 0,1,2,3
Card → SIO0:  [0xFF]   [ID]  [response...]

For memory card in Multi-Tap slot B of Port 1:
  SIO0_CTRL bit 13 = 0 (Port 1)
  Address byte = 0x82 (slot B)

For memory card in Multi-Tap slot C of Port 2:
  SIO0_CTRL bit 13 = 1 (Port 2)
  Address byte = 0x83 (slot C)
```

### 3.3 Multi-Tap Detection

To detect if a Multi-Tap is connected (vs. a direct controller/card):

1. Send address byte 0x01 to the port
2. If response indicates "Multi-Tap" device ID (0x5A80), a multi-tap is present
3. Alternatively: Attempt to communicate with address 0x82 — if /ACK is received, a Multi-Tap is present

**For Blackroo Linux:** Auto-detection is optional. The driver can simply try all 8 slots during initialization and mark absent cards as empty.

### 3.4 Multi-Tap Timing

The Multi-Tap adds minimal latency:
- Address byte routing: ~1 byte time (~40us at 250kHz)
- /CS propagation: ~1us
- Total added overhead: <50us per transaction (negligible)

---

## 4. Kernel Driver Modifications

### 4.1 Header Changes (bu.h)

```c
/* ===== Current ===== */
#define BU_MINORS (2)

/* ===== Modified ===== */
#ifdef CONFIG_PSX_MULTITAP
#define BU_MINORS       (8)     /* 4 per port x 2 ports */
#define BU_PORTS        (2)     /* Number of controller ports */
#define BU_SLOTS_PER_PORT (4)   /* Multi-Tap slots per port */
#else
#define BU_MINORS       (2)     /* Direct slots only */
#define BU_PORTS        (2)
#define BU_SLOTS_PER_PORT (1)
#endif

/* Multi-Tap slot address calculation */
#define BU_SLOT_ADDR(slot)  (0x81 + ((slot) % BU_SLOTS_PER_PORT))
#define BU_SLOT_PORT(slot)  ((slot) / BU_SLOTS_PER_PORT)
```

### 4.2 Port/Address Selection (bu.c)

Replace the current card selection in `bu_rd_state0()`:

```c
/* ===== Current (line 230-234) ===== */
static int bu_rd_state0(bu_t *bu) {
    outw((((bu->bu_request->card) & 1) << 13) | 0x1003, BU_CONTROL);
    bu->byte = inb(BU_DATA);
    outb(0x81 + bu->bu_request->floor, BU_DATA);
    return 1;
}

/* ===== Modified ===== */
static int bu_rd_state0(bu_t *bu) {
    int card = bu->bu_request->card;
    int port = BU_SLOT_PORT(card);     /* 0 or 1 */
    int addr = BU_SLOT_ADDR(card);     /* 0x81 - 0x84 */

    /* Select port via bit 13 of SIO0_CTRL */
    outw((port << 13) | 0x1003, BU_CONTROL);
    bu->byte = inb(BU_DATA);

    /* Send multi-tap slot address */
    outb(addr, BU_DATA);

    return 1;
}
```

### 4.3 rd_routine Port Tracking

Also update `bu_rd_routine()` which also sets the port:

```c
/* ===== Current (line 377) ===== */
outw(inw(BU_CONTROL) | 0x13 | (((bu->bu_request->card) & 1) << 13), BU_CONTROL);

/* ===== Modified ===== */
int port = BU_SLOT_PORT(bu->bu_request->card);
outw(inw(BU_CONTROL) | 0x13 | (port << 13), BU_CONTROL);
```

### 4.4 Initialization Scan

Modify `bu_init()` to scan all 8 slots:

```c
/* The existing loop already iterates BU_MINORS times */
for (i = 0, bu_total = 0, n = 0; i < BU_MINORS; i++) {
    printk(KERN_INFO DEVICE_NAME ": detecting card in slot %d "
           "(port %d, tap %d)...\n",
           i + 1, BU_SLOT_PORT(i) + 1, (i % BU_SLOTS_PER_PORT) + 1);
    /* ... rest of detection code unchanged ... */
}
```

### 4.5 Kernel Config Addition

Add to `blackroo/Config` (or config.in for menuconfig):

```
# Multi-tap memory card support
CONFIG_PSX_MULTITAP=y     # Enable multi-tap (8 card slots)
# CONFIG_PSX_MULTITAP is not set   # Disable (2 card slots only)
```

---

## 5. Storage Topology

### 5.1 Linear Concatenation (JBOD)

The existing `CONFIG_PSX_LARGE_CARD` mode joins cards linearly:

```
Logical Block    Physical Location
─────────────    ──────────────────
0-1015           Card 0 (blocks 8-1023, first 8 reserved)
1016-2031        Card 1
2032-3047        Card 2
3048-4063        Card 3
4064-5079        Card 4
5080-6095        Card 5
6096-7111        Card 6
7112-8127        Card 7
                 ──────
                 Total: 8128 blocks x 128 bytes = ~1,016 KB usable
                 (8 blocks per card reserved for Blackroo header)
```

### 5.2 Device Nodes

```
/dev/bu0  - Card in Port 1, Slot A (or direct)
/dev/bu1  - Card in Port 1, Slot B (multi-tap)
/dev/bu2  - Card in Port 1, Slot C (multi-tap)
/dev/bu3  - Card in Port 1, Slot D (multi-tap)
/dev/bu4  - Card in Port 2, Slot A (or direct)
/dev/bu5  - Card in Port 2, Slot B (multi-tap)
/dev/bu6  - Card in Port 2, Slot C (multi-tap)
/dev/bu7  - Card in Port 2, Slot D (multi-tap)

/dev/bul  - Joined large device (all cards concatenated)
```

### 5.3 Filesystem Layout on 1MB RAID

```
Partition Map (1MB across 8 cards):
  Block 0:        Superblock
  Blocks 1-2:     Group descriptors
  Blocks 3-4:     Block bitmap (covers 8192 blocks)
  Blocks 5-6:     Inode bitmap
  Blocks 7-38:    Inode table (256 inodes x 128 bytes)
  Blocks 39-8127: Data blocks (~1,003 KB usable)

With ext2 (1KB block size):
  Each ext2 block = 8 memory card blocks (8 x 128 = 1024)
  Total ext2 blocks: ~1016
  Overhead: ~40 blocks (~5 KB)
  Usable: ~976 ext2 blocks (~976 KB)
```

---

## 6. Performance Analysis

### 6.1 Multi-Tap Overhead

The Multi-Tap adds negligible overhead:

| Metric | Without Multi-Tap | With Multi-Tap | Difference |
|--------|-------------------|----------------|------------|
| Address byte routing | 0us | ~40us | +40us |
| /CS propagation | Direct | ~1us relay | +1us |
| Per-block read total | 5.6ms | ~5.64ms | +0.7% |
| Full card read | 5.7s | ~5.74s | Negligible |

### 6.2 Multi-Card Operations

Since SIO0 is a shared serial bus, only **one card can be accessed at a time**. There is no parallel access.

| Operation | 1 Card | 2 Cards | 4 Cards | 8 Cards |
|-----------|--------|---------|---------|---------|
| Read all | 5.7s | 11.4s | 22.8s | 45.6s |
| Write all | 8.2s | 16.4s | 32.8s | 65.6s |
| Read 4KB file | 0.18s | 0.18s | 0.18s | 0.18s |
| Mount ext2 | ~0.2s | ~0.2s | ~0.2s | ~0.2s |

**Key insight:** For typical filesystem operations (reading individual files, directory listings), performance is dominated by the number of blocks accessed, not the number of cards. The RAID is transparent to the filesystem — it just provides more addressable blocks.

### 6.3 Bus Contention with Controllers

If a game controller is also plugged into a Multi-Tap port, the kernel must time-share SIO0 between controller polling and memory card access. This could cause:

- Dropped controller inputs during card reads/writes
- Card timeout errors during rapid controller polling

**Recommendation:** Dedicate both ports to memory cards for Blackroo Linux. No game controllers needed when running Linux.

---

## 7. Practical Considerations

### 7.1 Multi-Tap Availability

| Adapter | Compatibility | Notes |
|---------|--------------|-------|
| Sony SCPH-1070 | Excellent | Official, well-tested |
| Sony SCPH-1096 | Excellent | Later revision |
| Third-party generic | Good | Most work identically |
| Third-party with turbo buttons | Variable | Some modify addressing |

### 7.2 Physical Setup

The dual multi-tap setup requires:
- 2x Multi-Tap adapters
- 8x PlayStation memory cards
- PS1 console with controller ports accessible
- Sufficient desk space (multi-taps + trailing cables)

### 7.3 Card Compatibility

| Card Type | Block Size | Capacity | Compatible |
|-----------|-----------|----------|------------|
| Standard PS1 (Sony) | 128 bytes | 128 KB | Yes |
| Third-party PS1 | 128 bytes | 128 KB | Usually yes |
| PS1 mega cards (multi-page) | 128 bytes | 256KB-1MB | Needs investigation |
| PS2 memory card (8MB) | 512 bytes | 8 MB | **No** (different protocol, see doc 05) |
| PicoMemcard (RP2040) | 128 bytes | Configurable | Yes (emulates standard card) |

### 7.4 Error Handling

Multi-tap introduces additional failure modes:
- Multi-tap not detected (no response from slots B-D)
- Card removed mid-transaction (cable pull on multi-tap)
- Power supply droop (8 cards drawing from single port)

**Recommended approach:**
1. Timeout-based detection (already in bu.c)
2. Retry with exponential backoff
3. Mark failed cards as offline (remove from RAID)
4. Graceful degradation: continue with available cards

### 7.5 Power Considerations

Each controller port supplies 3.3V at limited current. Eight memory cards drawing power simultaneously:

- Per card current: ~20-50mA (read), ~80-100mA (write peak)
- 8 cards total: ~160-800mA
- Port supply rating: ~500mA per port (estimated)
- Two ports total: ~1A available

**Risk:** During simultaneous write operations across many cards, current draw may approach or exceed port limits. The linear RAID design mitigates this — only one card is ever accessed at a time.

---

## 8. Alternative Expansion Methods

### 8.1 PicoMemcard (RP2040-Based Memory Card Emulator)

[PicoMemcard](https://github.com/dangiu/PicoMemcard) uses a Raspberry Pi Pico (RP2040) to emulate a PlayStation memory card with storage backed by SD card or flash.

| Feature | Standard Card | PicoMemcard |
|---------|--------------|-------------|
| Capacity | 128 KB | Configurable (up to SD card size) |
| Speed | 250 kHz SIO | 250 kHz SIO (same bus) |
| Wear | ~100K writes | SD card (much higher) |
| Multi-image | No | Yes (switch between card images) |
| Cost | ~$5-10 used | ~$10-15 (Pico + adapter PCB) |

**For Blackroo Linux:** A PicoMemcard could present a much larger virtual memory card, but the SIO0 bus speed remains the bottleneck. The effective throughput is identical to a standard card. The advantage is capacity (megabytes instead of 128KB) and wear leveling (SD card).

### 8.2 PSIO (Optical Drive Replacement)

PSIO replaces the CD-ROM drive with an SD card reader. While primarily for game ISOs, it could theoretically be used for Linux storage if a driver were written.

**Advantages:** High capacity, faster than memory cards
**Disadvantages:** Expensive, requires driver development, replaces CD drive

### 8.3 Expansion Port (PIO) Devices

The Expansion Region 1 (0x1F000000, 8MB address space) is accessible via the parallel I/O port on the back of the PS1.

**Potential:** A custom device on this bus could provide fast, high-capacity storage. However:
- No standard devices exist for this port
- Would require custom hardware + driver development
- Only available on early PS1 models (PU-7 through PU-18)

---

## 9. Conclusion

Multi-Tap memory card expansion is the most practical approach to increasing PlayStation 1 storage for Linux:

1. **Hardware is cheap and widely available** (standard Multi-Tap + standard memory cards)
2. **Protocol is simple** — only the address byte changes (0x81 -> 0x82/83/84)
3. **Driver changes are minimal** — primarily expanding BU_MINORS and adding port/address mapping
4. **1MB total storage is achievable** — sufficient for a minimal ext2 root filesystem
5. **No hardware modifications required** — works on any PS1 model

The primary limitation is speed (~22KB/s read), making this suitable for persistent storage (configs, user data) rather than active working storage. The recommended architecture combines:
- **RAM-based initrd** for boot and system files (fast, read-only)
- **Memory card RAID** for persistent writable storage (slow, writable)

---

## References

1. psx-spx: Controllers and Memory Cards — https://psx-spx.consoledev.net/controllersandmemorycards/
2. psx-spx: SIO0 Registers — https://psx-spx.consoledev.net/serialinterfacessio/
3. PlayStation Multi-Tap SCPH-1070 — https://en.wikipedia.org/wiki/PlayStation_Multitap
4. PicoMemcard Project — https://github.com/dangiu/PicoMemcard
5. Blackroo Linux Memory Card Driver Source — `blackroo/drivers/block/bu.c`

---

*Blackroo Linux Technical White Paper Series*
*Document: BLKR-WP-001 — Multi-Tap Memory Card Expansion*
