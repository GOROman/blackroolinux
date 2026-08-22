# Blackroo Linux - PlayStation 1 Hardware Architecture Reference

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: UniROM as the upload path, BINFMT_FLAT, the 8 MB mod, the FPU emulator.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> Technical reference for the Sony PlayStation 1 (PSX) hardware as it relates to running Linux

---

## CPU: MIPS R3000A (CXD8530BQ / CXD8530CQ / CXD8606BQ)

### Specifications

| Parameter | Value |
|-----------|-------|
| Architecture | MIPS I (32-bit RISC) |
| Clock Speed | 33.8688 MHz (44100 Hz x 768) |
| Byte Order | Little-endian |
| Registers | 32 general-purpose (32-bit) + HI/LO for multiply/divide |
| Pipeline | 5-stage (IF, RD, ALU, MEM, WB) |
| Cache | 4 KB instruction cache, 1 KB data scratchpad (not a true cache) |
| MMU | **None** — no virtual memory, no TLB |
| FPU | **None** — requires software emulation |
| Coprocessors | COP0 (System Control), COP2 (GTE - Geometry Transform Engine) |

### Implications for Linux

- **No MMU:** Must use uClinux (CONFIG_UCLINUX=y) with flat memory model
- **No FPU:** Kernel includes full software FPU emulator (`arch/mipsnommu/ps/math-emu/`, 43 files)
- **No virtual memory:** All processes share a single flat address space
- **Binary format:** BINFMT_FLAT only (no standard ELF relocation without MMU)
- **No memory protection:** A misbehaving process can crash the entire system

### COP0 (System Control Coprocessor)

| Register | Number | Purpose |
|----------|--------|---------|
| SR (Status) | $12 | Interrupt enable, kernel/user mode, cache control |
| Cause | $13 | Exception cause code, pending interrupts |
| EPC | $14 | Exception return address |
| PRId | $15 | Processor ID (0x00000002 for R3000A) |
| BadVAddr | $8 | Address that caused address exception |

**Status Register (SR) key bits:**
- Bit 0: IEc (Current Interrupt Enable)
- Bit 1: KUc (Current Kernel/User mode)
- Bits 8-15: Interrupt mask (IM0-IM7)
- Bit 16: Isc (Isolate Cache — write to scratchpad)
- Bit 22: BEV (Boot Exception Vectors — 1=ROM, 0=RAM)

---

## Memory Map

### Physical Address Space

```
0x00000000 ┌──────────────────────────────────────────┐
           │  Main RAM                                 │
           │  Stock: 2 MB (0x00000000 - 0x001FFFFF)   │
           │  8MB mod: 8 MB (0x00000000 - 0x007FFFFF) │
0x00200000 ├──────────────────────────────────────────┤ (2MB boundary)
           │  (mirrors of main RAM in stock config)    │
0x00800000 ├──────────────────────────────────────────┤ (8MB boundary)
           │  (unmapped)                               │
0x1F000000 ├──────────────────────────────────────────┤
           │  Expansion Region 1 (PIO port)            │
           │  8 MB address space                       │
           │  Used for: Cheat carts, dev hardware      │
0x1F800000 ├──────────────────────────────────────────┤
           │  Scratchpad (D-cache as fast RAM)          │
           │  1 KB (0x1F800000 - 0x1F8003FF)          │
0x1F801000 ├──────────────────────────────────────────┤
           │  Hardware I/O Registers                    │
           │  8 KB (0x1F801000 - 0x1F802FFF)          │
0x1F802000 ├──────────────────────────────────────────┤
           │  Expansion Region 2 (POST output, etc.)   │
0x1FA00000 ├──────────────────────────────────────────┤
           │  Expansion Region 3                       │
0x1FC00000 ├──────────────────────────────────────────┤
           │  BIOS ROM                                 │
           │  512 KB (0x1FC00000 - 0x1FC7FFFF)        │
0x1FC80000 └──────────────────────────────────────────┘
```

### KSEG Address Mapping (MIPS R3000)

The R3000 uses fixed address segments (no TLB needed):

