#!/usr/bin/env python3
"""
blackroo-serial — Blackroo Linux PS1 Serial Tool v0.0.1

Host-side companion to the Blackroo kloader (bootloader) running
on the PlayStation 1. Communicates over SIO1 via USB-to-serial
adapter using the Blackroo Shell Protocol.

Attribution: New Blackroo work (2026, GPL v2)
"""

import sys
import os
import time
import struct
import select
import termios
import tty
import argparse
import textwrap

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial not installed.")
    print("  pip install pyserial")
    sys.exit(1)


# ── Version ───────────────────────────────────────────────────

VERSION = "0.0.1"
BANNER = f"blackroo-serial v{VERSION} — Blackroo Linux PS1 Tool"


# ── Blackroo Shell Protocol ──────────────────────────────────

def mktag(s):
    """Convert 4-char ASCII string to little-endian uint32."""
    return struct.unpack("<I", s.encode("ascii"))[0]

def tag_str(val):
    """Convert uint32 tag back to ASCII for display."""
    try:
        return struct.pack("<I", val).decode("ascii")
    except Exception:
        return f"0x{val:08X}"

# Beacon & responses
SHELL_BEACON = mktag("BK>>")
SHELL_OK     = mktag("OKOK")
SHELL_ERR    = mktag("FAIL")
SHELL_MORE   = mktag("MORE")
SHELL_DONE   = mktag("DONE")
SHELL_PONG   = mktag("PONG")

# Commands
CMD_PING = mktag("PING")
CMD_UEXE = mktag("UEXE")
CMD_UBIN = mktag("UBIN")
CMD_EXEC = mktag("EXEC")
CMD_BOOT = mktag("BOOT")
CMD_DUMP = mktag("DUMP")
CMD_PEEK = mktag("PEEK")
CMD_POKE = mktag("POKE")
CMD_FAST = mktag("FAST")
CMD_SLOW = mktag("SLOW")
CMD_REST = mktag("REST")

CHUNK_SIZE = 2048
BAUD_SLOW  = 115200
BAUD_FAST  = 518400

# PIO flash protocol (separate from shell, uses its own menu)
PIOFLASH_MAGIC    = 0x4C464B42  # "BKFL"
PIOFLASH_HDR_SIZE = 16
KNOWN_CHIPS = {
    (0xBF, 0xB5): ("SST", "SST39SF010", 128),
    (0xBF, 0xB6): ("SST", "SST39SF020", 256),
    (0xBF, 0xB7): ("SST", "SST39SF040", 512),
    (0x01, 0x20): ("AMD", "AM29F010", 128),
    (0x01, 0xB0): ("AMD", "AM29F020", 256),
    (0x01, 0xA4): ("AMD", "AM29F040", 512),
    (0x01, 0xD5): ("AMD", "AM29F080", 1024),
    (0xDA, 0x45): ("Winbond", "W29C020", 256),
    (0xDA, 0x46): ("Winbond", "W29C040", 512),
    (0x1F, 0xDA): ("Atmel", "AT29C020", 256),
    (0xC2, 0xA4): ("Macronix", "MX29F040", 512),
}

PSEXE_MAGIC = b"PS-X EXE"
PSEXE_HEADER_SIZE = 2048

# Global verbose flag
VERBOSE = False

def vprint(*args, **kwargs):
    """Print only in verbose mode."""
    if VERBOSE:
        ts = time.time() % 100  # seconds.millis within 100s window
        print(f"  [{ts:06.3f}]", *args, **kwargs)


def hexdump_bytes(data, prefix=""):
    """Format bytes as hex + ASCII for debug output."""
    hex_part = " ".join(f"{b:02X}" for b in data[:32])
    asc_part = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in data[:32])
    s = f"{prefix}{hex_part}  {asc_part}"
    if len(data) > 32:
        s += f" ... ({len(data)} bytes total)"
    return s


# ── Utilities ─────────────────────────────────────────────────

def fmt_size(n):
    if n >= 1024 * 1024:
        return f"{n / (1024*1024):.1f} MB"
    elif n >= 1024:
        return f"{n / 1024:.1f} KB"
    return f"{n} bytes"


def fmt_rate(nbytes, elapsed):
    if elapsed <= 0:
        return "N/A"
    return f"{nbytes / elapsed / 1024:.1f} KB/s"


def parse_int(s):
    """Parse integer from string (supports 0x hex prefix)."""
    if s.startswith("0x") or s.startswith("0X"):
        return int(s, 16)
    return int(s)


def open_serial(port, baud, timeout=10):
    """Open serial port with PS1-compatible settings: 8N1."""
    ser = serial.Serial(
        port=port,
        baudrate=baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=timeout,
        write_timeout=timeout,
        dsrdtr=False,
    )
    # Assert RTS and DTR so the PS1 sees CTS=ON and DSR=ON.
    # Without CTS, the PS1 SIO1 hardware blocks all TX.
    # Source: NOTPSXSerial sets DtrEnable=true, RtsEnable=true
    ser.rts = True
    ser.dtr = True
    time.sleep(0.1)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    return ser


def send_u32(ser, val):
    """Send a little-endian 32-bit word."""
    data = struct.pack("<I", val)
    vprint(f"TX: {' '.join(f'{b:02X}' for b in data)}  ({tag_str(val)})")
    ser.write(data)


def recv_u32(ser, timeout=5):
    """Receive a little-endian 32-bit word."""
    old = ser.timeout
    ser.timeout = timeout
    data = ser.read(4)
    ser.timeout = old
    if len(data) < 4:
        vprint(f"RX: TIMEOUT ({len(data)} bytes: "
               f"{' '.join(f'{b:02X}' for b in data)})" if data
               else "RX: TIMEOUT (0 bytes)")
        return None
    val = struct.unpack("<I", data)[0]
    vprint(f"RX: {' '.join(f'{b:02X}' for b in data)}  ({tag_str(val)})")
    return val


