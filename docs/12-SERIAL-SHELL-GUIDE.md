# Blackroo Serial Shell — User Guide v0.0.1

The Blackroo Serial Shell is a command-driven interface between your Linux PC
and the PlayStation 1. The PS1 runs the **kloader** (bootloader), and your PC
runs **blackroo-serial** to send commands over a serial cable.

Together they let you upload Linux kernels, inspect PS1 memory, program flash
cartridges, and get a Linux shell running on the PS1 — all from your terminal.

---

## 1. What You Need

### Hardware

| Component | Details |
|-----------|---------|
| PlayStation 1 | Any model (SCPH-1001 through SCPH-102) |
| Serial adapter | 3.3V USB-to-serial (FTDI FT232RL recommended) |
| Serial cable | 3 wires: GND, TX, RX (see wiring below) |
| Boot method | FreePSXBoot memory card, modchip, or swap trick |

### Software

| Component | Location |
|-----------|----------|
| Blackroo kloader | `output/bootloader.exe` (PS-EXE for PS1) |
| blackroo-serial | `tools/host/blackroo-serial.py` (Python 3) |
| pyserial | `pip install pyserial` |
| PSn00bSDK Docker | `blackroo-psn00bsdk` image (for building) |

### Serial Cable Wiring

```
PS1 SIO1 Port              FTDI Adapter
─────────────              ────────────
Pin 2: GND     ──────────  GND
Pin 5: RXD     ──────────  TXD   (crossed!)
Pin 8: TXD     ──────────  RXD   (crossed!)
```

**WARNING:** The PS1 uses 3.3V logic levels. A 5V adapter will damage the
PS1 SIO port. Use an FT232RL or similar 3.3V adapter.

The SIO1 port is on the back of the PS1, next to the AV multi-out.
It's a 10-pin connector. Only 3 pins are needed.

---

## 2. Getting Started

### Step 1: Build the Bootloader

```bash
cd blackroolinux-main
sudo bash bootloader/build.sh
```

This produces `output/bootloader.exe` — a PS-EXE that runs on the PS1.

### Step 2: Get the Bootloader onto the PS1

Load `bootloader.exe` on the PS1 using one of:

- **FreePSXBoot** — Memory card exploit (no hardware mod needed)
- **Serial upload via existing loader** — If you already have a serial loader
- **CD-R** — Burn to disc and boot via modchip or swap trick
- **Flash cartridge** — Program to a GameShark/Action Replay cart

### Step 3: Enter Serial Shell Mode

On the PS1 screen, navigate to **Serial Shell (115200)** and press Start.
The PS1 shows "Listening for commands" and starts sending beacons.

### Step 4: Connect from Your PC

```bash
# Find your serial port:
./tools/host/blackroo-serial.py scan

# Upload a Linux kernel and boot it:
./tools/host/blackroo-serial.py /dev/ttyUSB0 upload output/kernel.exe
```

The tool uploads the kernel, boots it, and drops you into a Linux shell.

---

## 3. blackroo-serial Command Reference

### upload — Upload PS-EXE and Boot

The primary workflow. Uploads a kernel PS-EXE, launches it, and enters
the serial console so you land in a Linux shell.

```bash
blackroo-serial PORT upload kernel.exe
```

Options:
- `--fast` — Upload at 518400 baud (4.5x faster)
- `--no-boot` — Upload only, don't launch the kernel
- `--no-console` — Don't enter console mode after boot
- `--cmdline "..."` — Override the kernel command line

```bash
# Fast upload with custom root device:
blackroo-serial /dev/ttyUSB0 upload kernel.exe --fast \
  --cmdline "root=/dev/ram0 init=/bin/sh console=ttyS0,115200"
```

### boot — Launch Previously Uploaded Kernel

If you uploaded with `--no-boot`, use this to launch later.

```bash
blackroo-serial PORT boot
blackroo-serial PORT boot --cmdline "root=/dev/bu0 init=/linuxrc"
```

### bin — Upload Raw Binary

Upload any binary file to a specific PS1 memory address.
Useful for ramdisk images, test payloads, or data blobs.

```bash
blackroo-serial PORT bin initrd.img 0x80100000
blackroo-serial PORT bin test.bin 0x80020000
```

### ping — Test Connection

Verify the serial link works. PS1 should respond with PONG.

```bash
blackroo-serial PORT ping
```

### peek — Read Memory

Read a 32-bit word from any PS1 address.

```bash
blackroo-serial PORT peek 0x1F801060     # RAM size register
blackroo-serial PORT peek 0x80000000     # Start of RAM
blackroo-serial PORT peek 0x1F801050     # SIO1 data register
```

### poke — Write Memory

Write a 32-bit value to any PS1 address.

```bash
blackroo-serial PORT poke 0x1F801060 0x0B88   # Set 8MB RAM
blackroo-serial PORT poke 0x80000180 0x00      # Clear cmdline
```