| Segment | Virtual Address | Physical Address | Cached | Purpose |
|---------|----------------|------------------|--------|---------|
| KUSEG | 0x00000000-0x7FFFFFFF | Pass-through | Yes | User space (2GB) |
| KSEG0 | 0x80000000-0x9FFFFFFF | Virt - 0x80000000 | Yes | Kernel cached |
| KSEG1 | 0xA0000000-0xBFFFFFFF | Virt - 0xA0000000 | No | Kernel uncached |
| KSEG2 | 0xC0000000-0xFFFFFFFF | — | — | Kernel mapped (unused, no TLB) |

**Example:** BIOS at physical `0x1FC00000` is accessed as:
- Cached: `0x9FC00000` (KSEG0)
- Uncached: `0xBFC00000` (KSEG1) — this is the reset vector

**For hardware I/O registers:** Always use KSEG1 (uncached) to avoid stale cache reads.

---

## Hardware I/O Register Map

### Overview (0x1F801000 - 0x1F802FFF)

```
0x1F801000  Memory Control 1 (expansion regions, BIOS delays)
0x1F801010  Memory Control 2 (RAM_SIZE register)
0x1F801020  Memory Control 3
0x1F801040  SIO0 (Controllers / Memory Cards)
0x1F801050  SIO1 (Serial Port / UART)
0x1F801060  RAM_SIZE register ← CRITICAL for 8MB mod
0x1F801070  Interrupt Control (I_STAT, I_MASK)
0x1F801080  DMA Registers
0x1F801100  Timer Registers (3 timers)
0x1F801800  CD-ROM Registers
0x1F801810  GPU Registers
0x1F801820  MDEC Registers
0x1F801C00  SPU Registers
0x1F802000  Expansion Region 2
```

### Interrupt Controller (0x1F801070 - 0x1F801074)

| Address | Register | Purpose |
|---------|----------|---------|
| 0x1F801070 | I_STAT | Interrupt status (read: pending, write: acknowledge) |
| 0x1F801074 | I_MASK | Interrupt mask (1 = enabled) |

**IRQ Sources (as implemented in kernel):**

| IRQ | Bit | Mask | Name | Kernel Use |
|-----|-----|------|------|------------|
| 0 | 6 | 0x040 | TIMER2 | System tick (jiffies) |
| 1 | 7 | 0x080 | CONTROLLER | Memory card / controller SIO |
| 2 | 10 | 0x400 | PIO | Parallel I/O expansion port |
| 3 | 3 | 0x008 | DMA | DMA transfer complete |
| 4 | 4 | 0x010 | TIMER0 | Timer 0 (dot clock / hblank) |
| 5 | 5 | 0x020 | TIMER1 | Timer 1 (hblank / vblank) |
| 6 | 1 | 0x002 | GPU | GPU ready / command complete |
| 7 | 0 | 0x001 | VBL | Vertical blank (NTSC: 60Hz, PAL: 50Hz) |
| 8 | 8 | 0x100 | SIO | Serial port interrupt |
| 9 | 9 | 0x200 | SPU | Sound processor |
| 10 | 2 | 0x004 | CDROM | CD-ROM controller |

**Interrupt acknowledge:** Write `1` to the corresponding bit in I_STAT to clear it. The kernel's `ack_psx_irq()` does this.

### SIO0 — Controllers and Memory Cards (0x1F801040)

| Address | R/W | Register | Purpose |
|---------|-----|----------|---------|
| 0x1F801040 | R/W | SIO0_TX/RX_DATA | Data (8-bit, directly read/written) |
| 0x1F801044 | R | SIO0_STAT | Status flags |
| 0x1F801048 | R/W | SIO0_MODE | Mode (baud rate multiplier, char length, stop bits) |
| 0x1F80104A | R/W | SIO0_CTRL | Control (TX/RX enable, DTR, interrupt enables) |
| 0x1F80104E | R/W | SIO0_BAUD | Baud rate reload value |

**SIO0_CTRL bit assignments for card selection:**
- Bit 0: TX enable
- Bit 1: DTR output (directly drives /CS for slot selection)
- Bit 13: Port select (0 = slot 1 / DTR0, 1 = slot 2 / DTR1)

**SIO0_BAUD for memory cards:** Set to `0x88` (~250 kHz at 33.8688 MHz / baud_factor)

