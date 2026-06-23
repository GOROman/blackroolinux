# Cart-Resident Boot Research — Booting kloader from the Expansion Port

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: UniROM as the upload path.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> How to make the PlayStation boot the Blackroo kloader **from a GameShark /
> Action Replay cartridge at power-on**, instead of uploading it into RAM via an
> existing exploit.

Investigation date: 2026-06-23
Status: Research — design not yet implemented

---

## 1. Why this is different from what we have today

Today the kloader reaches the console as a **PS-EXE loaded into RAM**. Something
else has already run first — UniROM on a cheat cart, or the FreePSXBoot
memory-card exploit — and that thing copies our PS-EXE to `0x80010000` and jumps
to it (see `docs/06-BOOTLOADER-DESIGN.md`, Methods A/B).

Our `pioflash.c` can already **detect, dump, and program** the NOR flash on a
GameShark/Action Replay/Xplorer/Caetla cart over the PIO port at `0x1F000000`.
But it treats that flash purely as **data storage** — it never makes the console
*boot from it*.

The goal of this research: replace the borrowed bootstrap with **our own
cart-resident image**, so a Blackroo cart boots straight to the kloader menu at
power-on with no memory-card exploit and no host PC.

```
        BEFORE (today)                          AFTER (this research)
  Power on → BIOS                          Power on → BIOS
     → UniROM/FreePSXBoot (someone        → BIOS scans EXP1 pre-boot header
        else's bootstrap)                    → finds OUR header on the cart
        → loads our PS-EXE to RAM            → jumps into OUR stub
           → kloader menu                       → stub loads kloader → menu
```

---

## 2. The mechanism: BIOS Pre-Boot Expansion ROM scan

The PS1 boot ROM (`0xBFC00000`) runs a **pre-boot expansion-ROM check** very
early in reset, *before* the intro/shell and before the kernel's `A0/B0/C0`
syscall vectors are set up. This is the same hook UniROM, Caetla, the original
Action Replay, and Lameguy64's open-source **n00brom** all use.

**What the BIOS checks (EXP1 base = `0x1F000000`):**

| Address | BIOS expects | Purpose |
|---------|--------------|---------|
| `0x1F000084` | ASCII `"Licensed by Sony Computer Entertainment Inc."` | signature that says "a bootable expansion ROM is present" |
| `0x1F000080` | a callable entry (BIOS does a `jal`/`jr` here) | the pre-boot entry point the BIOS jumps into when the signature matches |

> **VERIFY exact header layout before writing it.** The signature/entry pair
> above is the documented mechanism, but the precise field offsets, the exact
> bytes at `0x80` (jump opcode vs. function pointer), and how much of the
> `0x80..0xFF` window is reserved must be copied **verbatim from a known-good
> implementation** — use **n00brom** as the reference (`src/` expansion ROM
> header), and cross-check against the nocash "Cheat Devices" / "Kernel (BIOS)"
> pages. Do not hand-roll these bytes from memory.

**Critical constraint — the BIOS is barely initialised at this point:**
When the pre-boot vector is called, the **`A0h/B0h/C0h` syscall vectors are not
yet installed**, so the cart code **cannot call BIOS functions** (no
`printf`, no controller, no card, no `LoadExec`). Caetla handles this by
**checksumming `0xBFC06000..0xBFC07FFF` to identify the BIOS version**, then
using hard-coded per-version BIOS addresses. Our stub has the same problem and
needs the same discipline: assume nothing about kernel services on entry.

---

## 3. Hardware reality of the cart bus

> **CORRECTED 2026-08-22 — see `docs/21-PIO-PORT-REFERENCE.md`.** Two claims below
> are wrong: the bus is **16-bit** (D0-D15), not 8-bit (8-bit is only the default
> *mode*, bit 12 of `0x1F801008`); and the connector supplies **3.3 V** and 8 V,
> not 5 V. Doc 21 also has the full 68-pin pinout, the ~80 ns read window, DMA
> channel 5 (DREQ5/DACK5), an I2S audio input, and the **second** boot hook at
> `0x1F000000`/`0x1F000004` that this document misses.

From the pcsx-redux PIO-port notes and the n00brom hardware docs:

- A minimal working cart is just a ROM chip wired to address lines **A0–Axx**,
  data lines **D0–D7** (the PIO bus is **8-bit**), and control lines **!RD** and
  **!CS0**. That matches our `pioflash.c`, which already does byte-wide access at
  `0x1F000000`.