### dump — Download Memory

Download a region of PS1 memory. Displays hex on screen, or saves to file.

```bash
# Hex dump first 256 bytes of kernel:
blackroo-serial PORT dump 0x80010000 256

# Save 128KB region to file:
blackroo-serial PORT dump 0x80010000 131072 memdump.bin
```

### exec — Jump to Address

Make the PS1 jump to a memory address. The PS1 won't return to the shell.

```bash
blackroo-serial PORT exec 0x80010000
```

### console — Serial Terminal

Interactive terminal for talking to the PS1. Use this when the Linux kernel
is already running and you want a shell.

```bash
blackroo-serial PORT console
```

Keys:
- **Ctrl+]** — Disconnect
- **Ctrl+B** — Send break signal

### monitor — Hex Monitor

Show all raw bytes coming from the PS1 in hex + ASCII.
Useful for debugging protocol issues.

```bash
blackroo-serial PORT monitor
```

### flash-dump / flash-load — PIO Flash Operations

Read/write the expansion port flash cartridge (GameShark, Action Replay, etc).
These use the BKFL protocol via the PIO Flash Manager menu on the PS1.

```bash
# Backup flash cartridge:
blackroo-serial PORT flash-dump backup.bin

# Program new image:
blackroo-serial PORT flash-load newrom.bin
```

### scan — Find Serial Ports

List available serial ports and identify likely PS1 adapters.

```bash
blackroo-serial scan
```

### help — Full Documentation

```bash
blackroo-serial help
```

---

## 4. PS1 Bootloader Menu

When the kloader boots on the PS1, you see this menu:

```
BLACKROO LINUX v0.0.1
======================

> Serial Shell (115200)
  Serial Shell (518400)
  Boot from Memory Card
  Boot from CD-ROM
  Memory Card Manager
  System Settings
  PIO Flash Manager
  Hardware Info
```

Navigate with **D-pad**, confirm with **Start**.

### Serial Shell

Enters the command server mode. The PS1 listens for commands from
`blackroo-serial` on your PC. Press **Select** to exit back to the menu.

The 115200 option starts at standard baud. The 518400 option starts at
high speed (your PC must also connect at 518400).

When in shell mode, the host tool can switch speeds using the FAST/SLOW
commands without restarting.

### Boot from CD-ROM

Read a kernel PS-EXE directly from a burned CD-R. Looks for
`KERNEL.EXE` or `LINUX.EXE` on an ISO9660 filesystem.

### System Settings

Configure boot defaults stored on memory card:

- **Boot Source** — Which method to use on power-up
- **Root Device** — Kernel root filesystem (`/dev/bu0`, `/dev/bul`, `/dev/ram0`)
- **Init Program** — First process (`/bin/sh`, `/linuxrc`, `/sbin/init`)
- **Console** — GPU + Serial, GPU only, or Serial only
- **RAM Size** — Auto-detect or manual (2/4/8 MB)
- **Auto-boot** — Boot automatically after timeout

Settings persist across power cycles (saved to memory card block 1).

### Hardware Info

Shows detected hardware: CPU type, RAM size, video mode, memory cards,
PIO expansion port, DMA state.

---

## 5. Protocol Reference

The Blackroo Shell Protocol uses 4-byte ASCII tags for commands and responses.
All multi-byte values are little-endian. Data transfers use 2048-byte chunks
with byte-sum checksums.

### Beacon

The PS1 sends `BK>>` (0x3E3E4B42) periodically when idle. The host waits
for this before sending any command.

### Command Flow

```
Host  ──[command tag (4 bytes)]──>  PS1
PS1   ──[OKOK / FAIL (4 bytes)]──> Host
       ...command-specific data...
```

### Tags

| Tag | Hex (LE) | Direction | Meaning |
|-----|----------|-----------|---------|
| `BK>>` | 0x3E3E4B42 | PS1->Host | Beacon (ready) |
| `OKOK` | 0x4B4F4B4F | PS1->Host | Command accepted |
| `FAIL` | 0x4C494146 | PS1->Host | Error |
| `MORE` | 0x45524F4D | PS1->Host | Ready for next chunk |
| `DONE` | 0x454E4F44 | PS1->Host | Transfer complete |
| `PONG` | 0x474E4F50 | PS1->Host | Ping response |
| `PING` | 0x474E4950 | Host->PS1 | Ping |
| `UEXE` | 0x45585545 | Host->PS1 | Upload PS-EXE |
| `UBIN` | 0x4E494255 | Host->PS1 | Upload binary |
| `EXEC` | 0x43455845 | Host->PS1 | Jump to address |
| `BOOT` | 0x544F4F42 | Host->PS1 | Launch kernel |
| `DUMP` | 0x504D5544 | Host->PS1 | Download memory |
| `PEEK` | 0x4B454550 | Host->PS1 | Read word |
| `POKE` | 0x454B4F50 | Host->PS1 | Write word |
| `FAST` | 0x54534146 | Host->PS1 | Switch to 518400 |
| `SLOW` | 0x574F4C53 | Host->PS1 | Switch to 115200 |
| `REST` | 0x54534552 | Host->PS1 | Reset |