def checksum(data):
    """Compute byte-sum checksum (matches PS1 side)."""
    total = 0
    for b in data:
        total += b
    return total & 0xFFFFFFFF


# ── Shell connection ──────────────────────────────────────────

def wait_for_beacon(ser, timeout=30):
    """
    Wait for the BK>> beacon from the Blackroo shell.
    Returns True when beacon is found.
    Drains any trailing bytes after the beacon to avoid
    stale data corrupting the next command exchange.
    """
    print("  Waiting for shell beacon (BK>>)...")
    if not VERBOSE:
        print("  (Select 'Serial Shell' on PS1 and press Start)")

    ser.reset_input_buffer()
    beacon_bytes = struct.pack("<I", SHELL_BEACON)
    start = time.time()
    buf = bytearray()
    beacon_count = 0

    while time.time() - start < timeout:
        n = ser.in_waiting
        if n > 0:
            chunk = ser.read(n)
            buf.extend(chunk)
            if VERBOSE and beacon_count == 0:
                vprint(f"RX raw: {hexdump_bytes(chunk)}")

            idx = buf.find(beacon_bytes)
            if idx >= 0:
                beacon_count += 1
                elapsed_ms = (time.time() - start) * 1000
                if VERBOSE:
                    vprint(f"Beacon #{beacon_count} at buf offset {idx} "
                           f"({elapsed_ms:.0f}ms)")
                    # Show what was before the beacon (junk?)
                    if idx > 0:
                        vprint(f"Pre-beacon junk: "
                               f"{hexdump_bytes(buf[:idx])}")

                # Drain: keep only data after this beacon
                buf = buf[idx + 4:]

                # Small pause to let any trailing bytes arrive,
                # then flush them so they don't corrupt the
                # next command/response exchange.
                time.sleep(0.05)
                stale = ser.in_waiting
                if stale > 0:
                    stale_data = ser.read(stale)
                    if VERBOSE:
                        vprint(f"Flushed {stale} stale bytes after "
                               f"beacon: {hexdump_bytes(stale_data)}")
                elif VERBOSE:
                    vprint("No stale bytes after beacon (clean)")

                print(f"  Beacon received! "
                      f"({elapsed_ms:.0f}ms, {beacon_count} seen)")
                return True

            if len(buf) > 64:
                if VERBOSE:
                    vprint(f"Buffer trim, no beacon yet "
                           f"({len(buf)} bytes)")
                buf = buf[-16:]
        else:
            time.sleep(0.05)

    print("  ERROR: No beacon received (timeout)")
    if VERBOSE and buf:
        vprint(f"Buffer at timeout: {hexdump_bytes(buf)}")
    return False


def send_command(ser, cmd, timeout=5):
    """Send a command tag and wait for response."""
    send_u32(ser, cmd)
    return recv_u32(ser, timeout=timeout)


# ── Command: help ─────────────────────────────────────────────

def cmd_help():
    """Print detailed usage guide."""
    print(BANNER)
    print()
    print(textwrap.dedent("""\
    QUICK START
    ───────────
      1. Connect PS1 SIO1 to PC via 3.3V FTDI adapter
      2. Load the Blackroo bootloader on PS1
      3. Select 'Serial Shell' on the PS1 menu
      4. Run: blackroo-serial /dev/ttyUSB0 upload kernel.exe

      This uploads the kernel, boots it, and drops you into
      a Linux shell over serial. That's it.

    COMMANDS
    ────────
      upload <file.exe>          Upload PS-EXE, boot, enter console
        --no-boot                  Upload only, don't launch kernel
        --no-console               Don't enter console after boot
        --cmdline "..."            Override kernel command line
        --fast                     Upload at 518400 baud

      boot                       Launch a previously uploaded kernel
        --cmdline "..."            Override kernel command line

      bin <file> <addr>          Upload raw binary to PS1 address
                                   Address in hex: 0x80100000

      dump <addr> <size> [file]  Download memory from PS1
                                   Displays hex if no file given

      peek <addr>                Read 32-bit word at PS1 address
      poke <addr> <value>        Write 32-bit word to PS1 address

      exec <addr>                Jump to address (PS1 won't return)

      ping                       Test connection to kloader shell

      diag                       Run connection diagnostics
                                   Tests beacon, ping, and UEXE handshake
                                   Shows raw hex of everything received

      console                    Interactive serial terminal
                                   Ctrl+] to exit, Ctrl+B for break

      monitor                    Raw hex dump of serial traffic
                                   Ctrl+C to exit

      sendexe <file.exe>         Send PS-EXE to UniROM or other loader
                                   Use this to get kloader onto the PS1
                                   the first time. After that, use 'upload'.

      flash-dump <out.bin>       Dump PIO expansion port flash
      flash-load <in.bin>        Program PIO expansion port flash

      scan                       List available serial ports

      help                       Show this guide

    OPTIONS
    ───────
      --baud <rate>     Baud rate (default: 115200)
      --fast            Shortcut for --baud 518400
      --timeout <sec>   Serial timeout (default: 10)
      --version         Show version

    HARDWARE WIRING
    ───────────────
      PS1 SIO1 Port         FTDI/CH341 Adapter
      ─────────────         ──────────────────
      Pin 2: GND    ──────  GND
      Pin 5: RXD    ──────  TXD   (crossed!)
      Pin 8: TXD    ──────  RXD   (crossed!)

      IMPORTANT: Use a 3.3V adapter only!
      5V will damage the PS1 SIO port.

    SHELL PROTOCOL
    ──────────────
      The kloader on PS1 runs a command server over SIO1.
      It sends "BK>>" beacons when ready for commands.
      All data is little-endian. Transfers use 2KB chunks
      with byte-sum checksums for reliability.

      Commands: PING, UEXE, UBIN, EXEC, BOOT, DUMP,
                PEEK, POKE, FAST, SLOW, REST

    EXAMPLES
    ────────
      # Upload and boot Linux kernel:
      blackroo-serial /dev/ttyUSB0 upload kernel.exe

      # Upload at high speed with custom cmdline:
      blackroo-serial /dev/ttyUSB0 upload kernel.exe --fast \\
        --cmdline "root=/dev/ram0 init=/bin/sh console=ttyS0,115200"

      # Read PS1 RAM size register:
      blackroo-serial /dev/ttyUSB0 peek 0x1F801060

      # Dump first 4KB of kernel memory:
      blackroo-serial /dev/ttyUSB0 dump 0x80010000 4096 kern.bin

      # Upload raw ramdisk to high memory:
      blackroo-serial /dev/ttyUSB0 bin initrd.img 0x80100000

      # Just connect to a running Linux kernel:
      blackroo-serial /dev/ttyUSB0 console

      # Find your serial port:
      blackroo-serial scan
    """))