### SIO1 — Serial Port / UART (0x1F801050)

| Address | R/W | Register | Purpose |
|---------|-----|----------|---------|
| 0x1F801050 | R/W | SIO1_TX/RX_DATA | Data register |
| 0x1F801054 | R | SIO1_STAT | Status flags |
| 0x1F801058 | R/W | SIO1_MODE | Mode config |
| 0x1F80105A | R/W | SIO1_CTRL | Control (DTR, RTS, IRQ enables) |
| 0x1F80105E | R/W | SIO1_BAUD | Baud rate |

**Kernel configuration (siocon.c):**
```
Mode:  SIO_BRS16 | SIO_CHR8 | SIO_SB1  (16x baud, 8-bit, 1 stop)
Baud:  SIO_B11520 (= 115200 baud)
Ctrl:  SIO_TX | SIO_RX | SIO_DTR | SIO_RTS (TX/RX enable, handshake)
```

### Timer Registers (0x1F801100 - 0x1F801128)

| Timer | Counter | Mode | Target | Use |
|-------|---------|------|--------|-----|
| Timer 0 | 0x1F801100 | 0x1F801104 | 0x1F801108 | Dot clock / HBlank |
| Timer 1 | 0x1F801110 | 0x1F801114 | 0x1F801118 | HBlank / VBlank |
| Timer 2 | 0x1F801120 | 0x1F801124 | 0x1F801128 | **System clock / 8** (kernel jiffies) |

**Timer 2** is used by the kernel for the system tick. It counts at sysclock/8 = 4.233600 MHz.

### GPU Registers (0x1F801810 - 0x1F801814)

| Address | R/W | Register | Purpose |
|---------|-----|----------|---------|
| 0x1F801810 | W | GP0 | Rendering commands / VRAM access |
| 0x1F801810 | R | GPUREAD | Read result from VRAM / GPU |
| 0x1F801814 | W | GP1 | Display control commands |
| 0x1F801814 | R | GPUSTAT | GPU status register |

The kernel uses the GPU for console text output (`CONFIG_GPUPSX_CONSOLE`).

### RAM_SIZE Register (0x1F801060)

This is the critical register for memory expansion. See `docs/02-MEMORY-SUBSYSTEM.md` for full details.

---

## DMA Controller

The PSX has 7 DMA channels:

| Channel | Direction | Device |
|---------|-----------|--------|
| DMA0 | To RAM | MDEC (decoder) |
| DMA1 | From RAM | MDEC (encoder) |
| DMA2 | Both | GPU |
| DMA3 | To RAM | CD-ROM |
| DMA4 | Both | SPU |
| DMA5 | Both | PIO (expansion port) |
| DMA6 | From RAM | GPU OTC (ordering table clear) |

**DMA registers base:** 0x1F801080
- Each channel: Base + (channel * 0x10)
- DMA control: 0x1F8010F0 (DPCR)
- DMA interrupt: 0x1F8010F4 (DICR)

The memory card driver (`bu.c`) does NOT use DMA — it uses programmed I/O through SIO0 at 250 kHz. This is a potential future optimization point, though SIO0 is inherently serial.

---

## Exception Vectors

### Boot Exception Vectors (BEV=1, ROM)

| Address | Exception |
|---------|-----------|
| 0xBFC00000 | Reset |
| 0xBFC00100 | UTLB Miss (unused on R3000A in PSX) |
| 0xBFC00180 | General Exception |

### Runtime Exception Vectors (BEV=0, RAM)

| Address | Exception |
|---------|-----------|
| 0x80000000 | UTLB Miss |
| 0x80000080 | General Exception |

The kernel's `head.S` sets up the runtime exception vectors at 0x80000080 and clears BEV to redirect exceptions to RAM.

---

## Power-On Boot Sequence

```
1. CPU starts at reset vector 0xBFC00000 (BIOS ROM)
2. BIOS initializes hardware:
   - RAM_SIZE register (0x1F801060) set to 0x0888 (2MB)
   - Cache initialized
   - GPU reset
   - SPU silenced
   - Interrupt controller cleared
3. BIOS copies exception handlers to RAM
4. BIOS reads memory card slot 1 (for icon display)
5. BIOS checks for CD-ROM:
   - Valid disc → load PSX-EXE from CD
   - No disc → display Sony logo, go to memory card browser
6. If PS-EXE loaded, jump to its entry point
7. Kernel's head.S takes over:
   - Clear BSS
   - Set up stack
   - Configure cache
   - Clear BEV (use RAM exception vectors)
   - Jump to start_kernel()
```

