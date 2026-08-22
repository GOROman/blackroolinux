# PIO / Parallel Expansion Port — Hardware Reference

> Source: pcsx-redux wiki, "PIO port"
> <https://github.com/grumpycoders/pcsx-redux/wiki/PIO-port>
> Retrieved 2026-08-22. Facts below are from that page unless marked otherwise.
>
> **This supersedes the hardware notes in `docs/18-CART-RESIDENT-BOOT-RESEARCH.md` §3,
> which were wrong on two counts — see §7.**

---

## 1. Why this matters to us

Two live projects depend on this port:

- **blackroo-linux** — cart-resident boot (`docs/18`), and flash as XIP storage for a
  root filesystem instead of burning RAM on a ramdisk.
- **psx-video / "PlayFlix"** — the high-bandwidth transport option for streaming
  video into the console from a microcontroller.

---

## 2. Physical

- **68-pin** proprietary rectangular connector, derived from the VHDCI SCSI design.
- Present on essentially all original PSX models **except SCPH-900x**. (Separately:
  the PSone has no expansion port at all — so anything built here is not
  reproducible on a PSone.)
- **No side keys** — the connector can be inserted reversed. Walls are thin and
  crack if the cart is levered while unplugging. Treat used carts as fragile.

## 3. Bus

| Property | Value |
|---|---|
| Data bus | **16-bit** (D0–D15) |
| Address bus | **24-bit** (A0–A23) |
| Clock | 33.8688 MHz (same as the CPU) |
| Supply rails on the connector | **3.3 V and 8 V** |
| Default window | 512 KB at `0x1F000000` (also `0x9F000000` / `0xBF000000`) |
| Max window | **8 MB**, via the size field in `0x1F801008` |

### Pinout

Pins run in two columns; the table pairs them as the source presents them.

| Signal | Pin | | Pin | Signal |
|---|---|---|---|---|
| GND | 1 | | 35 | GND |
| !RESET | 2 | | 36 | DACK5 |
| DREQ5 | 3 | | 37 | !INT10 |
| !CS0 | 4 | | 38 | !SWR1 |
| SBEN | 5 | | 39 | !CS2 |
| D0 | 6 | | 40 | D1 |
| D2 | 7 | | 41 | D3 |
| D4 | 8 | | 42 | D5 |
| D6 | 9 | | 43 | D7 |
| D8 | 10 | | 44 | D9 |
| D10 | 11 | | 45 | D11 |
| D12 | 12 | | 46 | D13 |
| D14 | 13 | | 47 | D15 |
| A0 | 14 | | 48 | A1 |
| A2 | 15 | | 49 | A3 |
| GND | 16 | | 50 | GND |
| 3.3 V | 17 | | 51 | 3.3 V |
| 8 V | 18 | | 52 | 8 V |
| GND | 19 | | 53 | GND |
| A4 | 20 | | 54 | A5 |
| A6 | 21 | | 55 | A7 |
| A8 | 22 | | 56 | A9 |
| A10 | 23 | | 57 | A11 |
| A12 | 24 | | 58 | A13 |
| A14 | 25 | | 59 | A15 |
| A16 | 26 | | 60 | A17 |
| A18 | 27 | | 61 | A19 |
| A20 | 28 | | 62 | A21 |
| A22 | 29 | | 63 | A23 |
| !SRD | 30 | | 64 | !SWR0 |
| !IN2 | 31 | | 65 | !CS5 |
| SYSCLK1 | 32 | | 66 | LRCLK |
| BCLK | 33 | | 67 | SDIN |
| GND | 34 | | 68 | GND |

Control lines: **!CS0** chip select (PIO only), **!SRD** read strobe,
**!SWR0/!SWR1** write strobes, **!INT10** interrupt, **!RESET** open-collector
(grounding it forces a console reset). SBEN and !CS2 are weak grounds and may be
cut and reused; !IN2 and !CS5 are not connected and are commonly repurposed.

## 4. Timing — register `0x1F801008`

Default value `0x0013243F`.

