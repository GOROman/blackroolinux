# PS1 Keyboard Research — Controller Port Keyboard Input

Investigation date: 2026-04-14
Status: Research complete, no implementation yet

## Problem Statement

Blackroo Linux needs keyboard input for the shell once booted. Currently the
only input method is the serial console over SIO1. A keyboard connected through
the PS1 controller port would provide a much better user experience. This
document investigates what keyboard hardware exists, the SIO0 protocol for
keyboards, and what would be needed to implement it.

---

## Background: PS1 SIO0 Controller Protocol

The PS1 uses SIO0 (a synchronous SPI-like serial interface at ~250 kHz) to
communicate with controllers and memory cards. Key parameters:

- **Registers:** SIO0 at 0x1F801040 (same bus as memory cards)
- **Baud rate:** ~250 kHz (SIO0_BAUD = 0x0088)
- **SPI mode:** CPOL=1 (clock high when idle), CPHA=1 (data sampled on rising)
- **Data format:** 8-bit, LSB first
- **Bus topology:** Shared bus with open-drain DATA and ACK lines; dedicated ATT (/CS) per port
- **ACK timing:** Device must pull /ACK low for at least 2us within 100us of last SCK pulse

**Device addressing** (first byte sent selects device class):

| Address | Device |
|---------|--------|
| 0x01 | Controllers |
| 0x81 | Memory Cards |
| 0x21 | Yaroze Access Card |
| 0x61 | PS2 DVD Remote |

**Standard polling sequence** (read command 0x42):

| Byte # | Console sends (CMD) | Device replies (DATA) |
|--------|--------------------|-----------------------|
| 1 | 0x01 (address) | Hi-Z |
| 2 | 0x42 (read cmd) | Device ID low byte |
| 3 | 0x00 | 0x5A (ready marker) |
| 4+ | 0x00... | Payload data |

**Device Type IDs** (ID low byte, combined as 0x5Axx):

| ID | Device |
|----|--------|
| 0x12 | Mouse |
| 0x23 | NegCon |
| 0x41 | Digital Pad |
| 0x53 | Analog Stick (flight mode, green LED) |
| 0x73 | Analog Pad (red LED) |
| 0x79 | DualShock2 |
| 0x80 | Multitap |
| **0x96** | **Keyboard (Lightspan)** |
| 0xE3 | Jogcon |
| **0xE8** | **Keyboard/Sticks adaptor (homebrew)** |
| 0xF3 | Config Mode |
| 0xFFFF | No controller (floating Hi-Z) |

Source: psx-spx.consoledev.net/controllersandmemorycards/

---

## Finding 1: Known PS1 Keyboard Hardware

There was **no official retail keyboard** for the PS1. However, several
obscure peripherals existed:

### a) Sony SCPH-2000 PS/2 Keyboard/Mouse Adaptor (prototype)

- Two variants known: short-cable (prototype marked) and direct-plug (final?)
- Internal chip: 44-pin Motorola SC440881
- **Extremely rare** — only a handful known to exist
- Protocol: same as Lightspan keyboard (device ID 0x96)
- Uses PS/2 Scan Code Set 2 format
- No mouse support observed in protocol traces

Source: psx-spx Controllers-Keyboards section
Source: obscuregamers.com SCPH-2000 reverse engineering thread

### b) Lightspan Online Connection CD Keyboard (1997)

- Used with Lightspan educational web browser disc (extremely rare)
- Disc required dial-up modem on serial port + keyboard adapter
- Device ID: 0x96 (keyboard type, 6 halfwords = 12 bytes payload)
- This is the "standard" PS1 keyboard protocol

### c) Spectrum Emulator Keyboard & Sega Sticks Adaptor v2 (2000)

- Made by Anthony Ball (sinistersoft.com/psxkeyboard)
- Device ID: 0xE8 in keyboard+sticks mode, 0x41 in pad-only mode
- Translates PS/2 scancodes to custom ASCII-style mapping (NOT raw Set 2)
- **Known bug:** Responds to ANY 0x01 byte on bus, not just first byte,
  causing conflicts with memory cards

### d) Homebrew PS/2 Keyboard/Mouse Adaptor (PSone era)

- Made by Simon Armstrong
- Device ID: 0x12 (masquerades as standard Sony Mouse)
- Cleverly encodes keyboard data in unused mouse bits
- Works with any mouse-compatible PS1 game
- Simple protocol: 7 bytes total per poll

### e) Spectrum Emulator Keyboard Adaptor v1 (serial port)

- Connected to SIO1 (serial port), NOT controller port
- 19200 baud, 8N2 format
- CTS line = Caps Lock state, DSR = Num Lock state
- Not relevant for controller port approach, but shows SIO1 alternative

### f) Runix USB Keyboard/Mouse Adaptor (2001)

