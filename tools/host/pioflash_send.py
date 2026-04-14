#!/usr/bin/env python3
"""
pioflash_send.py — Send ROM image to PS1 PIO flash via Blackroo bootloader

Sends a binary file to the PS1 bootloader's PIO Flash Manager
"Load from Serial" function, which programs it into the cartridge flash.

Usage:
    python3 pioflash_send.py /dev/ttyUSB0 firmware.bin
    python3 pioflash_send.py COM3 gameshark_backup.bin

Protocol: Sends BKFL header (16 bytes) + raw data.
The bootloader erases and programs the flash, then sends 'V' on success.

Attribution: New Blackroo work (2026, GPL v2)
"""

import sys
import struct
import serial
import time
import os

PIOFLASH_MAGIC = 0x4C464B42  # "BKFL" little-endian
HEADER_SIZE = 16
BAUD_RATE = 115200


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <serial_port> <input_file>")
        print(f"  e.g. {sys.argv[0]} /dev/ttyUSB0 firmware.bin")
        sys.exit(1)

    port = sys.argv[1]
    infile = sys.argv[2]

    # Read input file
    if not os.path.exists(infile):
        print(f"ERROR: File not found: {infile}")
        sys.exit(1)

    with open(infile, "rb") as f:
        data = f.read()

    size = len(data)
    print(f"File:  {infile}")
    print(f"Size:  {size} bytes ({size // 1024} KB)")

    if size > 1024 * 1024:
        print("ERROR: File too large (max 1MB)")
        sys.exit(1)

    print(f"Connecting to {port} at {BAUD_RATE} baud...")
    ser = serial.Serial(port, BAUD_RATE, timeout=60)
    time.sleep(0.5)

    print("Select 'Load from Serial' on PS1 and press Start...")
    print("Waiting 3 seconds then sending...")
    time.sleep(3)

    # Build and send header
    hdr = struct.pack("<IIBBxxxxxx", PIOFLASH_MAGIC, size, 0, 0)
    ser.write(hdr)

    print(f"Sending {size} bytes...")

    # Send data in chunks
    chunk_size = 256
    sent = 0
    last_pct = -1
    for i in range(0, size, chunk_size):
        chunk = data[i:i + chunk_size]
        ser.write(chunk)
        sent += len(chunk)

        pct = (sent * 100) // size
        if pct != last_pct:
            print(f"\r  {sent:>7} / {size} bytes ({pct}%)", end="", flush=True)
            last_pct = pct

        # Small delay to avoid overrunning the PS1's serial buffer
        # At 115200 the PS1 programs each byte as it arrives
        time.sleep(0.001)

    print(f"\n\nData sent. Waiting for verification...")

    # Wait for verification response
    ser.timeout = 120  # Programming can take a while
    resp = ser.read(1)
    if resp == b'V':
        print("SUCCESS! Flash programmed and verified.")
    elif resp:
        print(f"WARNING: Got response '{resp}' (expected 'V')")
    else:
        print("WARNING: No verification response (timeout)")

    ser.close()
    print("Done!")


if __name__ == "__main__":
    main()