| Bits | Function | Default |
|---|---|---|
| 0–3 | Write delay (cycles − 1) | 15 (**optimal ~2 ≈ 80 ns**) |
| 4–7 | Read delay (cycles − 1) | 3 (**optimal ~2 ≈ 80 ns**) |
| 8 | Recovery period | off |
| 9 | Hold period | off |
| 10 | Float period | on |
| 11 | Pre-strobe period | off |
| 12 | Access type: 0 = 8-bit, 1 = 16-bit | 8-bit |
| 13 | Address increment | on (off splits 16/32-bit into 8-bit ops) |
| 14 | unknown | |
| 15 | Extended delay | off |
| 16+ | Bus width in bits | 19 → 512 KB; **max 23 → 8 MB** |

`0x1F801000` holds the base address (default `0x1F000000`).

**Key number: a read cycle can be tuned to roughly 80 ns.** That is the window any
device on this bus must answer within.

## 5. DMA — channel 5

**DREQ5 / DACK5 are on the connector.** PS1 DMA channel 5 is the PIO channel, so
transfers from a cart do **not** have to be CPU-driven byte loads. This matters
enormously for streaming: it means the console can pull data without stealing
cycles from the MDEC VLC decompressor, which is already the expensive part of
video playback.

## 6. I²S audio input — pins 66/67/33

Three pins form a **44.1 kHz, 16-bit stereo I²S audio input**:

- **LRCLK** (66) — left/right clock, **driven by the console**
- **BCLK** (33) — bit clock, **driven by the console**
- **SDIN** (67) — serial data, **provided by the cart**

A cart can therefore feed digital audio straight into the console's audio path,
bypassing the SPU and XA entirely. For a streaming player this is significant:
audio costs **zero** of the data budget and **zero** CPU, at better quality than
XA-ADPCM manages. The clocks being console-driven means the external device is a
slave and does not have to generate accurate timing.

## 7. Corrections to `docs/18-CART-RESIDENT-BOOT-RESEARCH.md`

| `docs/18` §3 said | Actually |
|---|---|
| "the PIO bus is **8-bit**" | **16-bit** (D0–D15). 8-bit is merely the *default mode* — bit 12 of `0x1F801008` selects 16-bit. |
| (implied 5 V, from the SST39SF/AM29F chips in `pioflash.c`) | The connector supplies **3.3 V** and 8 V. Signal levels are 3.3 V — **verify with a meter before connecting anything**, but this suggests an RP2040 or ESP32 can attach without level shifters. |

`docs/18` also lists only one boot hook. There are **two**:

| Stage | Signature string checked at | Entry executed |
|---|---|---|
| Early, right after the BIOS crt0 | `0x1F000084` | `0x1F000080` |
| Later, before loading disc content | `0x1F000004` | `0x1F000000` |

Both look for "Licensed by Sony Computer Entertainment Inc.". Two hook points means
a cart can inject code at two different stages of boot — relevant to the
kloader-from-cart design.

## 8. Consequences for our builds

**Storage (blackroo-linux).** The window is 512 KB by default and can be widened to
8 MB in software. That is the ceiling for a cart-resident XIP root filesystem — far
more useful than the 2 MB of system RAM it would otherwise consume.

**Streaming (psx-video).** Every obstacle identified earlier is smaller than thought:

- 3.3 V signalling → likely no level shifters
- ~80 ns read window → an RP2040 PIO state machine answers in ~16–24 ns at 125 MHz,
  comfortably inside it, with no external FIFO, dual-port SRAM or FPGA needed
- DMA channel 5 → console-side transfer need not burn CPU
- 16-bit data → double the throughput of the 8-bit assumption
- I²S input → audio for free, at 44.1 kHz 16-bit stereo

**Minimal cart.** A ROM wired to A0–Axx, D0–D7, !SRD and !CS0 is a working cart.
Commercial cheat carts add a PLA/CPLD to multiplex !CS0 across several address
ranges, which is model-specific.

---

## Sources

- pcsx-redux wiki, PIO port — <https://github.com/grumpycoders/pcsx-redux/wiki/PIO-port>
- Cross-reference: `docs/18-CART-RESIDENT-BOOT-RESEARCH.md`, `bootloader/src/pioflash.c`
- psx-spx, Memory Control — <https://psx-spx.consoledev.net/memorycontrol/>