# ── Command: sendexe — Upload PS-EXE to UniROM/compatible loader ──

def cmd_sendexe(ser, filepath):
    """
    Send a PS-EXE to any standard PS1 serial loader (UniROM, etc).
    Uses the standard SEXE serial protocol with optional V2 checksums.
    Once the bootloader is running, use 'upload' instead.
    """
    if not os.path.exists(filepath):
        print(f"  ERROR: File not found: {filepath}")
        return False

    with open(filepath, "rb") as f:
        data = f.read()

    if len(data) < PSEXE_HEADER_SIZE:
        print(f"  ERROR: File too small for PS-EXE ({len(data)} bytes)")
        return False

    if data[0:8] != PSEXE_MAGIC:
        print(f"  ERROR: Not a PS-EXE file (bad magic)")
        return False

    # Parse PS-EXE header
    pc0    = struct.unpack_from("<I", data, 0x10)[0]
    t_addr = struct.unpack_from("<I", data, 0x18)[0]
    t_size = struct.unpack_from("<I", data, 0x1C)[0]

    # Payload is everything after the 2048-byte header
    payload = data[PSEXE_HEADER_SIZE:]
    # Pad to match t_size
    if len(payload) < t_size:
        payload += b'\x00' * (t_size - len(payload))
    payload = payload[:t_size]

    # Pad payload to 2048-byte boundary for sector alignment
    pad_size = t_size
    if pad_size % CHUNK_SIZE:
        pad_size += CHUNK_SIZE - (pad_size % CHUNK_SIZE)
        payload += b'\x00' * (pad_size - t_size)

    # Checksum: byte sum of payload
    csum = checksum(payload[:t_size])

    print(f"  SEXE Upload (UniROM-compatible)")
    print(f"  File:   {os.path.basename(filepath)}")
    print(f"  Entry:  0x{pc0:08X}")
    print(f"  Load:   0x{t_addr:08X}")
    print(f"  Size:   {fmt_size(t_size)}")
    print(f"  Baud:   {ser.baudrate}")
    print()

    # Send SEXE command
    print("  Sending SEXE...")
    ser.reset_input_buffer()
    ser.write(b"SEXE")

    # Read response — skip echo if the loader mirrors input
    resp = ser.read(4)
    if resp == b"SEXE":
        resp = ser.read(4)
    protocol = 1

    if resp == b"OKV2":
        protocol = 2
        ser.write(b"UPV2")
        print("  Protocol: V2 (per-chunk checksums)")
        resp = ser.read(4)
    elif resp == b"OKV3":
        protocol = 3
        ser.write(b"UPV3")
        print("  Protocol: V3 (DJB2 checksums)")
        resp = ser.read(4)

    if resp != b"OKAY":
        if resp == b"UNSP":
            print("  ERROR: Command not supported (PS1 may be in debug mode)")
        elif resp:
            print(f"  ERROR: Unexpected response: {resp}")
        else:
            print("  ERROR: No response (is the loader listening?)")
        return False

    print("  Connected! Sending...")

    # Send full 2048-byte PS-EXE header sector
    header_sector = data[:PSEXE_HEADER_SIZE]
    if len(header_sector) < PSEXE_HEADER_SIZE:
        header_sector += b'\x00' * (PSEXE_HEADER_SIZE - len(header_sector))
    ser.write(header_sector)

    # Send metadata: entry, load address, data size, checksum
    send_u32(ser, pc0)
    send_u32(ser, t_addr)
    send_u32(ser, t_size)
    send_u32(ser, csum)

    # Send payload in 2048-byte chunks
    t0 = time.time()
    sent = 0
    last_pct = -1

    while sent < pad_size:
        chunk = payload[sent:sent + CHUNK_SIZE]
        ser.write(chunk)
        sent += len(chunk)

        # V2/V3: handle per-chunk checksum verification
        if protocol >= 2:
            chk_resp = ser.read(4)
            if chk_resp == b"CHEK":
                if protocol == 3:
                    # DJB2 hash
                    h = 5381
                    for b in chunk:
                        h = ((h << 5) + h) ^ b
                        h &= 0xFFFFFFFF
                    send_u32(ser, h)
                else:
                    # V2: simple byte sum
                    send_u32(ser, sum(chunk) & 0xFFFFFFFF)

                ack = ser.read(4)
                if ack == b"ERR!":
                    print(f"\n  ERROR: Chunk checksum failed at "
                          f"offset {sent - len(chunk)}")
                    return False
                # "MORE" means ok, continue

        pct = min((sent * 100) // pad_size, 100)
        if pct != last_pct:
            elapsed = time.time() - t0
            print(f"\r  {min(sent, t_size):>8} / {t_size} ({pct}%) "
                  f"[{fmt_rate(min(sent, t_size), elapsed)}]",
                  end="", flush=True)
            last_pct = pct

    elapsed = time.time() - t0
    print(f"\n  Done! {fmt_size(t_size)} in {elapsed:.1f}s "
          f"({fmt_rate(t_size, elapsed)})")
    print(f"  PS1 executing at 0x{pc0:08X}")
    return True


# ── Command: ping ─────────────────────────────────────────────

def cmd_ping(ser):
    if not wait_for_beacon(ser):
        return False
    send_u32(ser, CMD_PING)
    resp = recv_u32(ser, timeout=5)
    if resp == SHELL_PONG:
        print("  PONG — connection OK")
        return True
    else:
        print(f"  ERROR: expected PONG, got "
              f"{tag_str(resp) if resp else 'timeout'}")
        return False


# ── Command: upload ───────────────────────────────────────────

def cmd_upload(ser, filepath, boot=True, console=True, cmdline=None):
    """Upload PS-EXE to the Blackroo shell."""
    if not os.path.exists(filepath):
        print(f"  ERROR: File not found: {filepath}")
        return False

    with open(filepath, "rb") as f:
        data = f.read()

    if len(data) < PSEXE_HEADER_SIZE:
        print(f"  ERROR: File too small for PS-EXE ({len(data)} bytes)")
        return False

    if data[0:8] != PSEXE_MAGIC:
        print(f"  ERROR: Not a PS-EXE file (bad magic)")
        return False

    # Parse PS-EXE header
    pc0    = struct.unpack_from("<I", data, 0x10)[0]
    t_addr = struct.unpack_from("<I", data, 0x18)[0]
    t_size = struct.unpack_from("<I", data, 0x1C)[0]

    # Extract payload (skip 2048-byte header)
    payload = data[PSEXE_HEADER_SIZE:PSEXE_HEADER_SIZE + t_size]
    if len(payload) < t_size:
        payload += b'\x00' * (t_size - len(payload))

    csum = checksum(payload)

    print(f"  File:   {os.path.basename(filepath)}")
    print(f"  Entry:  0x{pc0:08X}")
    print(f"  Load:   0x{t_addr:08X}")
    print(f"  Size:   {fmt_size(t_size)}")
    print(f"  Baud:   {ser.baudrate}")
    print()

    # Connect to shell
    if not wait_for_beacon(ser):
        return False

    vprint(f"Checksum: 0x{csum:08X}")
    vprint(f"Sending UEXE command...")

    # Send UEXE command
    resp = send_command(ser, CMD_UEXE)
    if resp != SHELL_OK:
        print(f"  ERROR: UEXE rejected "
              f"({tag_str(resp) if resp else 'timeout'})")
        if VERBOSE and resp is None:
            # Dump whatever is in the buffer
            time.sleep(0.5)
            stale = ser.in_waiting
            if stale:
                raw = ser.read(stale)
                vprint(f"Buffer after timeout: {hexdump_bytes(raw)}")
        return False

    vprint("UEXE accepted, sending header...")

    # Send header: entry, load, size, checksum
    send_u32(ser, pc0)
    send_u32(ser, t_addr)
    send_u32(ser, t_size)
    send_u32(ser, csum)

    resp = recv_u32(ser, timeout=5)
    if resp != SHELL_OK:
        print(f"  ERROR: Header rejected "
              f"({tag_str(resp) if resp else 'timeout'})")
        return False

    vprint("Header accepted, starting data transfer...")
    print(f"  Sending {fmt_size(t_size)}...")

    t0 = time.time()
    sent = 0
    last_pct = -1

    while sent < t_size:
        chunk_len = min(t_size - sent, CHUNK_SIZE)
        chunk = payload[sent:sent + chunk_len]
        ser.write(chunk)

        resp = recv_u32(ser, timeout=10)
        if resp != SHELL_MORE:
            print(f"\n  ERROR: Chunk failed at offset {sent} "
                  f"({tag_str(resp) if resp else 'timeout'})")
            if VERBOSE and resp is None:
                time.sleep(0.5)
                stale = ser.in_waiting
                if stale:
                    raw = ser.read(stale)
                    vprint(f"Buffer after chunk timeout: "
                           f"{hexdump_bytes(raw)}")
            return False

        sent += chunk_len
        pct = (sent * 100) // t_size
        if pct != last_pct:
            elapsed = time.time() - t0
            if not VERBOSE:
                print(f"\r  {sent:>8} / {t_size} ({pct}%) "
                      f"[{fmt_rate(sent, elapsed)}]",
                      end="", flush=True)
            else:
                vprint(f"Chunk {sent//CHUNK_SIZE}: "
                       f"{sent}/{t_size} ({pct}%)")
            last_pct = pct

    resp = recv_u32(ser, timeout=10)
    elapsed = time.time() - t0

    if resp == SHELL_DONE:
        print(f"\n  OK: {fmt_size(t_size)} in {elapsed:.1f}s "
              f"({fmt_rate(t_size, elapsed)})")
    elif resp == SHELL_ERR:
        print(f"\n  ERROR: Checksum mismatch!")
        return False
    else:
        print(f"\n  WARNING: Unexpected response: "
              f"{tag_str(resp) if resp else 'timeout'}")
        return False

    if boot:
        print()
        return cmd_boot(ser, console=console, cmdline=cmdline)

    return True


# ── Command: bin ──────────────────────────────────────────────

def cmd_bin(ser, filepath, addr):
    """Upload raw binary to a specified address."""
    if not os.path.exists(filepath):
        print(f"  ERROR: File not found: {filepath}")
        return False

    with open(filepath, "rb") as f:
        data = f.read()

    csum = checksum(data)
    size = len(data)

    print(f"  File:   {os.path.basename(filepath)}")
    print(f"  Addr:   0x{addr:08X}")
    print(f"  Size:   {fmt_size(size)}")
    print()

    if not wait_for_beacon(ser):
        return False

    resp = send_command(ser, CMD_UBIN)
    if resp != SHELL_OK:
        print(f"  ERROR: UBIN rejected")
        return False

    send_u32(ser, addr)
    send_u32(ser, size)
    send_u32(ser, csum)

    resp = recv_u32(ser, timeout=5)
    if resp != SHELL_OK:
        print(f"  ERROR: Header rejected")
        return False

    print(f"  Sending {fmt_size(size)}...")

    t0 = time.time()
    sent = 0
    last_pct = -1

    while sent < size:
        chunk_len = min(size - sent, CHUNK_SIZE)
        chunk = data[sent:sent + chunk_len]
        ser.write(chunk)

        resp = recv_u32(ser, timeout=10)
        if resp != SHELL_MORE:
            print(f"\n  ERROR: Chunk failed at offset {sent}")
            return False

        sent += chunk_len
        pct = (sent * 100) // size
        if pct != last_pct:
            elapsed = time.time() - t0
            print(f"\r  {sent:>8} / {size} ({pct}%) "
                  f"[{fmt_rate(sent, elapsed)}]",
                  end="", flush=True)
            last_pct = pct

    resp = recv_u32(ser, timeout=10)
    elapsed = time.time() - t0

    if resp == SHELL_DONE:
        print(f"\n  OK: {fmt_size(size)} in {elapsed:.1f}s")
        return True
    else:
        print(f"\n  ERROR: Transfer failed "
              f"({tag_str(resp) if resp else 'timeout'})")
        return False


# ── Command: boot ─────────────────────────────────────────────

def cmd_boot(ser, console=True, cmdline=None):
    """Send BOOT command to launch uploaded kernel."""
    if not wait_for_beacon(ser):
        return False

    resp = send_command(ser, CMD_BOOT)
    if resp == SHELL_ERR:
        print("  ERROR: No kernel uploaded — upload first")
        return False
    if resp != SHELL_OK:
        print(f"  ERROR: BOOT failed "
              f"({tag_str(resp) if resp else 'timeout'})")
        return False

    if cmdline:
        cmdline_bytes = cmdline.encode("ascii")
        send_u32(ser, len(cmdline_bytes))
        ser.write(cmdline_bytes)
        print(f"  Cmdline: {cmdline}")
    else:
        send_u32(ser, 0)
        print("  Cmdline: (using PS1 settings)")

    print("  Kernel launched!")

    if console:
        if ser.baudrate != BAUD_SLOW:
            print(f"  Switching to {BAUD_SLOW} baud for kernel console...")
            ser.baudrate = BAUD_SLOW
            time.sleep(0.5)

        print("  Entering console (Ctrl+] to exit)...\n")
        cmd_console(ser)

    return True


# ── Command: dump ─────────────────────────────────────────────

def cmd_dump(ser, addr, size, outfile=None):
    """Dump memory from PS1 to host."""
    print(f"  Address: 0x{addr:08X}")
    print(f"  Size:    {fmt_size(size)}")
    print()

    if not wait_for_beacon(ser):
        return False

    resp = send_command(ser, CMD_DUMP)
    if resp != SHELL_OK:
        print(f"  ERROR: DUMP rejected")
        return False

    send_u32(ser, addr)
    send_u32(ser, size)

    print(f"  Receiving...")

    t0 = time.time()
    data = bytearray()
    received = 0
    last_pct = -1

    while received < size:
        chunk_len = min(size - received, CHUNK_SIZE)
        chunk = ser.read(chunk_len)
        if len(chunk) == 0:
            print(f"\n  ERROR: Timeout at {received}/{size}")
            return False
        data.extend(chunk)
        received += len(chunk)

        if received % CHUNK_SIZE == 0 or received >= size:
            send_u32(ser, SHELL_MORE)

        pct = (received * 100) // size
        if pct != last_pct:
            print(f"\r  {received:>8} / {size} ({pct}%)",
                  end="", flush=True)
            last_pct = pct

    remote_csum = recv_u32(ser, timeout=5)
    local_csum = checksum(data)
    elapsed = time.time() - t0

    if remote_csum == local_csum:
        print(f"\n  OK: {fmt_size(size)} in {elapsed:.1f}s "
              f"(checksum match)")
    else:
        print(f"\n  WARNING: Checksum mismatch "
              f"(local=0x{local_csum:08X} "
              f"remote=0x{remote_csum:08X if remote_csum else 0:08X})")

    if outfile:
        with open(outfile, "wb") as f:
            f.write(data)
        print(f"  Saved to {outfile}")
    else:
        for i in range(0, min(len(data), 256), 16):
            hexpart = " ".join(f"{b:02X}" for b in data[i:i+16])
            ascpart = "".join(
                chr(b) if 0x20 <= b < 0x7F else "."
                for b in data[i:i+16])
            print(f"  {addr+i:08X}  {hexpart:<48s}  {ascpart}")
        if len(data) > 256:
            print(f"  ... ({len(data) - 256} more bytes, "
                  f"specify output file to save all)")

    return True


# ── Command: peek ─────────────────────────────────────────────

def cmd_peek(ser, addr):
    if not wait_for_beacon(ser):
        return False

    resp = send_command(ser, CMD_PEEK)
    if resp != SHELL_OK:
        print(f"  ERROR: PEEK rejected")
        return False

    send_u32(ser, addr)
    val = recv_u32(ser, timeout=5)

    if val is not None:
        print(f"  [0x{addr:08X}] = 0x{val:08X} ({val})")
        return True
    else:
        print(f"  ERROR: No response")
        return False


# ── Command: poke ─────────────────────────────────────────────

def cmd_poke(ser, addr, val):
    if not wait_for_beacon(ser):
        return False

    resp = send_command(ser, CMD_POKE)
    if resp != SHELL_OK:
        print(f"  ERROR: POKE rejected")
        return False

    send_u32(ser, addr)
    send_u32(ser, val)

    resp = recv_u32(ser, timeout=5)
    if resp == SHELL_DONE:
        print(f"  [0x{addr:08X}] <- 0x{val:08X}")
        return True
    else:
        print(f"  ERROR: POKE failed")
        return False


# ── Command: exec ─────────────────────────────────────────────

def cmd_exec(ser, addr):
    if not wait_for_beacon(ser):
        return False

    resp = send_command(ser, CMD_EXEC)
    if resp != SHELL_OK:
        print(f"  ERROR: EXEC rejected")
        return False

    send_u32(ser, addr)
    print(f"  Jumping to 0x{addr:08X}")
    return True


# ── Command: console ──────────────────────────────────────────

def cmd_console(ser):
    """Interactive serial console. Ctrl+] to exit."""
    print(f"Blackroo Console — {ser.port} @ {ser.baudrate}")
    print(f"  Ctrl+] exit | Ctrl+B break")
    print(f"  {'─' * 40}")

    old_settings = termios.tcgetattr(sys.stdin)

    try:
        tty.setraw(sys.stdin.fileno())

        while True:
            if ser.in_waiting:
                data = ser.read(ser.in_waiting)
                for b in data:
                    if b == 0x0D:
                        sys.stdout.write('\r')
                    elif b == 0x0A:
                        sys.stdout.write('\n')
                    elif 0x20 <= b < 0x7F or b == 0x09:
                        sys.stdout.write(chr(b))
                    elif b == 0x08:
                        sys.stdout.write('\b \b')
                    else:
                        sys.stdout.write(f'[{b:02X}]')
                sys.stdout.flush()

            if select.select([sys.stdin], [], [], 0.01)[0]:
                ch = sys.stdin.read(1)
                if ch == '\x1d':  # Ctrl+]
                    print("\r\n[Disconnected]")
                    break
                elif ch == '\x02':  # Ctrl+B
                    ser.send_break(0.25)
                    print("\r\n[BREAK]")
                else:
                    ser.write(ch.encode('latin-1'))

    except KeyboardInterrupt:
        print("\r\n[Interrupted]")
    finally:
        termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old_settings)


