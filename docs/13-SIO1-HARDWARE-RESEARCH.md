# SIO1 Hardware Research — Real Hardware vs Emulator

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: UniROM as the upload path.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


Investigation date: 2026-04-13
Status: Two critical bugs found, fixes verified

## Problem Statement

The Blackroo kloader v0.0.1 Serial Shell works correctly in DuckStation emulator
but fails on real PS1 hardware. The shell enters "Listening" mode and sends BK>>
beacons, but the host PC never receives them. Memory card detection also fails
on real hardware.

---

## Finding 1: CTS Not Asserted — SIO1 Hardware Refuses to Transmit

**Severity:** CRITICAL — explains why host receives nothing

### The Problem

The PS1 SIO1 hardware enforces a hard requirement: TX data is only shifted out
when TXEN=1 **AND** CTS is asserted. This is enforced in silicon, not software.

Our `serial_init()` sets `SIO1_CTRL = 0x07`:
```
0x07 = 0b00000111
  Bit 0: 1 = TXEN (TX enabled)
  Bit 1: 1 = DTR asserted
  Bit 2: 1 = RXEN (RX enabled)
  Bit 5: 0 = RTS NOT asserted  ← PROBLEM
```

Without RTS asserted (bit 5), the FTDI adapter's RTS output stays low.
Since the FTDI's RTS connects to the PS1's CTS input, CTS remains
de-asserted, and the SIO1 hardware silently refuses to transmit.

### The Fix

Change CTRL from `0x07` to `0x27` to assert RTS:
```
0x27 = 0b00100111
  Bit 0: 1 = TXEN
  Bit 1: 1 = DTR
  Bit 2: 1 = RXEN
  Bit 5: 1 = RTS asserted  ← FIXED
```

### Why It Works in DuckStation

Emulators typically don't enforce the CTS requirement. They transmit whenever
TXEN is set and data is written to SIO1_DATA. Real hardware enforces CTS strictly.

### PS1 Motherboard Detail

The PS1 motherboard (PU-18, PU-22, PU-23) has:
- 1K ohm pull-down resistor on CTS input (CPU side)
- Transistor inverters between connector and CPU for CTS, DSR, TXD, DTR, RTS
- When nothing drives CTS, the CPU sees CTS=OFF → TX blocked

### Sources

- psx-spx Serial Interfaces: https://psx-spx.consoledev.net/serialinterfacessio/
  "Ings at bit0,2 and bit4 can beings if CTS is off"
  "SIO_STAT bit 0 (TX Ready) depends on CTS"
- psx-spx Pinouts: https://psx-spx.consoledev.net/pinouts/
  Documents the transistor inverters on the SIO1 connector signals
- Konami System 573 documentation: https://psx-spx.consoledev.net/konamisystem573/
  Documents CTS-to-RTS tie requirement for serial communication
- PSn00bSDK sio.c: https://github.com/Lameguy64/PSn00bSDK/blob/master/libpsn00b/psxsio/sio.c
  Reference SIO1 initialization
- NOTPSXSerial source: https://github.com/JonathanDotCel/NOTPSXSerial
  Sets DtrEnable=true, RtsEnable=true explicitly

---

## Finding 2: Wrong Baud Rate Multiplier — MUL1 Instead of MUL16

**Severity:** CRITICAL — wrong baud rate even if TX worked

### The Problem

Our code sets `SIO1_MODE = 0x004D` with a comment saying "x16 baud multiplier",
but the value is actually wrong:

```
0x004D = 0b01001101
  Bits 0-1: 01 = MUL1 (multiply by 1)  ← WRONG
  Bits 2-3: 11 = 8 data bits           ← correct
  Bit 4:    0  = no parity             ← correct
  Bits 6-7: 01 = 1 stop bit            ← correct
```

The baud multiplier bits (0-1) should be `10` (MUL16), not `01` (MUL1).

### SIO1_MODE Register Bit Layout

| Bits | Field | Values |
|------|-------|--------|
| 0-1 | Baud multiplier | 00=STOP, 01=MUL1, **10=MUL16**, 11=MUL64 |
| 2-3 | Character length | 00=5bit, 01=6bit, 10=7bit, 11=8bit |
| 4 | Parity enable | 0=no, 1=yes |
| 5 | Parity type | 0=even, 1=odd |
| 6-7 | Stop bits | 00=reserved, 01=1bit, 10=1.5bit, 11=2bit |

### The Fix

Change MODE from `0x004D` to `0x004E`:
```
0x004E = 0b01001110
  Bits 0-1: 10 = MUL16               ← FIXED
  Bits 2-3: 11 = 8 data bits
  Bit 4:    0  = no parity
  Bits 6-7: 01 = 1 stop bit
```

### Baud Rate Calculation

With MUL16 and BAUD register = 0x12 (18 decimal):
```
Actual baud = 33868800 / (18 * 16) = 117,600 bps
```
This is within UART tolerance of 115200 (2.1% error, acceptable).

With MUL1 (our bug) and BAUD = 0x12:
```
Actual baud = 33868800 / (18 * 1) = 1,881,600 bps
```
Completely wrong — 16x too fast, no UART can sync to this.