When using UniROM (loaded via cheat cart or FreePSXBoot):
```
1-4. Same as above
5. UniROM gains control (from cartridge or memory card exploit)
6. UniROM waits for serial commands
7. Host sends PS-EXE via serial (nops tool)
8. UniROM loads PS-EXE to RAM and jumps to entry point
9. Kernel's head.S takes over (same as step 7 above)
```

---

## Board Revisions Relevant to 8MB Mod

| Board | CPU Package | RAM Type | 8MB Mod Method |
|-------|-------------|----------|----------------|
| PU-7 | 160-pin QFP | 4x 512Kx8 | Chip swap |
| PU-8 | 160-pin QFP | 4x 512Kx8 | Chip swap |
| PU-18 | 160-pin QFP | 4x 512Kx8 | Chip swap (KM48V2104AJ-6) |
| PU-20 | 208-pin QFP | 2x 1Mx16 | Complex, not recommended |
| PU-22 | 208-pin QFP | 2x 1Mx16 | Daughterboard ([hkzlab mod](https://github.com/hkzlab/PS1_PU22_8MB_mod)) |
| PU-23 | 208-pin QFP | 2x 1Mx16 | Same as PU-22 |
| PM-41 | PSOne SoC | 2x 1Mx16 | Same as PU-22 (tight space) |

**RAM chip requirements for 8MB:**
- 3.3V EDO DRAM
- 2048 refresh cycles (NOT 4096!)
- Example: KM48V2104AJ-6, GM71V18163CJ6

### Buy list — resolved 2026-08-22

| Role | Part | Notes |
|---|---|---|
| **Stock chip (PU-18), being removed** | Samsung **KM48V514BJ-6** | 512K x 8. Four fitted = 2 MB. |
| **Replacement for 8 MB** | Samsung **KM48V2104AJ-6** | 2M x 8. **Four needed.** 1024 row x 2048 column addresses, 2048 refresh over 32 ms, 3.3 V I/O. |
| Also reported working | Samsung **KM48V2105AJ-6** | |
| **Donor-module route** | 144-pin 16 MB EDO SO-DIMM, e.g. Samsung **KMM466F213BS1-L6** | Its chips are **KM48V2104BS-L6**. Correct die, **but the `BS` suffix is not the SOJ package** — SO-DIMMs use TSOP. Only useful for a PU-18 with an adapter or rework. For a straight drop-in you want the **`AJ`** suffix. |

### It is NOT a pinout-compatible swap — 5 pins change function

Same SOJ-28 footprint, **different pin assignment**. From the PU-18 mod
documentation:

| Pin | KM48C/V514BJ (out) | KM48V2104 (in) |
|---|---|---|
| 6 | N.C | **/WE** |
| 7 | /WE | **/RAS** |
| 8 | /RAS | **N.C** |
| 9 | A9 | **A10** |
| 21 | N.C | **A9** |
| 20 | A8 | A8 — *same function, but must be rerouted from the A11 line to A8* |

Everything else is identical: pins 1/14 VDD, 15/28 VSS, 2-5 and 24-27 the data
lines, 22 /OE, 23 /CAS, 10-13 A0-A3, 16-19 A4-A7.

Net rework: **4 traces cut, 6 connections made.**

(Data-line naming differs cosmetically — the old part labels them DQ0-DQ7, the new
one DQ1-DQ8. Same physical lines, different index base.)

**Consequence for cross-referencing:** a substitute must match the *KM48V2104*
pin assignment, not the stock one. Do not assume any 2M x 8 SOJ-28 part drops in.

### Valid package suffixes — wider than first thought

The datasheet page covers **AJ / ALJ / ALLJ / ASLJ** together, all SOJ 400 mil,
all sharing the pinout above. So the sourcing search should include:

- KM48V2104**AJ**-6
- KM48V2104**ALJ**-6
- KM48V2104**ALLJ**-6
- KM48V2104**ASLJ**-6

Still excluded: `BK`, `BS`, `AT` (other packages) and anything `KM48C` (5 V).

### Package — the thing that decides drop-in fit

**PU-18 needs SOJ-28.** Both the stock and replacement parts are SOJ-28
(PDSO28), which is exactly why this is a chip swap rather than a daughterboard:

| | Part | Package | Organisation |
|---|---|---|---|
| Out | KM48V514BJ-6 | SOJ-28 (`J`) | 512K x 8 |
| In | KM48V2104**AJ**-6 | SOJ-28 (`AJ`) | 2M x 8, EDO, 3.3 V, 60 ns |

**Read the package suffix, not just the number.** In Samsung's scheme `J` is SOJ;
other suffixes (`K`, `S`, `BS`) are different packages and will not sit on the
PU-18 footprint. `KM48V2104BK-6` and `KM48V2104BS-L6` are the same die in the
wrong body.

### Validation: this is the dev-kit configuration

psx-spx's dev board chipset reference confirms Sony's own development hardware
used **the same part**:

| Board | Main RAM | Chips | Position |
|---|---|---|---|
| **DTL-H2000** (ISA) | **8 MB** | 4x 28-pin SEC **KM48V2104AJ-6** (2M x 8) | U8-U11 |
| **DTL-H2500** (PCI) | **8 MB** | 4x 28-pin SEC **KM48V2104AT-6** (2M x 8) | IC106-IC109 |
| DTL-H2700 | unknown | daughterboards block access | |

Two things follow:

1. **The part is validated by Sony's own usage.** A PU-18 8 MB swap is not exotic
   — it rebuilds the DTL-H2000 memory configuration inside a retail shell.
2. **28-pin confirms the SOJ-28 footprint** for the `AJ` suffix. Note DTL-H2500
   used `AT`, a different package on a board laid out for it — further reason to
   buy on the suffix, not the base number.

Dev boards ran 8 MB regardless of bus architecture, and psx-spx describes the
DTL-H2000 arrangement as **single-bank with unified /RAS control** — which is
exactly what a PU-18 chip swap produces.

CPU chipsets for reference: DTL-H2000 = Sony CXD8530BQ (208-pin);
DTL-H2500 = CXD8530CQ. VRAM on both is 2x SEC KM4216V256G-60 (256K x 16).

Source: <https://github.com/psx-spx/psx-spx.github.io/blob/master/docs/psxdevboardchipsets.md>

### Sourcing

- Buy **KM48V2104AJ-6** outright from legacy-component brokers (Octopart and
  PartStack both index it) or the usual surplus channels.
- **ISSI** still lists a 3.3 V EDO/FPM DRAM line and is worth checking for an
  in-production cross-reference.
- Obsolete DRAM is a well-known target for remarked and counterfeit parts. Buy
  spares, and buy from more than one seller if you can.

### Second source confirmed: Mitsubishi M5M4V17805CJ-6

Found 2026-08-23 by screening a 16 MB 144-pin EDO SO-DIMM (3.3 V, 60 ns,
8 chips dual-sided) carrying **M5M4V17805CTP-6**. Arithmetic checks out:
64-bit bus / 8 chips = 8 bits each, 16 MB / 8 = 2 MB each — **2M x 8, 16 Mbit**.

That die is correct on every electrical spec: 2M x 8, 16 Mbit, EDO, 3.3 V, 60 ns.
Only the package is wrong — `TP` = TSOP.

**So the part to buy is `M5M4V17805CJ-6`** — same die, `CJ` = SOJ. Note this is
one character from `M5M417805CJ-6`, the **5 V** part that was mistakenly fitted
first: the `V` is the entire difference.

| Part | Volts | Package | Verdict |
|---|---|---|---|
| M5M4**17805CJ**-6 | 5 V | SOJ | wrong rail — was fitted, no boot |
| M5M4**V**17805**CTP**-6 | 3.3 V | TSOP | right die, wrong body |
| **M5M4V17805CJ-6** | **3.3 V** | **SOJ** | **target** |

**Refresh confirmed 2026-08-23: 2048.** Full parametric data for the family
(taken from the `M5M4V17805CTP-5ST` entry; the die is common across package
suffixes):

| Parameter | Value |
|---|---|
| Memory organisation | **2M x 8** |
| Density | 16,777,216 bits (16 Mbit) |
| Type | **EDO DRAM** (fast page with EDO) |
| **Refresh cycles** | **2048** |
| Supply voltage | **3.3 V nom** (3.0-3.6) |
| Terminals | **28** |
| Terminal pitch | **1.270 mm** |
| Body length x width | 18.41 x 10.16 mm |
| Refresh modes | RAS-only / CAS-before-RAS / hidden / **self-refresh** |
| Speed grades | -5 / -6 / -7 (plus S) |

Every requirement met. **`M5M4V17805CJ-6` is a fully validated second source**
alongside the Samsung.

**Pinout verified identical to KM48V2104** (Mitsubishi datasheet, outline
28P0N-A, 400 mil SOJ). All 28 pins match, including the six the PU-18 rework
touches: 6 = /W, 7 = /RAS, 8 = NC, 9 = A10, 20 = A8, 21 = A9. A board reworked
for the Samsung part accepts the Mitsubishi with **no further changes**.

Note the `CTP` (TSOP) variant carries the **same pinout and is also a 400 mil
outline** (28P3N-C). So pinout is not an obstacle to using TSOP parts — only
lead form (gull-wing vs J-lead) and lead span differ. Compare outline drawings
28P0N-A and 28P3N-C for exact span before attempting to fit TSOP to SOJ pads.

### CONFIRMED DONOR MODULE — Kingston KTM2MX72S

Found 2026-08-23. A **16 MB, 144-pin, no-parity SO-DIMM, 8 chips dual-sided**,
populated with **`M5M4V17805CJ-6S`** — the SOJ part, not the TSOP one.

Arithmetic checks out: 144-pin is a 64-bit bus, 8 chips x 8 bits = 64, and
16 MB / 8 = 2 MB per chip = **2M x 8**.

| Check | Value | |
|---|---|---|
| Organisation | 2M x 8, 16 Mbit | OK |
| Type | EDO | OK |
| Voltage | 3.3 V (the `V`) | OK |
| Refresh | 2048 | OK |
| **Package** | **`CJ` = 400 mil SOJ-28** | **OK** |
| Speed | -6S = 60 ns, self-refresh capable | OK |
| Pinout | identical to KM48V2104 | OK |

**All six criteria met — datasheet-verified**, not inferred. From the Mitsubishi
`M5M4V17805CJ,TP-5,-6,-7,-5S,-6S,-7S` datasheet:

- "HYPER PAGE MODE 16777216-BIT (2097152-WORD BY 8-BIT) DYNAMIC RAM"
- "Standard 28 pin SOJ, 28 pin TSOP"
- **"Single 3.3V +/-10% supply"**
- **"2048 refresh cycles every 32ms (A0~A10)"**
- Pin description: "Vcc — Power supply (+3.3V)"
- Speed grades: -5/-5S 50 ns, **-6/-6S 60 ns**, -7/-7S 70 ns (RAS access)
- "All inputs, outputs TTL compatible"
- `S` suffix = self-refresh option; an added capability, not a behaviour change

The `A0~A10` line is the direct statement of the 2048-row requirement the PU-18
rework wires for — eleven address inputs, no twelfth.

**Eight chips is four for the 8 MB mod plus four spares, or all eight for the
16 MB dual-bank mod.**

The `-6S` suffix denotes self-refresh capability — an added feature, not a
behavioural change; the console drives RAS/CAS refresh normally regardless.

Harvesting requires hot air, but SOJ is considerably more forgiving to remove
and refit than TSOP, and the PU-18 footprint is SOJ anyway.

**This is the cheapest confirmed route to the correct part.** Loose brokered
chips remain the alternative if module supply dries up.

### Refresh count = row count — a shortcut

Refresh cycles equal the number of row addresses, because one row is refreshed
per cycle. 2048 rows means **11 row-address bits, A0-A10**.

The PU-18 rework routes A10 to pin 9 and A9 to pin 21, giving the chip exactly
**A0-A10**. So the board itself states the requirement, and it explains why
`KM48V2004` fails: 4096 rows needs A0-A11, and a 28-pin package has nowhere to
put the twelfth line.

**Fast screening rule: count the address pins on any candidate datasheet.
A0-A10 = good. A0-A11 = reject.** Far easier to find on a pinout page than a
refresh table.

The 144-pin module above remains a known-good source of the correct die if `CJ`
proves unobtainable — which makes a TSOP-to-SOJ-28 adapter a fallback with a
confirmed part behind it rather than a gamble.

### Donor-module route — and the trap in it

Harvesting chips from old memory modules is the obvious idea. The catch is that
**package and voltage pull in opposite directions**:

| Module type | Voltage | Chip package | Verdict |
|---|---|---|---|
| **Desktop 72-pin SIMM** (e.g. 8 MB EDO) | **5 V** | **SOJ-28** | **DO NOT USE.** Right package, right organisation, *wrong voltage.* |
| **Laptop 72-pin SO-DIMM** (16 MB EDO) | **3.3 V** | usually TSOP | Right voltage, wrong package |
| **Laptop 144-pin SO-DIMM** (16 MB EDO) | **3.3 V** | usually TSOP | Right voltage, wrong package |

The dangerous one is the desktop SIMM. An 8 MB 72-pin EDO SIMM is built from four
**KM48C2104AJ-6** — the exact 5 V sibling of our part, same SOJ-28 body, same
2M x 8 organisation. It looks like a perfect donor and will overdrive the 3.3 V
CPU inputs. **`C` = 5 V. Check the letter, every time.**

3.3 V EDO of this era lived in *laptop* memory, and by then the industry had
largely moved to TSOP — which is why loose SOJ 3.3 V parts are scarce.

**Confirmed 3.3 V EDO donor modules** (chips still need package verification):

- Samsung **KMM466F213BS1-L6** — 144-pin, 16 MB EDO; chips are KM48V2104BS-L6
- **Micron MT8LDT432HG-6X** — 72-pin SO-DIMM, 16 MB EDO, 60 ns, 3.3 V
- 72-pin SO-DIMM, 16 MB EDO, 60 ns, 3.3 V — 8x **Mitsubishi M5M4V17405DTP-6**
- IBM **92G7341** — 144-pin, 16 MB EDO, 70 ns, 3.3 V

**Buying from photos:** SOJ chips are chunky with J-leads folded under the body;
TSOP are flat and wide with gull-wing leads along the long edges. Insist on a
clear photo of the markings and look for a `KM48V...AJ` style suffix.

**Practical conclusion:** buy loose `KM48V2104AJ-6` while brokers still have it.
The module route only helps if paired with a TSOP-to-SOJ adapter board — viable
since we have PCB fab access, but it is a mod on top of a mod.

Search filter, if hunting a cross-reference: **2M x 8, 16 Mbit, EDO, 3.3 V,
2048 refresh, 60 ns or faster, SOJ-28.**

### Parts that explicitly DO NOT work

| Part | Why |
|---|---|
| **KM48C...** (any) | The **C** denotes the 5 V variant. Also, a trailing **0** in these part numbers marks fast-page-mode; the PSX memory controller needs **EDO**. |
| **KM48V2004AJ-6** | **4096 row addresses.** The memory controller does not support it. Note this is one digit from the part that does work — `2004` vs `2104`. Read the marking twice. |

The `2004` / `2104` trap is the single easiest way to waste money here.

---

## References

- [psx-spx: Complete PSX Specifications](https://psx-spx.consoledev.net/)
- [psx-spx: Memory Control](https://psx-spx.consoledev.net/memorycontrol/)
- [psx-spx: Interrupt Controller](https://psx-spx.consoledev.net/interruptcontrol/)
- [psx-spx: SIO](https://psx-spx.consoledev.net/serialinterfacessio/)
- [psx-spx: Timers](https://psx-spx.consoledev.net/timers/)
- [psx-spx: DMA](https://psx-spx.consoledev.net/dmachannels/)
- [PS1 PU-22 8MB Mod](https://github.com/hkzlab/PS1_PU22_8MB_mod)

---

*Blackroo Linux Hardware Architecture Reference*
*Based on psx-spx documentation and kernel source analysis*
