# Blackroo Linux - Memory Subsystem

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: BINFMT_FLAT, the 8 MB mod, the FPU emulator.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> RAM configuration, auto-detection, and memory budgets for 2MB/4MB/8MB

---

## RAM_SIZE Register (0x1F801060)

This is the single most important register for memory expansion. The BIOS sets it during boot, and the kernel must re-configure it to match the actual installed RAM.

### Register Layout

```
Bit(s)  | Name              | Description
--------|-------------------|---------------------------------------------
0       | Unknown           | Usually set to 0
1-2     | Unknown           | Usually 00
3       | Crash bit         | MUST be 1 on most board revisions (0 = crash)
4-6     | Delay config      | Memory access timing
7       | Fetch delay       | CODE+DATA fetch delay (1 = one cycle delay)
8       | 8MB enable        | Must be SET for 8MB, CLEARED for <=4MB
9-11    | RAM config        | Bank size/configuration (see table)
12-15   | Window size       | Usually 0 (default 8MB window)
16-31   | Reserved          | Keep as 0
```

### RAM Configuration Values (bits 9-11)

| Bits 9-11 | Binary | Full Register | Configuration |
|-----------|--------|---------------|---------------|
| 0b000 | 000 | — | 1 MB on /RAS0 |
| 0b001 | 001 | — | 4 MB on /RAS0 |
| 0b100 | 100 | 0x0888 | **2 MB on /RAS0 (stock PlayStation)** |
| 0b101 | 101 | 0x0B88 | **8 MB on /RAS0 (8MB mod)** |
| 0b110 | 110 | — | 2 MB + 2 MB dual bank |
| 0b111 | 111 | — | 8 MB + 8 MB dual bank — **no longer dev-kit only, see below** |

### Current Code (PROBLEM)

**File:** `blackroo/arch/mipsnommu/ps/prom/memory.c`

```c
static void __init setup_memory_region(void)
{
    volatile int *mem_size_reg = (int *)0x1f801060;
    *mem_size_reg = 0x888;    // HARDCODED 2MB!
    mem_size = 2 << 20;       // 2MB
    add_memory_region(0, mem_size, BOOT_MEM_RAM);
}
```

This unconditionally sets 2MB regardless of installed RAM. An 8MB-modded console will only use 2MB of its RAM.

### Required Modification

```c
static void __init setup_memory_region(void)
{
    unsigned long mem_start, mem_size;
    volatile int *mem_size_reg = (int *)0x1f801060;

#if defined(CONFIG_PSX_RAM_AUTO)
    /* Auto-detect RAM size by probing */
    mem_size = psx_detect_ram_size(mem_size_reg);
#elif defined(CONFIG_PSX_8MB_RAM)
    *mem_size_reg = 0x0B88;   /* 8MB: bits 9-11=101, bit 8 set */
    mem_size = 8 << 20;
#elif defined(CONFIG_PSX_4MB_RAM)
    *mem_size_reg = 0x0988;   /* 4MB: bits 9-11=001 (needs verification) */
    mem_size = 4 << 20;
#else
    *mem_size_reg = 0x0888;   /* 2MB: bits 9-11=100 (stock) */
    mem_size = 2 << 20;
#endif

    mem_start = 0;
    add_memory_region(mem_start, mem_size, BOOT_MEM_RAM);
    printk("PSX: %lu KB RAM configured (reg=0x%08x)\n",
           mem_size >> 10, *mem_size_reg);
}
```

### Auto-Detection Algorithm

```c
static unsigned long __init psx_detect_ram_size(volatile int *mem_size_reg)
{
    volatile unsigned long *probe;
    unsigned long test_offset;

    /* Step 1: Configure for maximum (8MB) */
    *mem_size_reg = 0x0B88;

    /* Step 2: Write unique patterns at power-of-2 boundaries */
    /* Use uncached addresses (KSEG1) to avoid cache effects */
    *(volatile unsigned long *)0xA0000000 = 0xDEADBEEF;  /* Base */
    *(volatile unsigned long *)0xA0200000 = 0xCAFEBABE;  /* 2MB */
    *(volatile unsigned long *)0xA0400000 = 0x12345678;  /* 4MB */

    /* Step 3: Read back from base — if it mirrors, RAM is smaller */
    if (*(volatile unsigned long *)0xA0000000 == 0x12345678) {
        /* 4MB write wrapped to base — we have <4MB */
        if (*(volatile unsigned long *)0xA0000000 == 0xCAFEBABE) {
            /* 2MB write also wrapped — only 1MB (unlikely) */
            *mem_size_reg = 0x0888;
            return 1 << 20;
        }
        /* 2MB boundary is distinct, 4MB wrapped — 2MB system */
        *mem_size_reg = 0x0888;
        return 2 << 20;
    }

    /* Base still has original value, check 2MB boundary */
    if (*(volatile unsigned long *)0xA0200000 != 0xCAFEBABE) {
        /* 2MB write was lost — unusual, treat as 2MB */
        *mem_size_reg = 0x0888;
        return 2 << 20;
    }

    /* Check if 4MB boundary is distinct from 2MB boundary */
    if (*(volatile unsigned long *)0xA0400000 == 0x12345678) {
        /* 4MB is distinct — we have 8MB */
        *mem_size_reg = 0x0B88;
        return 8 << 20;
    }

    /* 4MB wrapped over — check if it's a 4MB config */
    *mem_size_reg = 0x0988;
    return 4 << 20;
}
```