### Sources

- psx-spx SIO registers: https://psx-spx.consoledev.net/serialinterfacessio/
  Full MODE register bit definitions
- PSn00bSDK psxsio.h: https://github.com/Lameguy64/PSn00bSDK/blob/master/libpsn00b/include/psxsio.h
  Defines SIO_MODE_BAUD_MUL16 = 0x0002

---

## Finding 3: Host Tool Should Assert RTS/DTR

**Severity:** IMPORTANT — affects PS1 receiving from host

### The Problem

The Python host tool (`blackroo-serial.py`) opens the serial port with
`dsrdtr=False` and doesn't explicitly assert RTS or DTR. When using
a 3-wire cable (TX, RX, GND), the FTDI adapter's RTS output goes
to the PS1's CTS input. If pyserial doesn't assert RTS, the PS1
may not be able to receive data from the host.

### The Fix

After opening the serial port, explicitly set:
```python
ser.rts = True   # Assert RTS so PS1 sees CTS=ON
ser.dtr = True   # Assert DTR so PS1 sees DSR=ON
```

### How NOTPSXSerial Does It

From NOTPSXSerial source (C#):
```csharp
activeSerial.DtrEnable = true;
activeSerial.RtsEnable = true;
activeSerial.Handshake = Handshake.None;
```

### Sources

- NOTPSXSerial: https://github.com/JonathanDotCel/NOTPSXSerial
  Serial port initialization with explicit DTR/RTS
- pyserial documentation: https://pyserial.readthedocs.io/
  rts and dtr properties

---

## Finding 4: Stop Bits — NOTPSXSerial Uses 2 Stop Bits

**Severity:** LOW — may improve reliability

NOTPSXSerial switched to 2 stop bits (8N2) as of Release 5 (June 2020),
noting improved reliability on real hardware. Our code uses 1 stop bit.

Not changing this now — 1 stop bit should work. Noted for future
troubleshooting if we see intermittent byte errors.

### Sources

- NOTPSXSerial Release 5 changelog
- UniROM USB serial cable guide: https://unirom.github.io/serial_psx_cable/

---

## Finding 5: SIO1 Signal Inversion on PS1 Motherboard

**Severity:** INFORMATIONAL — affects custom cable design only

The PS1 motherboard inverts most SIO1 signals via transistors between
the CPU die and the external connector:

- **Inverted at connector:** TXD, DTR, RTS (outputs)
- **Inverted at connector:** CTS, DSR (inputs)
- **NOT inverted:** RXD

This means the SIO1 connector outputs inverted UART (idle-low instead
of standard idle-high). Standard FTDI adapters expect idle-high.

However, many people successfully use direct FTDI connections with UniROM,
suggesting FTDI chips tolerate inverted idle states or board revisions
handle this differently.

### Sources

- psx-spx Pinouts: https://psx-spx.consoledev.net/pinouts/
  "Active accent accent accent accent at accent accent accent serial port socket are accent accent inverted"
- UniROM serial cable guide: https://unirom.github.io/serial_psx_cable/
  Documents 3-wire connection working with standard FTDI

---

## SIO1_CTRL Register Reference

| Bit | Mask | Field | Our Value |
|-----|------|-------|-----------|
| 0 | 0x001 | TXEN (TX enable) | 1 |
| 1 | 0x002 | DTR output | 1 |
| 2 | 0x004 | RXEN (RX enable) | 1 |
| 3 | 0x008 | TX break | 0 |
| 4 | 0x010 | Error/IRQ reset | 0 |
| 5 | 0x020 | RTS output | **1 (was 0)** |
| 6 | 0x040 | Reset | 0 |

## SIO1_STAT Register Reference

| Bit | Mask | Field |
|-----|------|-------|
| 0 | 0x001 | TX FIFO not full (**depends on CTS**) |
| 1 | 0x002 | RX FIFO not empty |
| 2 | 0x004 | TX idle (**depends on TXEN and CTS**) |
| 7 | 0x080 | DSR input level |
| 8 | 0x100 | CTS input level |

---

## Summary of Required Changes

### serial.c (PS1 side)
1. `SIO1_MODE = 0x004D` → `SIO1_MODE = 0x004E` (fix MUL1 to MUL16)
2. `SIO1_CTRL = 0x07` → `SIO1_CTRL = 0x27` (assert RTS for CTS)

### blackroo-serial.py (Host side)
1. Add `ser.rts = True` and `ser.dtr = True` after opening port

### Files NOT changed
- memcard.c — SIO0, separate from SIO1, different issue
- shell.c — protocol layer, no hardware register access
- kernel.c — no serial involvement
- main.c — no serial involvement

---

## Memory Card Issue (Separate Investigation Needed)

Memory card detection failure on real hardware is a separate issue from
SIO1. Memory cards use SIO0 (0x1F801040), not SIO1 (0x1F801050).
Potential causes identified in code review:

- SIO0 settling time after StopPAD() may be too short (5000 iterations)
- No retry logic on detection failure
- Timeout values may be tight for real hardware
- No interrupt disable during SIO0 operations

This needs separate investigation and testing on real hardware.
