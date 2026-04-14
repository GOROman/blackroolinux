# Blackroo Linux - Serial Upload and Boot Process

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: UniROM and nops - the WIRING and PINOUT here are still correct.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> Loading Linux on PlayStation 1 via UniROM serial connection

---

## Overview

The primary method for loading Blackroo Linux onto a PlayStation 1 is via serial upload through the SIO1 port using the [UniROM](https://github.com/JonathanDotCel/unirom8_bootdisc_and_firmware_for_ps1) bootloader and the [NOTPSXSerial (nops)](https://github.com/JonathanDotCel/NOTPSXSerial) upload tool.

---

## Hardware Requirements

### Serial Cable

A 3-wire serial cable connecting the PS1 SIO1 port to a USB-to-serial adapter:

```
PlayStation 1 SIO Port          USB Serial Adapter
(rear of console)               (FTDI FT232 or CH341)
┌──────────────────┐            ┌──────────────┐
│ Pin 2: GND  ─────┼────────────┤ GND          │
│ Pin 5: RXD  ─────┼────────────┤ TXD          │
│ Pin 8: TXD  ─────┼────────────┤ RXD          │
└──────────────────┘            └──────────────┘
         (cross TX/RX)

Note: TX on PS1 connects to RX on adapter (and vice versa).
Only 3 wires needed. No handshake lines required for basic upload.
```

**SIO Port Pinout (looking at rear of PS1):**
```
         ┌─────────────────┐
         │  8  7  6  5  4  │
         │  3  2  1        │
         └─────────────────┘

Pin 1: DSR (Data Set Ready) - optional CTS
Pin 2: GND ← REQUIRED
Pin 3: DTR (Data Terminal Ready) - optional RTS
Pin 4: DCD (Data Carrier Detect) - not used
Pin 5: RXD (Receive Data) ← REQUIRED
Pin 6: N/C
Pin 7: N/C
Pin 8: TXD (Transmit Data) ← REQUIRED
```

### USB-to-Serial Adapters

| Adapter | Chipset | Compatibility | Notes |
|---------|---------|---------------|-------|
| FTDI FT232RL | FT232 | Excellent | Most reliable, 3.3V logic |
| CH341 | CH341 | Good | Cheaper, widely available |
| CP2102 | CP2102 | Good | Some need 5V-to-3.3V divider |
| PL2303 | PL2303 | Fair | Older models may have driver issues |

**Voltage warning:** The PS1 SIO port uses 3.3V logic levels. Most USB adapters output 3.3V (FTDI, CH341) but some output 5V TTL. Using 5V signals may damage the PS1. Use a voltage divider or level shifter if needed.

### UniROM Installation

UniROM can be loaded onto the PS1 via two methods:

**Method A: Cheat Cartridge (Simplest)**
1. Obtain an Action Replay, GameShark, or Xplorer cartridge
2. Flash UniROM firmware to the cartridge's flash ROM
3. Insert cartridge into the PS1 parallel port
4. Power on — UniROM starts automatically

**Method B: FreePSXBoot (No Hardware Required)**
1. Download [FreePSXBoot](https://github.com/brad-lin/FreePSXBoot) image for your PS1 model
2. Write the exploit image to a PS1 memory card using a PS3, Memcarduino, or similar
3. Insert memory card in slot 1
4. Power on with disc lid open
5. Navigate to memory card manager
6. Exploit triggers, UniROM loads

---

## Upload Process

### Step 1: Prepare the PS1

```
1. Connect serial cable (PS1 SIO → USB adapter → host PC)
2. Insert UniROM cartridge (or FreePSXBoot memory card)
3. Power on PlayStation
4. UniROM menu appears on TV screen
5. Select "Serial" mode (or it defaults to waiting for serial)
```

### Step 2: Upload from Host

**Linux:**
```bash
# Install nops (if not already)
# Download from: https://github.com/JonathanDotCel/NOTPSXSerial

# Standard speed (115200 baud, ~11 KB/s)
./nops /exe output/kernel.exe /dev/ttyUSB0

# Fast mode (518400 baud, ~50 KB/s) — requires UniROM 8.0.J+
./nops /fast /exe output/kernel.exe /dev/ttyUSB0

# Upload kernel with embedded initrd
./nops /fast /exe output/linux.image.exe /dev/ttyUSB0
```

**Windows:**
```cmd
REM Standard speed
nops.exe /exe output\kernel.exe COM3

REM Fast mode
nops.exe /fast /exe output\kernel.exe COM3
```

**macOS:**
```bash
./nops /exe output/kernel.exe /dev/tty.usbserial-*
```

### Step 3: Kernel Boots

After upload completes:
1. UniROM places the PS-EXE in RAM at the specified load address
2. UniROM jumps to the PS-EXE entry point
3. Kernel's `head.S` starts executing
4. Kernel initializes hardware, prints boot messages via serial
5. (If initrd is embedded) Kernel mounts initrd
6. Kernel attempts to run `/linuxrc` or `/init`

### Monitoring Boot Output

The kernel sends all console output back through the serial port:

```bash
# Linux: Monitor serial output
screen /dev/ttyUSB0 115200

# Or with minicom
minicom -D /dev/ttyUSB0 -b 115200

# Or with picocom
picocom -b 115200 /dev/ttyUSB0

# Python (for scripting)
python3 -c "
import serial
s = serial.Serial('/dev/ttyUSB0', 115200)
while True:
    line = s.readline()
    print(line.decode('ascii', errors='replace'), end='')
"
```

**Expected boot output (serial console):**
```
Linux version 2.4.x (mipsel-linux-gcc) #1
PSX: 2048 KB RAM configured (reg=0x00000888)
PSX SIO console enable
Calibrating delay loop... XX.XX BogoMIPS
Memory: XXXk available
Freeing unused PROM memory: XXk freed
bu: detecting card in slot 1 ...
bu: detecting card in slot 2 ...
bu: driver for 2 cards initialized
VFS: Mounted root (ext2 filesystem)
Freeing init memory: XXk freed

Simple initrd is active

/ #
```

---

## Transfer Speed Comparison

| Mode | Baud Rate | Throughput | 800KB Kernel | 2.8MB Kernel+InitRD |
|------|-----------|-----------|--------------|---------------------|
| Standard | 115,200 | ~11 KB/s | ~73 seconds | ~255 seconds |
| Fast | 518,400 | ~50 KB/s | ~16 seconds | ~56 seconds |

**Fast mode requirements:**
- UniROM 8.0.J or newer
- Serial adapter that supports 518400 baud (most FTDI/CH341 do)
- `/fast` flag in nops command

---

## Troubleshooting

### Common Issues

| Problem | Cause | Solution |
|---------|-------|----------|
| "No response from PSX" | Wrong COM port or cable | Check cable connections, try different port |
| "Timeout waiting for handshake" | UniROM not running | Ensure PS1 is in UniROM serial mode |
| Garbage characters in output | Baud rate mismatch | Verify both sides use 115200 |
| Upload starts but hangs | Cable signal integrity | Use shorter cable, check solder joints |
| Upload completes but no boot | PS-EXE format issue | Re-convert with elf2psx, check header |
| Kernel panics immediately | Wrong RAM config | Verify RAM_SIZE register matches hardware |
| "Unable to mount root fs" | No initrd or wrong format | Embed initrd with addinitrd, verify ext2 |
| Serial output then silence | Kernel crash | Enable DEBUG in bu.c, check serial output |

### Verifying the Serial Cable

```bash
# Send test byte from host
echo -n "A" > /dev/ttyUSB0

# On UniROM: check if byte received (may show in debug view)

# Loopback test (connect TX to RX on adapter, no PS1):
echo "Hello" > /dev/ttyUSB0 &
cat /dev/ttyUSB0
# Should show "Hello" back
```

### Verifying the PS-EXE

```bash
# Check PS-EXE header
hexdump -C output/kernel.exe | head -5
# Should start with: 50 53 2D 58 20 45 58 45  ("PS-X EXE")

# Check file size
ls -la output/kernel.exe
# Size should be reasonable (500KB-3MB depending on initrd)

# Verify ELF before conversion
file output/linux.elf
# Should show: ELF 32-bit LSB executable, MIPS, MIPS-I version 1
```

---

## Alternative Upload Methods

### Direct Serial (Without nops)

For custom upload protocols or debugging, you can write directly to the serial port:

```python
#!/usr/bin/env python3
"""Direct PS-EXE upload to UniROM via serial"""
import serial
import struct
import sys
import time

PORT = sys.argv[1]      # e.g., /dev/ttyUSB0
PSEXE = sys.argv[2]     # e.g., kernel.exe

# Open serial port
ser = serial.Serial(PORT, 115200, timeout=5)
time.sleep(0.5)

# Read PS-EXE
with open(PSEXE, 'rb') as f:
    data = f.read()

# Verify PS-EXE header
if data[:8] != b'PS-X EXE':
    print("ERROR: Not a valid PS-EXE file")
    sys.exit(1)

# Parse header
pc0 = struct.unpack_from('<I', data, 0x10)[0]      # Entry point
t_addr = struct.unpack_from('<I', data, 0x18)[0]    # Text start
t_size = struct.unpack_from('<I', data, 0x1C)[0]    # Text size

print(f"Entry: 0x{pc0:08X}")
print(f"Load:  0x{t_addr:08X}")
print(f"Size:  {t_size} bytes ({t_size/1024:.1f} KB)")

# The actual upload protocol depends on UniROM's serial interface
# See UniROM documentation for the handshake protocol
print("Use nops tool for actual upload — this is just a header check")

ser.close()
```

### Pi Pico as Serial Bridge

A Raspberry Pi Pico can act as a high-speed serial bridge between the PS1 and a modern computer, potentially achieving faster transfer rates than standard USB-serial adapters.

```
Host PC ←─ USB ─→ Pi Pico ←─ UART ─→ PS1 SIO
                   (custom firmware)
```

The Pico's dual-core RP2040 can handle bidirectional serial at rates up to 921600 baud with buffering, reducing the chance of dropped bytes at high speeds.

---

## Serial Console as Primary I/O

Once Linux boots, the serial console becomes the primary interface:

### Shell Access

```bash
# On host PC, after kernel boots:
# The serial terminal (screen, minicom, etc.) becomes the Linux shell

/ # ls
bin   dev   etc   mnt   proc  sbin  sys   tmp
/ # cat /proc/cpuinfo
processor       : 0
cpu model       : R3000
system type     : PlayStation
/ # cat /proc/meminfo
MemTotal:        XXXX kB
MemFree:         XXXX kB
/ # dmesg | head
Linux version 2.4.x ...
PSX: XXXX KB RAM configured
...
/ # mount
rootfs on / type rootfs (rw)
/dev/ram0 on / type ext2 (rw)
proc on /proc type proc (rw)
```

### File Transfer Over Serial

To transfer files to/from the running Linux system:

```bash
# On PS1 Linux: receive a file via serial
cat /dev/ttyS0 > /tmp/received_file

# On host: send a file
cat somefile > /dev/ttyUSB0

# For reliable transfer, use a protocol:
# (requires busybox with rx/sx or custom tool)

# XMODEM receive on PS1:
rx /tmp/file < /dev/ttyS0 > /dev/ttyS0

# XMODEM send from host:
sx somefile > /dev/ttyUSB0 < /dev/ttyUSB0
```

---

## Boot Method Summary

| Method | Requirements | Speed | Ease | Standalone |
|--------|-------------|-------|------|------------|
| **UniROM + Serial** | Serial cable, UniROM cart | 11-50 KB/s | Easy | No (needs host) |
| **FreePSXBoot + Serial** | Serial cable, exploit card | 11-50 KB/s | Medium | No (needs host) |
| **CD Boot** | Mod chip or swap trick | N/A (direct) | Hard | Yes |
| **Memory Card Boot** | Bootloader + stored kernel | ~22 KB/s | Medium | Yes (after setup) |
| **Bootloader + Serial** | PSn00bSDK bootloader | 11-50 KB/s | Easy | No (needs host) |

**Recommended development workflow:**
1. Use UniROM + serial for rapid development iteration
2. Upload new kernel builds in ~15-70 seconds
3. Monitor boot via serial console
4. Iterate on kernel/initrd until boot succeeds
5. Once stable, create CD image or memory card image for standalone boot

---

*Blackroo Linux Serial Upload and Boot Process*
