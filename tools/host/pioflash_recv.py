#!/usr/bin/env python3
"""
pioflash_recv.py — Receive PIO flash dump from Blackroo bootloader

Connects to PS1 serial port and receives a flash ROM dump sent by the
bootloader's PIO Flash Manager "Dump to Serial" function.

Usage:
    python3 pioflash_recv.py /dev/ttyUSB0 output.bin
    python3 pioflash_recv.py COM3 gameshark_backup.bin

Protocol: Receives BKFL header (16 bytes) + raw flash data.

Attribution: New Blackroo work (2026, GPL v2)
"""

import sys
import struct
import serial
import time

PIOFLASH_MAGIC = 0x4C464B42  # "BKFL" little-endian
HEADER_SIZE = 16
BAUD_RATE = 115200

# Known chip database (mirrors pioflash.c)
KNOWN_CHIPS = {
    (0xBF, 0xB5): ("SST", "SST39SF010", 128),
    (0xBF, 0xB6): ("SST", "SST39SF020", 256),
    (0xBF, 0xB7): ("SST", "SST39SF040", 512),
    (0xBF, 0x10): ("SST", "SST29EE020", 256),
    (0x01, 0x20): ("AMD", "AM29F010", 128),
    (0x01, 0xB0): ("AMD", "AM29F020", 256),
    (0x01, 0xA4): ("AMD", "AM29F040", 512),
    (0x01, 0xD5): ("AMD", "AM29F080", 1024),
    (0xDA, 0x45): ("Winbond", "W29C020", 256),
    (0xDA, 0x46): ("Winbond", "W29C040", 512),
    (0x1F, 0xDA): ("Atmel", "AT29C020", 256),
    (0xC2, 0xB0): ("Macronix", "MX29F020", 256),
    (0xC2, 0xA4): ("Macronix", "MX29F040", 512),
}


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <serial_port> <output_file>")
        print(f"  e.g. {sys.argv[0]} /dev/ttyUSB0 gameshark.bin")
        sys.exit(1)

    port = sys.argv[1]
    outfile = sys.argv[2]

    print(f"Connecting to {port} at {BAUD_RATE} baud...")
    ser = serial.Serial(port, BAUD_RATE, timeout=30)
    time.sleep(0.5)

    print("Waiting for BKFL header from PS1...")
    print("(Press Start on the PS1 to begin dump)")

    # Read header
    hdr_data = ser.read(HEADER_SIZE)
    if len(hdr_data) < HEADER_SIZE:
        print(f"ERROR: Only received {len(hdr_data)} header bytes (expected {HEADER_SIZE})")
        sys.exit(1)

    magic, size, mfr_id, dev_id = struct.unpack("<IIBBxxxxxx", hdr_data)

    if magic != PIOFLASH_MAGIC:
        print(f"ERROR: Bad magic 0x{magic:08X} (expected 0x{PIOFLASH_MAGIC:08X})")
        sys.exit(1)

    chip_key = (mfr_id, dev_id)
    if chip_key in KNOWN_CHIPS:
        mfr_name, dev_name, _ = KNOWN_CHIPS[chip_key]
        chip_str = f"{mfr_name} {dev_name}"
    else:
        chip_str = f"Unknown (MFR:0x{mfr_id:02X} DEV:0x{dev_id:02X})"

    print(f"Flash: {chip_str}")
    print(f"Size:  {size} bytes ({size // 1024} KB)")
    print(f"Receiving data...")

    # Receive flash data
    data = bytearray()
    last_pct = -1
    while len(data) < size:
        remaining = size - len(data)
        chunk = ser.read(min(remaining, 4096))
        if not chunk:
            print(f"\nERROR: Timeout after {len(data)} bytes")
            sys.exit(1)
        data.extend(chunk)

        pct = (len(data) * 100) // size
        if pct != last_pct:
            print(f"\r  {len(data):>7} / {size} bytes ({pct}%)", end="", flush=True)
            last_pct = pct

    print(f"\n\nReceived {len(data)} bytes")

    # Write to file
    with open(outfile, "wb") as f:
        f.write(data)

    print(f"Saved to {outfile}")
    print("Done!")

    ser.close()


if __name__ == "__main__":
    main()
