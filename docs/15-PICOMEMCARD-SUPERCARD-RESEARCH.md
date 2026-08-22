# PicoMemcard "SuperCard" Research — Multi-Function RP2040 PS1 Peripheral

Investigation date: 2026-04-14
Status: Research complete, design phase

## Problem Statement

Blackroo Linux needs multiple peripherals to function on a real PS1:
1. Memory card storage (SIO0) — for rootfs, swap, persistent data
2. Keyboard input (SIO0) — for typing in the Linux shell
3. Serial console bridge (SIO1) — replacing the FTDI adapter for upload/debug
4. Network access — for SSH into the running PS1 Linux system

Currently these require separate hardware: real memory cards, a BlueRetro ESP32,
an FTDI USB-serial adapter, and no networking at all. This document investigates
building a single RP2040-based device — the "SuperCard" — that handles all four
functions simultaneously.

---

## Finding 1: PicoMemcard Architecture

PicoMemcard (by dangiu) is an open-source RP2040-based PS1 memory card emulator.
Understanding its architecture is critical because it proves SIO0 emulation works
on the RP2040 and provides the base for our multi-function device.

### PIO State Machine Usage

PicoMemcard uses **4 PIO state machines**, all on **PIO0**, consuming
**24 of 32 instruction words**:

| State Machine | Instructions | Function |
|---------------|-------------|----------|
| `sel_monitor` | 4 words | Watches SEL line, fires IRQ when SEL goes high (end of transaction) |
| `cmd_reader` | 10 words | Samples CMD line on rising CLK edge, 8 bits, sends ACK pulse |
| `dat_reader` | 4 words | Samples DAT line on rising CLK, passive sniffer |
| `dat_writer` | 6 words | Outputs bits on DAT via pin direction (open-drain), synced to falling CLK |

### Pin Assignments

| Pico GPIO | PS1 Signal | Function |
|-----------|-----------|----------|
| GPIO 5 | DAT | Controller/card data (bidirectional, open-drain) |
| GPIO 6 | CMD | Command from PS1 (input) |
| GPIO 7 | SEL | Chip select, active-low (input) |
| GPIO 8 | CLK | ~250 kHz SPI clock from PS1 (input) |
| GPIO 9 | ACK | Acknowledge pulse (output, active-low) |

**Constraint:** DAT, CMD, SEL, CLK must be **consecutive GPIOs** due to PIO
addressing requirements.

### Dual-Core Architecture

- **Core 0:** Runs `simulate_memory_card()`, handles IRQ from `sel_monitor`
  (resets state machines when SEL goes high), otherwise idle
- **Core 1:** Runs `simulation_thread()` in tight loop, reads command bytes
  from PIO FIFOs, executes `state_machine_tick()` protocol handler

### USB Mode

Uses TinyUSB for USB Mass Storage (to copy .mcr save files to/from SD card)
and CDC serial. At boot, checks for USB host connection; if none detected
within 3 seconds, switches to memory card emulation mode.

Source: github.com/dangiu/PicoMemcard (MIT license)

---

## Finding 2: SD2PSXtd Architecture (Alternative Reference)

SD2PSXtd is a more advanced RP2040 memory card emulator supporting both
PS1 and PS2 modes. Provides additional architectural reference.

### PIO Usage

Uses **3 PIO state machines** on PIO0 for PS1 mode:
- `cmd_reader` — receives commands from PS1
- `dat_writer` — transmits responses
- `cntrl_reader` — reads controller messages (for PS1/PS2 mode detection)

PIO code is "largely based on PicoMemcard's psxSPI.pio" but with
configurable pin injection rather than hardcoded pins.

### Additional PIO

- `ws2812.pio` for status LED (1 state machine on PIO1)
- `qspi.pio` for PSRAM access (separate state machines on PIO1)

### Hardware

Uses external PSRAM for card image storage, SD card for persistence,
OLED display + buttons for menu navigation, USB-C for firmware updates.

### Dual-Core

Core 1 dedicated to memory card emulation (PIO + protocol handling).
Core 0 runs main application (UI, SD card, USB).

Source: github.com/sd2psXtd/firmware (GPL-2.0 license)
Source: sd2psxtd.github.io

---

## Finding 3: SIO0 Bus Topology — Keyboard and Memory Card Coexistence

### How the PS1 SIO0 Bus Works

The PS1 has a single SPI port shared across 2 controller ports and 2 memory
card slots. Only two chip select lines (DTR/CSn) exist — one per port.
Devices sharing the same DTR line are **wired in parallel** and use an
**address byte** in the data stream to determine which device responds.

### Address Byte Routing