- Connected via PIO expansion port (parallel I/O), NOT controller port
- Used SL811H USB host controller IC
- Part of the original PSXLinux (Runix) project
- Our PIO expansion port cartridge could potentially host this

Source: psx-spx Controllers-Keyboards section
Source: github.com/CodeAsm/PS1Linux

---

## Finding 2: Lightspan Keyboard Protocol (Device ID 0x96)

This is the most relevant protocol for Blackroo Linux — it's the closest
thing to a "standard" PS1 keyboard device.

### Polling Sequence

```
Console sends:  01h 42h 00h 00h 00h 00h 00h 00h 00h 00h 00h 00h 00h 00h 06h
Device replies: HiZ 96h 5Ah num dat dat dat dat dat dat dat dat dat dat dat
```

- Byte 1: 0x01 (controller address)
- Byte 2: 0x42 (read) → Response: 0x96 (keyboard device ID)
- Byte 3: 0x00 → Response: 0x5A (ready marker)
- Byte 4: → Response: `num` — number of valid scancode bytes (0x00–0x0B, or 0xFF = no keyboard)
- Bytes 5–15: → Response: Raw PS/2 Scan Code Set 2 data

### PS/2 Scan Code Set 2 Format

- **Make code** (key press): single byte for most keys, or 0xE0 prefix + byte for extended keys
- **Break code** (key release): 0xF0 + make code, or 0xE0 0xF0 + byte for extended keys
- Multiple scancodes can appear in a single frame (up to 11 bytes)
- Unidirectional: keyboard to console only (no LED control observed)

### Common Scan Codes (PS/2 Set 2)

| Key | Make Code | Key | Make Code |
|-----|-----------|-----|-----------|
| A | 1C | Enter | 5A |
| B | 32 | Space | 29 |
| C | 21 | Backspace | 66 |
| D | 23 | Escape | 76 |
| E | 24 | Tab | 0D |
| F | 2B | L-Shift | 12 |
| Z | 1A | R-Shift | 59 |
| 0 | 45 | L-Ctrl | 14 |
| 1 | 16 | Up | E0,75 |
| 9 | 46 | Down | E0,72 |
| - | 4E | Left | E0,6B |
| = | 55 | Right | E0,74 |

Source: psx-spx Controllers-Keyboards section
Source: wiki.osdev.org/PS/2_Keyboard (Scan Code Set 2 reference)

---

## Finding 3: Homebrew Mouse-Keyboard Protocol (Device ID 0x12)

A simpler alternative that encodes one scancode + mouse data per poll
in only 7 bytes:

```
Console sends:  01h 42h 00h 00h 00h 00h 00h
Device replies: HiZ 12h 5Ah key  flg  dx   dy
```

- `key`: PS/2 scancode byte (one key per poll)
- `flg` bit layout:
  - Bits 0-1: Always 11b (differs from Sony mouse)
  - Bit 2: Left mouse button (0=pressed)
  - Bit 3: Right mouse button (0=pressed)
  - Bits 4-5: Always 11b
  - Bit 6: Key release flag (0 = break/F0h code)
  - Bit 7: Extended key flag (0 = E0h-prefixed code)
- `dx`, `dy`: Mouse delta movement

**Advantage:** Masquerades as standard mouse; simpler protocol.
**Disadvantage:** One scancode per frame = limited to ~60 keys/sec at 60 Hz
polling. Fast typists may drop keys.

---

## Finding 4: Existing Adapter Projects

### BlueRetro (ESP32) — HAS Lightspan Keyboard Emulation

- **Platform:** ESP32
- **Source:** github.com/darthcloud/BlueRetro
- Already implements PS1 Lightspan keyboard emulation (device ID 0x96)
- Takes input from any Bluetooth HID keyboard
- Translates to PS/2 Scan Code Set 2 for PS1
- Added in pre-release v0.9.1 (December 2020)
- **This is the most complete existing implementation**

Source: hackaday.io/project/170365-blueretro/log/186816

### PicoCtrl (RP2040) — Controller Emulator

- **Source:** github.com/ARandomOSDever/PicoCtrl
- Emulates PS1 digital controller using PIO-based SPI (psxSPI.pio)
- Uses PicoMemcard pinout: DAT=GPIO5, CMD=GPIO6, SEL=GPIO7, CLK=GPIO8, ACK=GPIO9
- Maps PC keyboard keys to controller buttons via USB serial
- Does NOT emulate keyboard device type — only maps to controller buttons
- **Good base code for RP2040 SIO0 emulation**

### PicoMemcard (RP2040) — Memory Card Emulator

- **Source:** github.com/dangiu/PicoMemcard
- Emulates PS1 memory card using PIO state machines
- Same pinout as PicoCtrl
- Contains proof-of-concept controller simulator
- **We already know this project from our PIO cartridge research**