# ── Command: monitor ──────────────────────────────────────────

def cmd_monitor(ser):
    """Hex dump monitor — display all received bytes."""
    print(f"Monitor — {ser.port} @ {ser.baudrate}")
    print(f"  Ctrl+C to exit")
    print(f"  {'─' * 40}")

    offset = 0
    line_buf = bytearray()

    try:
        while True:
            if ser.in_waiting:
                data = ser.read(ser.in_waiting)
                for b in data:
                    line_buf.append(b)
                    if len(line_buf) >= 16:
                        hexpart = " ".join(f"{x:02X}" for x in line_buf)
                        ascpart = "".join(
                            chr(x) if 0x20 <= x < 0x7F else "."
                            for x in line_buf)
                        print(f"  {offset:06X}  {hexpart:<48s}  "
                              f"{ascpart}")
                        offset += len(line_buf)
                        line_buf = bytearray()
            else:
                if line_buf:
                    time.sleep(0.1)
                    if not ser.in_waiting:
                        hexpart = " ".join(f"{x:02X}" for x in line_buf)
                        ascpart = "".join(
                            chr(x) if 0x20 <= x < 0x7F else "."
                            for x in line_buf)
                        print(f"  {offset:06X}  {hexpart:<48s}  "
                              f"{ascpart}")
                        offset += len(line_buf)
                        line_buf = bytearray()
                else:
                    time.sleep(0.01)

    except KeyboardInterrupt:
        print(f"\n  Total: {offset + len(line_buf)} bytes")