- "Real" carts (GameShark/AR) add a CPLD/PLA to multiplex `!CS0` and subdivide
  the address space (flash + SRAM + control registers all behind one chip
  select). **Programming and booting larger images may require bank-switching**
  through that logic, which is cart-model-specific — our current flasher assumes
  a flat, directly-mapped chip.
- EXP1 bus timing/width is set by the Memory Control registers
  **`0x1F801000`** (Expansion 1 Base Address) and **`0x1F801008`** (Expansion 1
  Delay/Size). The BIOS programs sane defaults at reset; we should not need to
  touch these for boot, but they matter if we widen/speed up flash access.

---

## 4. Does it fit? (the size question)

Yes, with room to spare.

| Thing | Size | Note |
|-------|------|------|
| kloader PS-EXE (built 2026-06-23) | **58 KB** | `0x8001A178` entry, loads at `0x80010000` |
| Smallest cart flash we support | **128 KB** | SST39SF010 / AM29F010 (`pioflash.c` chip DB) |
| Typical GameShark/AR flash | 128–512 KB | up to 1 MB (AM29F080) in our DB |

A cart-resident image is **[EXP1 header + small stub] + [the kloader payload]**.
The header+stub is a few hundred bytes; the payload is our existing ~58 KB. Even
the smallest 128 KB cart holds the whole thing with >half the flash free —
enough to also stash settings or a compressed kernel later.

---

## 5. What needs to be built (design sketch, not yet implemented)

1. **EXP1 pre-boot header + stub** (new, tiny, position-dependent at
   `0x1F000080`):
   - the `"Licensed by Sony…"` signature at `0x1F000084`
   - an entry at `0x1F000080` that runs with **no BIOS services**
   - copies the kloader payload from cart flash into RAM at `0x80010000`
   - flushes I-cache and jumps to the kloader entry
   - (optional, Caetla-style) BIOS-version checksum if it ever needs a BIOS call
2. **Cart-resident build target** — a new linker layout + build step that emits a
   **flashable `.bin`** (header + stub + payload), *not* a PS-EXE. The current
   `bootloader/build.sh` only produces the PS-EXE.
3. **Self-install path** — wire that `.bin` into the existing
   `pioflash_program()` so a running kloader can **flash itself onto the cart**
   ("Install to cart" mode in the PIO Flash Manager). The write/erase/verify
   primitives already exist in `pioflash.c`.
4. **Brick-safety** — the cart currently holds the user's GameShark firmware.
   Before this is usable we need: read-back/verify (`pioflash_verify`, already
   present), a "dump original first" prompt, and ideally a recovery path.

---

## 6. Open questions to resolve before coding

- **Exact header bytes** at `0x1F000080`/`0x84` — copy from n00brom, do not guess.
- **Which cart models** map flash flat vs. behind a CPLD bank-switch (affects
  whether our flat `pioflash` write path even reaches all sectors).
- **Boot order** — does our signature coexist with a still-installed UniROM, or
  must the cart be dedicated? (Two pre-boot ROMs on one bus = conflict.)
- **Write-protect / !WE wiring** — some carts gate flash writes behind a jumper
  or CPLD register; programming may need that unlocked first.

---

## 7. Sources

- **n00brom** — open-source Caetla equivalent for PS1 carts (reference for the
  pre-boot header + stub): https://github.com/Lameguy64/n00brom
- **pcsx-redux — PIO port wiki** (expansion ROM format, bus wiring):
  https://github.com/grumpycoders/pcsx-redux/wiki/PIO-port
- **nocash psx-spx — Cheat Devices** (pre-boot vector, Caetla BIOS-version
  checksum, `A0/B0/C0` uninitialised at pre-boot):
  https://www.problemkaputt.de/psxspx-cheat-devices.htm
- **nocash psx-spx — Kernel (BIOS)** (boot sequence context):
  https://psx-spx.consoledev.net/kernelbios/ (mirror: problemkaputt.de)
- **UniROM** (working cart firmware, install reference):
  https://unirom.github.io/installation/
- **Our own `bootloader/src/pioflash.c` / `pioflash.h`** — existing flash
  detect/dump/erase/program/verify path and chip database.

> Per project convention: the exact pre-boot header bytes are flagged VERIFY and
> must be lifted from the n00brom source at implementation time, not transcribed
> from this document.
