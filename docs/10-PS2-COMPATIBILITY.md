# Blackroo Linux - PS2 Compatibility Analysis

> Can the PS1 Linux kernel run on a PlayStation 2? Short answer: No. But the PS2 is
> a valuable development tool for this project.

---

## The Question

The PlayStation 2's IOP (I/O Processor) is an R3000A — the same CPU as the PS1. Since Blackroo Linux targets the R3000A, can it run on a PS2?

## The Answer: No

The CPU instruction set is compatible, but **every hardware peripheral register is at a different address** on the PS2. The Blackroo kernel has dozens of hardcoded PS1 register addresses that simply don't exist on the PS2 IOP.

### Specific Incompatibilities

| Component | PS1 Address | PS2 IOP | Status |
|-----------|-------------|---------|--------|
| RAM_SIZE register | 0x1F801060 | Does not exist | **Incompatible** |
| Interrupt status | 0x1F801070 | Different IRX system | **Incompatible** |
| Interrupt mask | 0x1F801074 | Different IRX system | **Incompatible** |
| SIO0 (memcard) | 0x1F801040 | SIO2 (0x1F808200+) | **Incompatible** |
| SIO1 (serial) | 0x1F801050 | Different address | **Incompatible** |
| GPU registers | 0x1F801810 | Goes through EE | **Incompatible** |
| Timer 0/1/2 | 0x1F801100+ | Different layout | **Incompatible** |
| DMA controller | 0x1F801080+ | Different DMA system | **Incompatible** |
| I/O port base | 0x1F800000 | Different mapping | **Incompatible** |

### Files With Hardcoded PS1 Addresses

These kernel files would ALL need rewriting for PS2 IOP:

```
arch/mipsnommu/ps/prom/memory.c    — RAM_SIZE at 0x1F801060
arch/mipsnommu/ps/setup.c          — mips_io_port_base = PSX_HW_REG_BASE
arch/mipsnommu/ps/irq.c            — INT_ACKN at 0x1F801070, INT_MASK at 0x1F801074
arch/mipsnommu/ps/time.c           — Timer regs at 0x1F801120+
arch/mipsnommu/ps/siocon.c         — SIO1 regs at 0x1F801050+
drivers/block/bu.c                 — SIO0 regs at 0x1F801040+
drivers/block/bu.h                 — BU_DATA=0x1040, BU_STATUS=0x1044, etc.
include/asm-mipsnommu/ps/hwregs.h  — All register definitions
include/asm-mipsnommu/ps/interrupts.h — IRQ mapping
include/asm-mipsnommu/ps/sio.h     — SIO register addresses
include/asm-mipsnommu/ps/timer.h   — Timer register addresses
```

### Additional PS2 IOP Barriers

1. **IOP is sandboxed:** The PS2's Emotion Engine (EE) controls the IOP. The IOP runs firmware modules (IRX format), not standalone code.
2. **Memory management:** IOP has 2MB RAM but it's managed by the EE, not via a RAM_SIZE register.
3. **No direct boot:** You can't just load a PS-EXE onto the PS2 IOP — it must be loaded through the EE's bootstrap.
4. **Peripheral access:** Memory cards, controllers, and serial go through IRX modules (mcman.irx, sio2man.irx), not direct register access.

### What It Would Take

Porting Blackroo to PS2 IOP would require:
- A complete new `arch/mipsnommu/ps2iop/` board directory
- New register definitions for every peripheral
- New interrupt handler using IOP's interrupt system
- New SIO2 driver (replacing SIO0 for memory cards)
- A PS2-side loader to bootstrap the kernel onto the IOP
- **Essentially a new port from scratch** — the only shared code would be the core kernel and R3000 CPU support

This is a separate project, not a configuration option.

---

## PS2 as a Development Tool

While the PS2 can't **run** Blackroo Linux, it's extremely useful as a **tool**:

### 1. Memory Card Formatter

The PS2 can read/write PS1 memory cards via its backward-compatible card slots. A PS2 homebrew application can:

- Detect PS1 memory cards in PS2 card slots
- Write Blackroo headers (BU_ID, size, serial, sequence number)
- Write an ext2 filesystem across multiple cards
- Verify card contents by reading back
- Format cards in the correct sequence for RAID

```
PS2 Homebrew App (PS2SDK)
    │
    ▼ PS1 card backward compat
┌────────────────────────┐
│ PS1 Memory Card Slot 1 │ → Write Blackroo header + data for Card 0
│ PS1 Memory Card Slot 2 │ → Write Blackroo header + data for Card 1
└────────────────────────┘
    │
    ▼ Swap cards, repeat
Cards 2-7 formatted in sequence
```

### 2. Card Image Transfer

The PS2 can copy data between:
- USB storage → PS1 memory card
- PS2 memory card → PS1 memory card
- Network → PS1 memory card (with PS2 network adapter)

This makes the PS2 a practical "memory card writer" for preparing Blackroo boot cards.

### 3. Testing Platform (Limited)

The PS2 in PS1 backward compatibility mode runs PS1 code on the IOP. While Linux won't boot (register incompatibility), simple PS1 test programs (memory card read/write tests, serial echo tests) may work. The PS2's PS1 compatibility is not perfect — some hardware edge cases differ.

---

## PS2 Card Formatter Design

### PS2SDK Application Outline

```c
/* ps2_blackroo_formatter.c - PS2 homebrew to format PS1 cards for Blackroo */
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <stdio.h>
#include <string.h>

/* PS1 memory card access on PS2 uses mcman/mcserv IRX modules */
#include <libmc.h>

/* Blackroo card header (matches bu.h bu_first_block_t) */
typedef struct {
    uint32_t id;       /* 0x1234 = BU_ID */
    uint32_t size;     /* 1024 (blocks per card) */
    uint32_t serial;   /* Unique serial number */
    uint32_t number;   /* Sequence: 0, 1, 2, ... */
    uint8_t  reserved[112]; /* Pad to 128 bytes */
} blackroo_header_t;

int main(int argc, char *argv[]) {
    /* Initialize PS2 RPC and load IRX modules */
    SifInitRpc(0);
    /* Load mcman.irx and mcserv.irx for memory card access */
    
    /* Detect PS1 cards in PS2 slots */
    /* Note: PS2 slot 0 and 1 can accept PS1 cards */
    
    /* For each card:
       1. Read current contents (backup if needed)
       2. Write Blackroo header at block 0
       3. Write ext2 filesystem data (or zeros)
       4. Verify by reading back
       5. Report success/failure on screen
    */
    
    return 0;
}
```

### Build with PS2SDK

```bash
# Using PS2SDK Docker container or local install
export PS2SDK=/usr/local/ps2dev/ps2sdk
make
# Produces: blackroo_formatter.elf
# Load via FreeMcBoot, USB, or network
```

This is a straightforward PS2 homebrew project. The hard part is already solved — PS2SDK provides `libmc` for PS1 memory card access.

---

## Conclusion

| Use Case | PS2 Viability |
|----------|---------------|
| Run Blackroo Linux on PS2 | **No** — hardware registers incompatible |
| Format PS1 memory cards via PS2 | **Yes** — excellent use case |
| Transfer data to PS1 cards via PS2 | **Yes** — via PS2 homebrew |
| Test PS1 programs on PS2 | **Limited** — PS1 compat mode has quirks |
| Full PS2 IOP Linux port | **Possible but separate project** |

The PS2 is a **tool** for Blackroo, not a **target**.

---

*Blackroo Linux — PS2 Compatibility Analysis*