| Address | Device Class |
|---------|-------------|
| 0x01 | Controllers (all types), Keyboard |
| 0x81 | Memory cards |
| 0x21 | Yaroze Access Card, PS2 Multitap |
| 0x61 | PS2 DVD remote receiver |

### Critical Insight: Shared PIO State Machines

A keyboard emulator and memory card emulator can coexist on the **same
physical port** using the **same PIO state machines** because they use
**different address bytes** (0x01 vs 0x81).

The `cmd_reader` PIO captures the first byte from the PS1. Software on
Core 1 then decides:
- **If first byte = 0x81:** Handle as memory card (read/write/ID commands)
- **If first byte = 0x01:** Handle as keyboard (respond with device ID 0x96)

The `dat_writer` sends the appropriate response via the shared DAT line.

This is identical to how real PS1 hardware works — the controller and
memory card share the same wires but respond to different addresses. No
additional PIO state machines are needed.

Source: psx-spx.consoledev.net/controllersandmemorycards/ (device addressing)
Source: problemkaputt.de/psxspx-controllers-keyboards.htm (keyboard ID 0x96)
Source: hackaday.io/project/170365-blueretro/log/186471 (BlueRetro PS1 SPI analysis)

---

## Finding 4: SIO1 Serial Bridge (FTDI Replacement)

### Approach

Use one of the RP2040's **2 hardware UARTs** (UART0/UART1) to bridge PS1
SIO1 to USB CDC serial. This replaces the external FTDI adapter entirely.

### Voltage Compatibility

PS1 SIO1 uses TTL levels (0–3.3V). The RP2040 is natively 3.3V. **No level
shifter needed** — unlike FTDI adapters which typically handle 5V/3.3V
RS-232 level conversion. Direct wiring works.

### Existing Reference Projects

- **pico-uart-bridge** (by Noltari): Bridges both hardware UARTs to USB
  CDC serial ports via TinyUSB. Proven, minimal code.
  Source: github.com/Noltari/pico-uart-bridge

- **rp2040zero-4uart-to-cdc-bridge** (by BartlomiejR): 4 UART ports
  using PIO for additional UARTs beyond the 2 hardware ones.
  Source: github.com/BartlomiejR/rp2040zero-4uart-to-cdc-bridge

### Implementation

- Use hardware UART1 (saves PIO state machines for SIO0)
- PS1 SIO1 TX → RP2040 UART1 RX
- PS1 SIO1 RX → RP2040 UART1 TX
- USB side presents as CDC ACM serial device
- Supports 115200 baud (standard) and 518400 baud (fast mode)

### RTS/DTR Handling

The PS1 SIO1 requires CTS to be asserted for TX to work (Finding 1 from
`docs/13-SIO1-HARDWARE-RESEARCH.md`). The RP2040 can assert RTS/DTR
via GPIO to satisfy this requirement, eliminating the FTDI CTS issue
that blocked our earlier serial work.

### Alternative: PIO UART

If hardware UARTs are needed for other purposes, PIO1 can implement
UART TX and RX using 2 state machines (~6 instruction words each).

---

## Finding 5: USB Composite Device

### TinyUSB Capabilities

TinyUSB (the USB stack used by Pico SDK and PicoMemcard) supports
**composite USB devices** — presenting as multiple device classes
simultaneously over a single USB-C port.

### Proposed USB Descriptor

| Interface | Class | Purpose |
|-----------|-------|---------|
| CDC ACM #0 | Serial | PS1 SIO1 serial bridge (console/upload/debug) |
| RNDIS/ECM | Network | IP network adapter (for SSH access) |
| MSC | Storage | Memory card image management (optional, on-demand) |

All three can coexist in a single composite descriptor.

Source: github.com/hathach/tinyusb (MIT license)
Source: github.com/hathach/tinyusb/discussions/742 (RNDIS/ECM support)

---

## Finding 6: Network Access — Options

### Option A: USB RNDIS/ECM (Wired Network via USB Cable)

The RP2040 can present as a USB network adapter using TinyUSB's RNDIS
(Windows+Linux) or CDC-ECM (Linux+macOS) class drivers, with lwIP
providing the TCP/IP stack on the RP2040 side.

- TinyUSB has a working `net_lwip_webserver` example demonstrating
  DHCP, DNS, and HTTP servers running entirely on the RP2040
- Can be combined with CDC serial in a composite device
- The RP2040 acts as a network gateway between the PS1 and the host PC

Source: github.com/hathach/tinyusb/blob/master/examples/device/net_lwip_webserver/
Source: github.com/mattmyne/usbnet (minimal working RNDIS example)
Source: forums.raspberrypi.com/viewtopic.php?t=378086 (Pico RNDIS thread)