# ── Command: diag ─────────────────────────────────────────────

def cmd_diag(ser):
    """Run connection diagnostics — beacon check, ping, buffer dump."""
    global VERBOSE
    old_verbose = VERBOSE
    VERBOSE = True

    print("  === BLACKROO SERIAL DIAGNOSTICS ===\n")

    # 1. Check what's on the line right now
    print("  [1] Raw line check (2 seconds)...")
    ser.reset_input_buffer()
    time.sleep(2)
    raw_count = ser.in_waiting
    if raw_count > 0:
        raw = ser.read(raw_count)
        print(f"      Got {raw_count} bytes:")
        print(f"      {hexdump_bytes(raw)}")
        # Check for beacons in raw data
        beacon_bytes = struct.pack("<I", SHELL_BEACON)
        count = 0
        idx = 0
        while True:
            pos = raw.find(beacon_bytes, idx)
            if pos < 0:
                break
            count += 1
            idx = pos + 4
        if count:
            print(f"      Found {count} BK>> beacon(s) — "
                  f"shell is running!")
        else:
            print(f"      No BK>> beacons found in raw data")
    else:
        print(f"      No data received — is shell running on PS1?")
    print()

    # 2. Beacon + ping test
    print("  [2] Beacon + Ping test...")
    if wait_for_beacon(ser, timeout=10):
        print("      Sending PING...")
        send_u32(ser, CMD_PING)
        resp = recv_u32(ser, timeout=5)
        if resp == SHELL_PONG:
            print("      PONG received — bidirectional OK!\n")
        else:
            print(f"      Expected PONG, got: "
                  f"{tag_str(resp) if resp else 'TIMEOUT'}")
            # Dump buffer
            time.sleep(0.3)
            leftover = ser.in_waiting
            if leftover:
                extra = ser.read(leftover)
                print(f"      Buffer: {hexdump_bytes(extra)}")
            print()
    else:
        print()

    # 3. Try UEXE handshake (no actual upload — just see if PS1
    #    recognizes the command)
    print("  [3] UEXE handshake test (no data)...")
    if wait_for_beacon(ser, timeout=10):
        print("      Sending UEXE tag...")
        send_u32(ser, CMD_UEXE)
        resp = recv_u32(ser, timeout=5)
        if resp == SHELL_OK:
            print("      OKOK received — PS1 recognized UEXE!")
            # PS1 is now waiting for header. Send zeros to
            # satisfy recv_u32 calls, then it'll fail checksum
            # and we'll get ERR — that's fine for diag.
            print("      Sending dummy header (will cause ERR)...")
            send_u32(ser, 0)  # entry
            send_u32(ser, 0)  # load_addr
            send_u32(ser, 0)  # size=0
            send_u32(ser, 0)  # checksum
            resp2 = recv_u32(ser, timeout=5)
            print(f"      Header response: "
                  f"{tag_str(resp2) if resp2 else 'TIMEOUT'}")
        elif resp is None:
            print("      TIMEOUT — PS1 did not respond to UEXE")
            time.sleep(0.5)
            leftover = ser.in_waiting
            if leftover:
                extra = ser.read(leftover)
                print(f"      Buffer: {hexdump_bytes(extra)}")
        else:
            print(f"      Unexpected: {tag_str(resp)}")
    print()

    print("  === DIAGNOSTICS COMPLETE ===")
    VERBOSE = old_verbose