### PicoGamepadConverter (RP2040)

- **Source:** github.com/Loc15/PicoGamepadConverter
- Supports PS1/PS2 device mode with USB host input
- Has PS/2 keyboard input on GPIO5 (data) and GPIO6 (clock)
- Converts keyboard/gamepad input to PS1 controller buttons (not keyboard device)

---

## Finding 5: Existing Blackroo Kernel Keyboard Code

The kernel has **stub-only** keyboard support:

### `arch/mipsnommu/ps/psx_kbd.c`

- Contains empty function stubs: `psx_kbd_read_input`, `psx_kbd_write_output`,
  `psx_kbd_read_status`, etc.
- A `psxsiokbd_ops` structure registers these stubs with the kernel keyboard subsystem
- `kbd_setkeycode()`, `kbd_getkeycode()`, `kbd_translate()`, `kbd_init_hw()` all return 0
- **No actual implementation** — just enough to prevent kernel crashes

### `arch/mipsnommu/ps/kbd-no.c`

- "No keyboard" fallback that returns -ENODEV for IRQ requests
- Used when no keyboard hardware is configured

**Conclusion:** The kernel framework for keyboard input exists but is completely
empty. The Runix developers clearly planned for it but never implemented it.

---

## Options for Blackroo Linux

### Option A: RP2040 Keyboard Adapter (Controller Port)

Build an adapter using Raspberry Pi Pico (RP2040) that:
1. Accepts USB keyboard input via Pico's USB host port
2. Responds to PS1 SIO0 polls as device ID 0x96 (Lightspan keyboard)
3. Translates USB HID scancodes to PS/2 Scan Code Set 2
4. Buffers up to 11 bytes of scancodes per poll frame

**Wiring (same as PicoMemcard):**

| PS1 Controller Port | Signal | RP2040 GPIO |
|---------------------|--------|-------------|
| Pin 1 | DATA | GPIO5 |
| Pin 2 | COMMAND | GPIO6 |
| Pin 6 | ATT/SEL | GPIO7 |
| Pin 7 | CLOCK | GPIO8 |
| Pin 9 | ACK | GPIO9 |
| Pin 4 | GND | GND |
| Pin 5 | VCC (3.3V) | 3V3 |

**Pros:** Standard protocol, 11 scancodes per frame, no PS1 software changes
needed beyond the kernel driver.
**Cons:** Requires building hardware adapter.

**Reference code:** BlueRetro (ESP32) has complete Lightspan keyboard emulation.
PicoCtrl/PicoMemcard have RP2040 PIO SPI slave code for PS1.

### Option B: BlueRetro (ESP32) — Off-the-shelf

Use the existing BlueRetro project with an ESP32 module. Chelson has a BlueRetro
kit available.

**Pros:** Already working, Bluetooth, no custom firmware needed.
**Cons:** ESP32 required (not RP2040), Bluetooth adds latency/complexity.
Project was archived 2025-12-14 — no new updates, but v25.04 is stable.

#### BlueRetro PS1 Setup Guide

**Firmware:**
- Use `BlueRetro_hw1_playstation.bin` from v25.04 release (latest stable)
- HW1 = external adapter (our use case). HW2 = internal console install only.
- Download from: github.com/darthcloud/BlueRetro/releases

**Flashing (first time, via USB):**
```bash
pip install esptool
# Download v25.04_hw1.zip, extract, then:
esptool.py --chip esp32 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x1000 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0xd000 ota/ota_data_initial.bin \
  0x10000 BlueRetro_hw1_playstation.bin
```

**Subsequent updates:** OTA via https://blueretro.io/ota.html in Chrome (BLE).

**Wiring — ESP32-DevKitC to PS1 Controller Port (Player 1):**

| PS1 Pin | Signal | ESP32 GPIO |
|---------|--------|------------|
| 1 | DATA | IO19 |
| 2 | CMD | IO32 |
| 3 | 8V (Motor) | LDO input -> 5V to ESP32 |
| 4 | GND | GND |
| 6 | ATT/CS | I34 |
| 7 | CLK | IO33 |
| 9 | ACK | IO21 |

**IMPORTANT:** Add 33-ohm inline resistors + TVS diodes (5VWM 9.2VC DO214AA)
on signal lines. Without them, PS1 gets random phantom button presses.
This is PS1-specific — the BlueRetro wiki explicitly warns about this.