**Caution:** The auto-detect must run BEFORE the kernel initializes page tables or other data structures in high memory, as the probe writes destructively.

---

## Memory Budget Analysis

### 2MB System (Stock PlayStation)

```
Address         Size        Content
────────────────────────────────────────────────
0x00000000      4 KB        Exception vectors + kernel entry
0x00001000      ~600 KB     Kernel code (.text)
0x00097000      ~100 KB     Kernel data (.data, .bss)
0x000B0000      ~200 KB     Page tables, kernel structs
0x000E0000      ~400 KB     InitRD (compressed ext2)
0x00148000      ~736 KB     User space (heap, stack, processes)
0x001FFFFF      ────        End of 2MB
                ════════
Total:          2,048 KB

Free for user:  ~736 KB
```

**What fits in 736KB user space:**
- BusyBox ash shell: ~30-50 KB (FLAT binary)
- Running 1-2 processes simultaneously
- Small scripts and basic file operations
- Memory card access via /dev/bu0, /dev/bu1

**What does NOT fit:**
- Multiple concurrent processes with significant data
- Any networking stack (~200KB minimum)
- Text editors, compilers, or complex tools

**Optimization strategies for 2MB:**
1. Strip kernel to absolute minimum (disable unused drivers)
2. Use XIP (Execute In Place) for initrd if possible
3. Smallest possible BusyBox config (ash + ls + cat + mount only)
4. No /proc (saves ~50KB of kernel memory)
5. Consider ROMFS instead of ext2 (smaller, read-only)

### 4MB System (Partial Upgrade)

```
Address         Size        Content
────────────────────────────────────────────────
0x00000000      4 KB        Exception vectors
0x00001000      ~700 KB     Kernel code (.text)
0x000B0000      ~150 KB     Kernel data (.data, .bss)
0x000D4000      ~300 KB     Page tables, kernel structs
0x00120000      ~500 KB     InitRD (ext2, more complete)
0x0019C000      ~2,596 KB   User space
0x003FFFFF      ────        End of 4MB
                ════════
Total:          4,096 KB

Free for user:  ~2,596 KB (2.5 MB)
```

**What fits in 2.5MB user space:**
- Full BusyBox with most applets
- 3-5 concurrent processes
- Basic text editor (vi)
- Shell scripting
- Larger scripts and data processing

### 8MB System (Full Upgrade — Primary Target)

```
Address         Size        Content
────────────────────────────────────────────────
0x00000000      4 KB        Exception vectors
0x00001000      ~800 KB     Kernel code (.text)
0x000C8000      ~200 KB     Kernel data (.data, .bss)
0x000FA000      ~400 KB     Page tables, kernel structs
0x0015E000      ~1,000 KB   InitRD (full ext2 with extras)
0x00256000      ~5,992 KB   User space
0x007FFFFF      ────        End of 8MB
                ════════
Total:          8,192 KB

Free for user:  ~5,992 KB (5.85 MB)
```

**What fits in ~6MB user space:**
- Full BusyBox + additional utilities
- RAMdisk for temporary workspace (1-2 MB)
- 10+ concurrent processes
- Text editors, scripting tools
- Memory card as swap (extends virtual memory to memcard)
- Development tools (assembler, possibly a C compiler subset)
- Custom applications (FLAT binaries)

---

## Kernel Size Optimization

### Current Kernel Config (estimated sizes)

| Component | Approximate Size | Can Disable? |
|-----------|-----------------|--------------|
| Core kernel (scheduler, VFS, etc.) | ~200 KB | No |
| MIPS R3000 arch code | ~50 KB | No |
| FPU emulator | ~100 KB | Only if no FP used |
| Memory card driver (bu.c) | ~15 KB | Optional |
| Serial console (siocon.c) | ~5 KB | No (needed for I/O) |
| GPU console | ~20 KB | Optional (serial-only mode) |
| ext2 filesystem | ~50 KB | No (needed for initrd) |
| proc filesystem | ~50 KB | Optional |
| BINFMT_FLAT | ~10 KB | No (needed for userspace) |
| Math library (lib/) | ~30 KB | Mostly needed |
| Memory management (nommu) | ~30 KB | No |
| Block device layer | ~20 KB | No (needed for memcard) |
| **Total estimated** | **~580 KB** | |

### Stripping for 2MB Target