# ── Command: scan ─────────────────────────────────────────────

def cmd_scan():
    """Scan for available serial ports."""
    ports = serial.tools.list_ports.comports()
    if not ports:
        print("No serial ports found.")
        print("  Check USB cable and adapter driver "
              "(ftdi_sio, ch341, cp210x)")
        return

    print(f"Found {len(ports)} serial port(s):\n")
    for p in ports:
        print(f"  {p.device}")
        print(f"    {p.description}")
        if p.manufacturer:
            print(f"    Manufacturer: {p.manufacturer}")
        if p.vid is not None:
            print(f"    USB: {p.vid:04X}:{p.pid:04X}")

        desc = (p.description or "").lower()
        if any(x in desc for x in
               ["ftdi", "ft232", "ch340", "cp210", "uart"]):
            print(f"    ** Likely PS1 serial adapter **")
        print()


# ── Command: flash-dump ──────────────────────────────────────

def cmd_flash_dump(ser, outfile):
    """Receive PIO flash dump from bootloader (BKFL protocol)."""
    print(f"PIO Flash Dump")
    print(f"  Waiting for BKFL header...")
    print(f"  (Select 'Dump to Serial' on PS1)")
    print()

    hdr_data = ser.read(PIOFLASH_HDR_SIZE)
    if len(hdr_data) < PIOFLASH_HDR_SIZE:
        print(f"  ERROR: Header timeout")
        return False

    magic, size, mfr_id, dev_id = struct.unpack(
        "<IIBBxxxxxx", hdr_data)
    if magic != PIOFLASH_MAGIC:
        print(f"  ERROR: Bad magic 0x{magic:08X}")
        return False

    chip_key = (mfr_id, dev_id)
    if chip_key in KNOWN_CHIPS:
        mfr, dev, _ = KNOWN_CHIPS[chip_key]
        print(f"  Chip: {mfr} {dev}")
    else:
        print(f"  Chip: Unknown (0x{mfr_id:02X}:0x{dev_id:02X})")

    print(f"  Size: {fmt_size(size)}")
    print(f"  Receiving...")

    t0 = time.time()
    data = bytearray()
    last_pct = -1

    while len(data) < size:
        chunk = ser.read(min(size - len(data), 4096))
        if not chunk:
            print(f"\n  ERROR: Timeout at {fmt_size(len(data))}")
            return False
        data.extend(chunk)

        pct = (len(data) * 100) // size
        if pct != last_pct:
            elapsed = time.time() - t0
            print(f"\r  {len(data):>8} / {size} ({pct}%) "
                  f"[{fmt_rate(len(data), elapsed)}]",
                  end="", flush=True)
            last_pct = pct

    elapsed = time.time() - t0
    with open(outfile, "wb") as f:
        f.write(data)

    print(f"\n  Saved {fmt_size(size)} to {outfile} ({elapsed:.1f}s)")
    return True