### Option B: WiFi Bridge (Pico W)

The **Raspberry Pi Pico W** variant includes a CYW43439 WiFi chip
connected via **internal SPI** — it does NOT consume any user-accessible
GPIO pins. All 26 user GPIOs remain available for PS1 signals.

- **PicoPiFi** project demonstrates Pico W as a driverless USB RNDIS
  WiFi dongle: USB 12 Mbps RNDIS link, 54 Mbps WiFi to access point
- Could provide WiFi-based SSH access to PS1 Linux without any wired
  Ethernet infrastructure
- The PS1 would appear as a device on the local WiFi network

Source: github.com/sidd-kishan/PicoPiFi (Pico W WiFi dongle project)

### Option C: SLIP Over SIO1 (Simplest Kernel-Side)

SLIP (Serial Line Internet Protocol) is the simplest network path from
the PS1 kernel's perspective:

1. PS1 SIO1 sends SLIP-encoded IP packets over serial at 115200 baud
2. RP2040 receives bytes via UART, decodes SLIP, feeds to lwIP stack
3. lwIP routes packets to USB RNDIS or WiFi (on Pico W)
4. PS1 appears as a device on the host's network

**Performance:** 115200 baud = ~11.5 KB/s theoretical max. Sufficient
for SSH text sessions, scp of small files. Not usable for anything
bandwidth-heavy.

**Kernel support:** The Blackroo kernel already includes standard Linux
SLIP driver (`drivers/net/slip.c`) and PPP stack (`drivers/net/ppp_*.c`).
The PS1 serial driver (`drivers/char/serial_psx.c`) needs fixes for
proper buffered I/O and termios support.

### Option D: Custom SIO0 Network Commands

Extend the PicoMemcard SIO0 protocol with custom commands for network I/O,
similar to the serial commands proposed in `docs/11-PICOMEMCARD-DUAL-MODE.md`:

| Command | Direction | Function |
|---------|-----------|----------|
| 0x5B | PS1 → Pico | Network packet out (64-byte chunks) |
| 0x5C | Pico → PS1 | Network packet in (64-byte chunks) |
| 0x5D | PS1 → Pico | Network status (bytes waiting, link state) |

**Advantage:** Faster than SIO1 SLIP (~22 KB/s via SIO0 vs ~11.5 KB/s via SIO1).
**Disadvantage:** Requires a custom kernel network driver for the SIO0 transport.

### Recommended: Option C (SLIP) First, Option D Later

SLIP requires minimal kernel work (fix existing serial driver, use existing
SLIP module) and the RP2040 handles all the networking. Option D is faster
but needs a custom kernel driver — save it for later optimization.

Source: github.com/CodeAsm/PS1Linux (original Runix project, serial driver)
Source: Linux kernel 2.4 SLIP documentation

---

## Finding 7: Existing Blackroo Codebase Support

### PicoMemcard Design Document

A complete design document already exists at `docs/11-PICOMEMCARD-DUAL-MODE.md`
describing a dual-mode PicoMemcard with:

- Standard memory card emulation (SIO0, commands 0x52/0x57/0x53)
- Custom serial commands over SIO0 (0x58 serial write, 0x59 serial read,
  0x5A serial status)
- Core 0: SIO0 emulation via PIO0
- Core 1: USB CDC serial bridge
- USB composite: CDC (virtual serial) + MSC (SD card)
- ~22 KB/s effective serial throughput over SIO0

### Bootloader Memory Card Code

`bootloader/src/memcard.c` (532 lines) implements the full SIO0 memory card
protocol from the PS1 side. Key register definitions:

```
SIO0_DATA:  0x1F801040  (TX/RX data)
SIO0_STAT:  0x1F801044  (status flags)
SIO0_MODE:  0x1F801048  (mode config — 8-bit, x1 baud)
SIO0_CTRL:  0x1F80104A  (control — DTR, port select bit 13)
SIO0_BAUD:  0x1F80104E  (baud rate — 0x0088 = ~250kHz)
```

### Kernel Memory Card Driver

`drivers/block/bu.c` (800+ lines) implements an interrupt-driven FSM with
13+ state handlers for memory card read/write. This shows the exact byte
sequences the kernel sends, which the RP2040 must respond to.

### Kernel Keyboard Stubs

`arch/mipsnommu/ps/psx_kbd.c` contains empty stubs (`psx_kbd_read_input`,
`psx_kbd_write_output`, `psx_kbd_read_status`, etc.) registered via
`psxsiokbd_ops`. The framework exists — needs real SIO0 polling +
scancode parsing implementation.