### UEXE Transfer Flow

```
Host -> PS1:  UEXE
PS1  -> Host: OKOK
Host -> PS1:  entry(4) + load_addr(4) + size(4) + checksum(4)
PS1  -> Host: OKOK
Host -> PS1:  [2048 bytes of data]
PS1  -> Host: MORE
Host -> PS1:  [2048 bytes of data]
PS1  -> Host: MORE
  ... repeat until all data sent ...
PS1  -> Host: DONE  (checksum match)
         or   FAIL  (checksum mismatch)
```

### Checksum

Simple byte sum, truncated to 32 bits:

```python
checksum = sum(data_bytes) & 0xFFFFFFFF
```

---

## 6. Typical Workflows

### Upload and Run Linux

```bash
# Build kernel (separate step, not covered here)
# Upload and boot:
blackroo-serial /dev/ttyUSB0 upload output/kernel.exe

# You're now in a Linux shell on the PS1!
```

### Fast Development Cycle

```bash
# Upload at high speed, don't auto-boot:
blackroo-serial /dev/ttyUSB0 upload test.exe --fast --no-boot

# Check something in memory:
blackroo-serial /dev/ttyUSB0 peek 0x80010000

# Boot when ready:
blackroo-serial /dev/ttyUSB0 boot
```

### Debug a Crash

```bash
# Dump the area around the crash address:
blackroo-serial /dev/ttyUSB0 dump 0x80010000 0x1000 crash_dump.bin

# Check hardware registers:
blackroo-serial /dev/ttyUSB0 peek 0x1F801070   # INT_STAT
blackroo-serial /dev/ttyUSB0 peek 0x1F801074   # INT_MASK
```

### Backup and Program Flash Cartridge

```bash
# On PS1: PIO Flash Manager -> Dump to Serial
blackroo-serial /dev/ttyUSB0 flash-dump gs_backup.bin

# On PS1: PIO Flash Manager -> Load from Serial
blackroo-serial /dev/ttyUSB0 flash-load new_rom.bin
```

---

## 7. Troubleshooting

### "No beacon received (timeout)"

- PS1 must be in Serial Shell mode (select it from the menu)
- Check cable wiring (TX/RX are crossed)
- Check baud rate matches (both sides must agree)
- Try `blackroo-serial PORT monitor` to see raw traffic
- Check permissions: `sudo chmod 666 /dev/ttyUSB0`

### "Checksum mismatch"

- Noisy cable or bad connection
- Try lower baud rate (115200 instead of 518400)
- Check cable length (keep under 1 meter for high speed)

### "Cannot open /dev/ttyUSB0"

- Adapter not plugged in or driver not loaded
- Run `blackroo-serial scan` to find the right port
- Check permissions: add your user to the `dialout` group
  ```bash
  sudo usermod -aG dialout $USER
  ```

### Kernel boots but no shell appears

- Check kernel cmdline includes `console=ttyS0,115200`
- Check kernel cmdline includes `init=/bin/sh`
- Make sure root filesystem is accessible
- Try `console=ttyS0,115200 console=tty0` for dual output

### Transfer is slow

- Use `--fast` for 518400 baud (~45 KB/s vs ~10 KB/s)
- The speed negotiation happens automatically
- FTDI adapters give best results at high speed
- CH340 adapters may not support 518400 reliably

---

## 8. Building from Source

### Prerequisites

- Docker with `blackroo-psn00bsdk` image
- Python 3 with `pyserial`

### Build the Bootloader

```bash
sudo bash bootloader/build.sh
```

The build script:
1. Removes all previous build artifacts (clean build)
2. Compiles via PSn00bSDK in Docker
3. Validates the PS-EXE output
4. Copies to `output/bootloader.exe`

### Install the Host Tool

The host tool is a single Python script with one dependency:

```bash
pip install pyserial

# Optional: make it available system-wide
sudo cp tools/host/blackroo-serial.py /usr/local/bin/blackroo-serial
sudo chmod +x /usr/local/bin/blackroo-serial
```

---

## Version History

### v0.0.1 — Kloader (2026-04-13)

Initial release of the Blackroo Serial Shell system.

- Command-driven serial protocol (Blackroo Shell Protocol)
- PS-EXE and raw binary upload with checksums
- Memory peek/poke/dump for hardware debugging
- Kernel launch with configurable command line
- Baud rate negotiation (115200 / 518400)
- Interactive serial console for Linux shell
- PIO flash cartridge read/write (BKFL protocol)
- Persistent settings on memory card
- Hardware detection (CPU, RAM, video, cards, PIO)