# ── Command: flash-load ──────────────────────────────────────

def cmd_flash_load(ser, infile):
    """Send ROM image to PS1 PIO flash (BKFL protocol)."""
    if not os.path.exists(infile):
        print(f"  ERROR: File not found: {infile}")
        return False

    with open(infile, "rb") as f:
        data = f.read()

    size = len(data)
    print(f"PIO Flash Program")
    print(f"  File: {infile} ({fmt_size(size)})")
    print(f"  Select 'Load from Serial' on PS1.")
    print(f"  Sending in 3s...")
    time.sleep(3)

    hdr = struct.pack("<IIBBxxxxxx", PIOFLASH_MAGIC, size, 0, 0)
    ser.write(hdr)

    print(f"  Sending {fmt_size(size)}...")

    t0 = time.time()
    sent = 0
    last_pct = -1

    for i in range(0, size, 256):
        chunk = data[i:i + 256]
        ser.write(chunk)
        sent += len(chunk)

        pct = (sent * 100) // size
        if pct != last_pct:
            elapsed = time.time() - t0
            print(f"\r  {sent:>8} / {size} ({pct}%) "
                  f"[{fmt_rate(sent, elapsed)}]",
                  end="", flush=True)
            last_pct = pct
        time.sleep(0.001)

    elapsed = time.time() - t0
    print(f"\n  Sent. Waiting for verification...")

    ser.timeout = 120
    resp = ser.read(1)
    if resp == b'V':
        print(f"  SUCCESS — flash verified!")
        return True
    elif resp:
        print(f"  WARNING: Got '{resp}' (expected 'V')")
        return False
    else:
        print(f"  WARNING: Timeout")
        return False