### Kernel Serial Driver

`drivers/char/serial_psx.c` exists but is incomplete — most termios code
is commented out, buffer management is unused. Needs fixing for SLIP
networking to work.

---

## PIO Resource Budget

The RP2040 has **2 PIO blocks x 4 state machines = 8 total**, each block
with **32 instruction words**.

### Proposed Allocation

| PIO Block | SM | Function | Est. Instructions |
|-----------|----|--------------------|-------------------|
| PIO0 | SM0 | `sel_monitor` (SEL watcher, IRQ) | 4 words |
| PIO0 | SM1 | `cmd_reader` (CMD input + ACK) | 10 words |
| PIO0 | SM2 | `dat_writer` (DAT output, open-drain) | 6 words |
| PIO0 | SM3 | `dat_reader` (DAT input sniffer) | 4 words |
| PIO1 | SM0 | UART TX (SIO1 serial bridge)* | ~6 words |
| PIO1 | SM1 | UART RX (SIO1 serial bridge)* | ~6 words |
| PIO1 | SM2 | WS2812 status LED (optional) | ~4 words |
| PIO1 | SM3 | *spare* | — |

**PIO0:** 24/32 words — SIO0 bus (memory card + keyboard share same SMs)
**PIO1:** ~16/32 words — SIO1 UART + LED, 1 spare SM

*If using hardware UART instead of PIO UART, PIO1 SM0 and SM1 are freed.

### Core Allocation

| Core | Primary Function | Secondary |
|------|-----------------|-----------|
| Core 0 | SIO0 PIO management + IRQ handling | USB stack (TinyUSB) |
| Core 1 | SIO0 protocol handler (memcard + keyboard) | UART/network forwarding |

---

## Proposed Hardware Design

### Board: Raspberry Pi Pico W

The Pico W variant is recommended because:
- WiFi (CYW43439) at no GPIO cost — connects via internal SPI
- Same RP2040 chip, same GPIO layout, same price (~$6)
- Enables wireless SSH without additional hardware

### GPIO Pin Assignment

| GPIO | Function | PS1 Signal | Notes |
|------|----------|-----------|-------|
| GP5 | PIO0 IN/OUT | SIO0 DAT | Open-drain, pull-up needed |
| GP6 | PIO0 IN | SIO0 CMD | Input from PS1 |
| GP7 | PIO0 IN | SIO0 SEL | Active-low chip select |
| GP8 | PIO0 IN | SIO0 CLK | ~250 kHz clock from PS1 |
| GP9 | PIO0 OUT | SIO0 ACK | Active-low acknowledge |
| GP16 | UART0 TX | SIO1 RXD (Pin 5) | PS1 serial data in |
| GP17 | UART0 RX | SIO1 TXD (Pin 8) | PS1 serial data out |
| GP18 | GPIO OUT | SIO1 CTS→RTS | Assert RTS so PS1 sees CTS |
| GP19 | GPIO OUT | SIO1 DSR→DTR | Assert DTR so PS1 sees DSR |
| GP25 | — | — | Pico onboard LED (WL_GPIO0 on Pico W) |
| GP26-28 | ADC | — | Available for future use |

### PS1 Connections

**Controller Port (SIO0) — 5 signal wires:**
```
PS1 Controller Port          Pico W
Pin 1 (DATA) ──────────────── GP5
Pin 2 (CMD)  ──────────────── GP6
Pin 4 (GND)  ──────────────── GND
Pin 5 (VCC)  ──────────────── 3V3 (or VSYS via LDO)
Pin 6 (SEL)  ──────────────── GP7
Pin 7 (CLK)  ──────────────── GP8
Pin 9 (ACK)  ──────────────── GP9
```

**Serial Port (SIO1) — 4 wires:**
```
PS1 SIO1 Port                Pico W
Pin 2 (GND)  ──────────────── GND
Pin 5 (RXD)  ──────────────── GP16 (UART0 TX)
Pin 8 (TXD)  ──────────────── GP17 (UART0 RX)
              (RTS/CTS)        GP18 (assert high)
```

**USB:** Single USB-C port on Pico W → host PC
Composite device: CDC Serial + RNDIS Network (+ optional MSC Storage)

---

## Feasibility Summary