If not wiring Player 2: tie IO5, IO26, IO27 to 3.3V (don't leave floating).

**Power:** PS1 pin 3 provides 8V. Feed through LT1117IST-5 LDO (with two
10uF caps) to produce 5V for ESP32. OR feed 8V directly into ESP32 DevKitC's
5V pin (its AMS1117-3.3 LDO is rated for 18V input).

**Alternative hardware:** mi213's PS1/PS2 Receiver PCB (github.com/Micha213/BlueRetro-PS1-2-Receiver)
is a custom SMD board that plugs directly into a PS1/PS2 controller port.
Requires ordering PCBs from JLCPCB + soldering.

#### Configuring Keyboard Mode

By default BlueRetro emulates a DualShock 2. Must change output to Keyboard mode:

1. Disconnect all controllers/keyboards from BlueRetro
2. Open Chrome (required — Web Bluetooth API) and go to:
   https://blueretro.io/advance.html
3. Click "Connect BlueRetro" — Chrome shows BLE pairing dialog
4. Select your BlueRetro device
5. In **Output Config** section:
   - Select output port (e.g. "Output 1" for Player 1)
   - Change **Mode** from "GamePad" to **"Keyboard"**
   - This makes BlueRetro emulate a Lightspan Keyboard (device ID 0x96)
6. Click **Save**
7. Reconnect your Bluetooth keyboard

PSX output modes available:
- **GamePad** — DualShock 2 emulation (default)
- **GamePadAlt** — Flightstick (SCPH-1110, green LED mode)
- **Keyboard** — Lightspan Keyboard (device ID 0x96)
- **Mouse** — PSX Mouse emulation

#### Pairing a Bluetooth Keyboard

1. Power on BlueRetro (LED on IO17 pulses = pairing mode)
2. Put Bluetooth keyboard into pairing mode
3. BlueRetro auto-discovers and connects
4. If keyboard asks for PIN: enter `0000` + Enter
5. LED goes solid = connected

#### Gotchas

- **Chrome only** for web config (Web Bluetooth API)
- **Disconnect BT devices before configuring** — web config won't connect otherwise
- **33-ohm resistors are essential** on PS1 signal lines
- **Project archived** — v25.04 is the last release, code is Apache-2.0
- Older BT keyboards may require PIN `0000`; BT 4.0+ usually don't

### Option C: PS/2 Keyboard via SIO1 (Serial Port)

Like the Spectrum Emulator v1 adapter — connect a PS/2 keyboard directly to
the SIO1 serial port. The keyboard's clock and data lines can be bit-banged
or a small MCU can translate to UART.

**Pros:** No SIO0 driver needed, uses existing serial console infrastructure.
**Cons:** Occupies SIO1 which is currently used for serial console/upload.
Would need to multiplex or switch between keyboard and serial host.

### Option D: USB Keyboard via PIO Expansion Port

Like the original Runix approach — use a USB host controller chip (SL811H or
MAX3421E) on the PIO expansion port to read USB keyboards.

**Pros:** Doesn't use controller port or serial port.
**Cons:** Requires PIO cartridge hardware modification, complex USB stack,
the original Runix USB driver was never completed.

---

## Recommended Approach

**Option A (RP2040 adapter)** is the best path for Blackroo Linux:

1. Hardware is cheap (~$4 Raspberry Pi Pico + PS1 controller extension cable)
2. We already have PicoMemcard PIO SPI code as reference
3. BlueRetro provides complete Lightspan protocol reference
4. The kernel driver is straightforward:
   - Poll controller port for device ID 0x96
   - Parse PS/2 Scan Code Set 2 scancodes
   - Feed into Linux keyboard input subsystem
   - The existing `psx_kbd.c` stub provides the framework

**Implementation order:**
1. Write the kernel driver first (can test with BlueRetro or any Lightspan adapter)
2. Build the RP2040 adapter for a cheaper/simpler solution
3. Fill in `psx_kbd.c` stubs with real SIO0 polling + scancode parsing

---

## Sources

- psx-spx Controllers and Memory Cards: psx-spx.consoledev.net/controllersandmemorycards/
- psx-spx Controllers - Keyboards: problemkaputt.de/psxspx-controllers-keyboards.htm
- psx-spx Serial Interfaces (SIO): psx-spx.consoledev.net/serialinterfacessio/
- BlueRetro (ESP32): github.com/darthcloud/BlueRetro
- BlueRetro Lightspan support log: hackaday.io/project/170365-blueretro/log/186816
- PicoCtrl (RP2040): github.com/ARandomOSDever/PicoCtrl
- PicoMemcard (RP2040): github.com/dangiu/PicoMemcard
- PicoGamepadConverter: github.com/Loc15/PicoGamepadConverter
- PS1Linux / Runix: github.com/CodeAsm/PS1Linux
- OSDev PS/2 Keyboard: wiki.osdev.org/PS/2_Keyboard
- SCPH-2000 RE thread: obscuregamers.com/threads/scph-2000-ps-2-keyboard-mouse-adapter-reverse-engineering.3048/
- GameSX PSX Controllers: gamesx.com/controldata/psxcont/psxcont.htm