# ── Main ──────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        prog="blackroo-serial",
        description=BANNER,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Run 'blackroo-serial help' for full documentation.")

    parser.add_argument("port", nargs="?",
                        help="Serial port (e.g. /dev/ttyUSB0)")
    parser.add_argument("command", nargs="?", default="console",
                        help="Command (default: console)")
    parser.add_argument("args", nargs="*",
                        help="Command arguments")
    parser.add_argument("--baud", type=int, default=BAUD_SLOW,
                        help=f"Baud rate (default: {BAUD_SLOW})")
    parser.add_argument("--fast", action="store_true",
                        help=f"Use {BAUD_FAST} baud")
    parser.add_argument("--no-boot", action="store_true",
                        help="Upload only, don't boot")
    parser.add_argument("--no-console", action="store_true",
                        help="Don't enter console after boot")
    parser.add_argument("--cmdline", type=str, default=None,
                        help="Override kernel command line")
    parser.add_argument("--timeout", type=int, default=10,
                        help="Timeout in seconds (default: 10)")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Show detailed protocol diagnostics")
    parser.add_argument("--version", action="version",
                        version=f"blackroo-serial v{VERSION}")

    args = parser.parse_args()

    # Handle commands that don't need a serial port
    if args.port == "help" or args.command == "help":
        cmd_help()
        return

    if args.port == "scan" or args.command == "scan":
        cmd_scan()
        return

    if not args.port:
        print(BANNER)
        print()
        print("Usage: blackroo-serial <port> <command> [args]")
        print()
        print("  blackroo-serial help               Full guide")
        print("  blackroo-serial scan               Find ports")
        print("  blackroo-serial PORT sendexe FILE   Send to UniROM")
        print("  blackroo-serial PORT upload FILE    Upload + boot")
        print("  blackroo-serial PORT console        Serial terminal")
        print("  blackroo-serial PORT ping           Test connection")
        print("  blackroo-serial PORT diag           Full diagnostics")
        print("  blackroo-serial PORT upload FILE -v Verbose upload")
        print()
        sys.exit(1)

    global VERBOSE
    VERBOSE = args.verbose
    baud = BAUD_FAST if args.fast else args.baud

    print(BANNER)
    print(f"  Port: {args.port}  Baud: {baud}")
    if VERBOSE:
        print(f"  Verbose mode ON")
    print()

    try:
        ser = open_serial(args.port, baud, args.timeout)
    except serial.SerialException as e:
        print(f"ERROR: Cannot open {args.port}: {e}")
        print(f"  Check cable and permissions "
              f"(try: sudo chmod 666 {args.port})")
        sys.exit(1)

    try:
        cmd = args.command

        if cmd == "console":
            cmd_console(ser)

        elif cmd == "upload":
            if not args.args:
                print("Usage: blackroo-serial PORT upload <file.exe>")
                sys.exit(1)
            # The return value used to be discarded, so `upload` exited 0
            # even when it printed "ERROR: No beacon received" and sent
            # nothing. Any script driving this tool was told every upload
            # succeeded. cmd_upload() already returns True/False correctly;
            # it just has to reach the exit code.
            if not cmd_upload(ser, args.args[0],
                              boot=not args.no_boot,
                              console=not args.no_console,
                              cmdline=args.cmdline):
                sys.exit(1)

        elif cmd == "bin":
            if len(args.args) < 2:
                print("Usage: blackroo-serial PORT bin <file> <addr>")
                print("  Example: blackroo-serial PORT bin blob.bin "
                      "0x80100000")
                sys.exit(1)
            cmd_bin(ser, args.args[0], parse_int(args.args[1]))

        elif cmd == "boot":
            cmd_boot(ser,
                     console=not args.no_console,
                     cmdline=args.cmdline)

        elif cmd == "ping":
            cmd_ping(ser)

        elif cmd == "peek":
            if not args.args:
                print("Usage: blackroo-serial PORT peek <addr>")
                sys.exit(1)
            cmd_peek(ser, parse_int(args.args[0]))

        elif cmd == "poke":
            if len(args.args) < 2:
                print("Usage: blackroo-serial PORT poke <addr> <value>")
                sys.exit(1)
            cmd_poke(ser, parse_int(args.args[0]),
                     parse_int(args.args[1]))

        elif cmd == "dump":
            if len(args.args) < 2:
                print("Usage: blackroo-serial PORT dump <addr> <size> "
                      "[outfile]")
                sys.exit(1)
            outfile = args.args[2] if len(args.args) > 2 else None
            cmd_dump(ser, parse_int(args.args[0]),
                     parse_int(args.args[1]), outfile)

        elif cmd == "exec":
            if not args.args:
                print("Usage: blackroo-serial PORT exec <addr>")
                sys.exit(1)
            cmd_exec(ser, parse_int(args.args[0]))

        elif cmd == "sendexe":
            if not args.args:
                print("Usage: blackroo-serial PORT sendexe <file.exe>")
                print("  Sends PS-EXE to UniROM or compatible loader")
                sys.exit(1)
            cmd_sendexe(ser, args.args[0])

        elif cmd == "diag":
            cmd_diag(ser)

        elif cmd == "monitor":
            cmd_monitor(ser)

        elif cmd == "flash-dump":
            if not args.args:
                print("Usage: blackroo-serial PORT flash-dump <out.bin>")
                sys.exit(1)
            cmd_flash_dump(ser, args.args[0])

        elif cmd == "flash-load":
            if not args.args:
                print("Usage: blackroo-serial PORT flash-load <in.bin>")
                sys.exit(1)
            cmd_flash_load(ser, args.args[0])

        else:
            print(f"Unknown command: {cmd}")
            print("Run 'blackroo-serial help' for usage.")
            sys.exit(1)

    finally:
        ser.close()


if __name__ == "__main__":
    main()