Disable these to save ~150KB:
- `CONFIG_MIPS_FPU_EMULATOR=n` (if no floating point needed: saves ~100KB)
- `CONFIG_PROC_FS=n` (saves ~50KB)
- `CONFIG_GPUPSX_CONSOLE=n` (saves ~20KB, serial-only output)
- `CONFIG_VT=n`, `CONFIG_VT_CONSOLE=n` (saves ~10KB)
- `CONFIG_PC_KEYB=n` (saves ~5KB)

**Minimal 2MB kernel target: ~430KB**

---

## Scratchpad Memory (1KB at 0x1F800000)

The PlayStation's 1KB scratchpad is essentially the data cache used as fast SRAM. It's accessible at physical address 0x1F800000.

**Potential uses in Linux:**
- Interrupt handler fast path (copy critical ISR code here)
- DMA buffer for memory card transfers
- Stack for critical kernel sections

**Limitation:** Only 1KB, and using it requires careful management to avoid conflicts with any code that accesses it directly.

**Current status:** Not used by the kernel. Could provide minor performance improvement for interrupt handling.

---

## Kernel Configuration Options to Add

### New Kconfig Entries (for arch/mipsnommu/config.in)

```
# RAM Configuration
choice 'PlayStation RAM Size' \
    "2MB    CONFIG_PSX_2MB_RAM  \
     4MB    CONFIG_PSX_4MB_RAM  \
     8MB    CONFIG_PSX_8MB_RAM  \
     Auto   CONFIG_PSX_RAM_AUTO" 2MB

# Memory card expansion
bool 'Multi-tap memory card support (up to 8 cards)' CONFIG_PSX_MULTITAP
if [ "$CONFIG_PSX_MULTITAP" = "y" ]; then
    int 'Number of memory card slots' CONFIG_PSX_MEMCARD_SLOTS 8
fi

# Ramdisk configuration
if [ "$CONFIG_PSX_8MB_RAM" = "y" -o "$CONFIG_PSX_RAM_AUTO" = "y" ]; then
    bool 'Large ramdisk support (for 8MB systems)' CONFIG_PSX_LARGE_RAMDISK
fi
```

---

## Page Table and Memory Management (uClinux)

Since there is no MMU, uClinux uses a flat memory model:

- **No page tables** in the traditional sense
- Memory is allocated from a flat heap
- `kmalloc()` returns physical addresses directly
- User processes share the same address space as the kernel
- `fork()` is NOT available — use `vfork()` + `exec()`
- Memory fragmentation is a real concern with no virtual memory

### Memory Allocator

The kernel uses a buddy allocator with no virtual memory overhead. Each page is 4KB. For a 2MB system, that's 512 pages total, with perhaps 180-200 pages free for user space after boot.

---

## 16 MB is real on retail hardware (2026)

The `0b111` dual-bank value above was long assumed to be a dev-kit-only
configuration. That framing was wrong twice over.

**Sony's dev boards were not dual-bank.** psx-spx's dev board chipset reference
lists DTL-H2000 and DTL-H2500 as **8 MB single-bank with unified /RAS control**
(4x KM48V2104AJ-6 / AT-6) — i.e. the `0b101` configuration a PU-18 chip swap
produces. The two-bank arrangement is attributed to Sony's **arcade** hardware,
not the dev kits.

**And it has now been demonstrated on a retail console.** So 16 MB is not
"unlocking the dev kit" — it goes beyond anything Sony shipped in a dev board.

- Credited to modder **TunerTom**, with the psxdev Discord community; demonstrated
  publicly by Tito Perez (Macho Nacho Productions).
- The capability was reportedly always in the CPU — Sony used two 8 MB banks on
  some of its own arcade hardware and simply never populated retail units.
- The console reports it as **two separate 8 MB banks**, exactly matching `0b111`.

**Method:** the four stock 512 KB EDO chips are replaced with **eight** 2 MB EDO
chips harvested from old PC memory modules — four in the original footprints and
**four stacked on top of those**. It additionally needs a custom quick-solder
board, trace cuts, precision wiring, and a resistor to enable the second bank.

**Caveat worth repeating before anyone attempts it:** commercial game
compatibility is mixed. Some titles run, others fail to boot because they assume
the original memory layout. The clear beneficiary is **homebrew** — which is us.

For chip part numbers (including the parts that do *not* work) see
`docs/01-ARCHITECTURE.md`.

---

## Swap on Memory Card

For 8MB systems, optional swap on a dedicated memory card:

```
# In /etc/fstab or init script:
mkswap /dev/bu1          # Format card 2 as swap
swapon /dev/bu1           # Enable swap

# Or with multiple cards via RAID:
mkswap /dev/bul           # Format joined cards as swap
swapon /dev/bul
```

**Performance reality:**
- Memory card I/O: 250 kHz serial = ~31 KB/s raw
- With protocol overhead: ~15-20 KB/s effective
- Swap page size: 4 KB
- Time per swap page: ~200-260ms
- **This is extremely slow** but can prevent OOM kills

Swap is only viable as a last resort to avoid process killing, not for active working set paging.

---

*Blackroo Linux Memory Subsystem Documentation*