| Function | Feasibility | Resources Needed | Complexity |
|----------|------------|-----------------|------------|
| Memory Card Emulator (SIO0) | **Proven** | PIO0: 4 SMs, 24 words | Low — fork PicoMemcard |
| Keyboard Emulator (SIO0) | **High** | Same PIO0 SMs + software | Medium — add 0x01 handler |
| Serial Bridge (SIO1→USB) | **Proven** | 1 HW UART or PIO1: 2 SMs | Low — pico-uart-bridge |
| USB Composite Device | **Proven** | USB peripheral | Medium — TinyUSB config |
| USB RNDIS/ECM Network | **Proven** | TinyUSB + lwIP | Medium |
| WiFi Bridge (Pico W) | **Proven** | CYW43439 (internal) | Medium-High |
| SLIP Gateway on RP2040 | **Feasible** | lwIP + UART | High — custom integration |
| PS1 Linux SLIP Networking | **Feasible** | Fix serial_psx.c | High — kernel work |

**Overall verdict:** All four functions can coexist on a single RP2040.
The PIO budget is sufficient. The USB composite device support in TinyUSB
allows serial + network + storage simultaneously. The Pico W adds WiFi
at no GPIO cost.

---

## Implementation Phases

### Phase 1: Stock PicoMemcard on Pico W
Fork PicoMemcard, build for Pico W, verify memory card emulation works
on real PS1 hardware.

### Phase 2: Add Keyboard Handler
Extend the SIO0 protocol handler to respond to address byte 0x01 with
Lightspan keyboard protocol (device ID 0x96). Add USB HID keyboard
input or PS/2 keyboard input on spare GPIOs.

### Phase 3: Add SIO1 Serial Bridge
Wire UART0 to PS1 SIO1 pins. Add USB CDC serial interface to composite
descriptor. Replace FTDI adapter entirely.

### Phase 4: USB Composite Device
Combine CDC serial + RNDIS/ECM network + MSC storage in single USB
descriptor using TinyUSB.

### Phase 5: SLIP Networking
Fix PS1 kernel serial driver. Configure SLIP on PS1 side. RP2040
decodes SLIP, routes via lwIP to USB RNDIS or WiFi.

### Phase 6: WiFi (Pico W)
Enable CYW43439 WiFi. RP2040 acts as WiFi gateway — PS1 appears on
local network. SSH directly to PS1 over WiFi.

### Phase 7: Custom SIO0 Network Driver (Optional)
Replace SLIP-over-SIO1 with custom SIO0 network commands (0x5B/0x5C/0x5D)
for ~2x throughput improvement.

---

## Sources

### PicoMemcard & SD2PSXtd
- PicoMemcard: github.com/dangiu/PicoMemcard (MIT license)
- SD2PSXtd firmware: github.com/sd2psXtd/firmware (GPL-2.0 license)
- SD2PSXtd documentation: sd2psxtd.github.io

### BlueRetro (Keyboard Reference)
- BlueRetro: github.com/darthcloud/BlueRetro (Apache-2.0 license)
- BlueRetro PSX Lightspan keyboard log: hackaday.io/project/170365-blueretro/log/186816
- BlueRetro PS1 SPI interface analysis: hackaday.io/project/170365-blueretro/log/186471

### PS1 Hardware Documentation
- psx-spx Controllers and Memory Cards: psx-spx.consoledev.net/controllersandmemorycards/
- psx-spx Controllers - Keyboards: problemkaputt.de/psxspx-controllers-keyboards.htm
- psx-spx Serial Interfaces (SIO): psx-spx.consoledev.net/serialinterfacessio/
- GameSX PSX Controllers: gamesx.com/controldata/psxcont/psxcont.htm

### RP2040 Serial Bridge
- pico-uart-bridge: github.com/Noltari/pico-uart-bridge
- rp2040zero-4uart-to-cdc-bridge: github.com/BartlomiejR/rp2040zero-4uart-to-cdc-bridge

### USB Networking
- TinyUSB: github.com/hathach/tinyusb (MIT license)
- TinyUSB RNDIS/ECM discussion: github.com/hathach/tinyusb/discussions/742
- TinyUSB net_lwip_webserver example: github.com/hathach/tinyusb/blob/master/examples/device/net_lwip_webserver/
- Pico RNDIS forum thread: forums.raspberrypi.com/viewtopic.php?t=378086
- usbnet minimal example: github.com/mattmyne/usbnet

### WiFi
- PicoPiFi (Pico W WiFi dongle): github.com/sidd-kishan/PicoPiFi

### PS1 Linux / Runix
- PS1Linux original project: github.com/CodeAsm/PS1Linux

### PS/2 Keyboard Protocol
- OSDev PS/2 Keyboard: wiki.osdev.org/PS/2_Keyboard
- SCPH-2000 reverse engineering: obscuregamers.com/threads/scph-2000-ps-2-keyboard-mouse-adapter-reverse-engineering.3048/
