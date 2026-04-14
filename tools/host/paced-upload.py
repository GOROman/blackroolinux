#!/usr/bin/env python3
"""
paced-upload.py — upload a PS-EXE to kloader's serial shell, slowly enough
that kloader can keep up.

Why this exists: kloader's `serial_getchar()` (bootloader/src/serial.c) polls
SIO1 RXRDY and **never deasserts RTS**, so it has no way to tell the host to
wait. At line rate the host can push 2 KB straight into a console that pauses
briefly for a VBlank IRQ or controller poll — bytes are lost, kloader then
blocks waiting for data that already went by, and the machine appears to
freeze. `blackroo-serial.py upload` dies this way part-way through a large
file.

The proper fix is RTS throttling in kloader's receive path, mirroring what the
kernel's own driver does (`sio_rtscts_get_buf()` in arch/mipsnommu/ps/siocon.c
drops RTS after each byte). That needs a kloader rebuild, which needs PSn00bSDK.

Until then: pace the host. Write a small burst, pause, repeat — the PS1's
8-byte SIO1 RX FIFO covers any stall shorter than the pause.

Usage:
    paced-upload.py PORT FILE.exe [--burst N] [--gap MS] [--no-boot]

Attribution: New Blackroo work (2026, GPL v2)
"""

import argparse
import struct
import sys
import time

import serial


def tag(s):
    return struct.unpack("<I", s.encode("ascii"))[0]


BEACON = tag("BK>>")
OK     = tag("OKOK")
MORE   = tag("MORE")
DONE   = tag("DONE")
FAIL   = tag("FAIL")
UEXE   = tag("UEXE")
BOOT   = tag("BOOT")
CHUNK  = 2048


def rd_u32(ser, timeout=10):
    ser.timeout = timeout
    b = ser.read(4)
    if len(b) != 4:
        return None
    return struct.unpack("<I", b)[0]


def name(v):
    if v is None:
        return "timeout"
    try:
        return struct.pack("<I", v).decode("ascii")
    except Exception:
        return f"0x{v:08X}"


def wait_beacon(ser, timeout=30):
    print("  waiting for BK>> beacon...")
    want = struct.pack("<I", BEACON)
    buf = b""
    end = time.time() + timeout
    ser.timeout = 0.3
    while time.time() < end:
        buf += ser.read(256)
        if want in buf:
            print("  beacon received")
            time.sleep(0.2)
            ser.reset_input_buffer()
            return True
    print("  ERROR: no beacon — is the serial shell running on the PS1?")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("file")
    ap.add_argument("--burst", type=int, default=8,
                    help="bytes per write (default 8 = SIO1 FIFO depth)")
    ap.add_argument("--gap", type=float, default=2.0,
                    help="pause between bursts in ms (default 2.0)")
    ap.add_argument("--no-boot", action="store_true")
    args = ap.parse_args()

    data = open(args.file, "rb").read()
    if data[:8] != b"PS-X EXE":
        sys.exit("not a PS-EXE")
    pc0    = struct.unpack_from("<I", data, 0x10)[0]
    t_addr = struct.unpack_from("<I", data, 0x18)[0]
    t_size = struct.unpack_from("<I", data, 0x1C)[0]
    payload = data[2048:2048 + t_size]
    csum = sum(payload) & 0xFFFFFFFF

    rate = args.burst / ((args.burst * 8 / 115200) + args.gap / 1000.0)
    print(f"  {args.file}: entry 0x{pc0:08X} load 0x{t_addr:08X} "
          f"{t_size/1024:.0f} KB")
    print(f"  pacing: {args.burst}B bursts, {args.gap}ms gap "
          f"-> ~{rate/1024:.1f} KB/s, ETA {t_size/rate/60:.1f} min")

    ser = serial.Serial(args.port, 115200, timeout=10)
    ser.rts = True
    ser.dtr = True

    if not wait_beacon(ser):
        return 1

    ser.write(struct.pack("<I", UEXE))
    r = rd_u32(ser, 5)
    if r != OK:
        print(f"  ERROR: UEXE rejected ({name(r)})")
        return 1

    for v in (pc0, t_addr, t_size, csum):
        ser.write(struct.pack("<I", v))
    r = rd_u32(ser, 5)
    if r != OK:
        print(f"  ERROR: header rejected ({name(r)})")
        return 1

    gap = args.gap / 1000.0
    sent = 0
    t0 = time.time()
    while sent < t_size:
        n = min(CHUNK, t_size - sent)
        chunk = payload[sent:sent + n]
        for i in range(0, n, args.burst):
            ser.write(chunk[i:i + args.burst])
            ser.flush()
            if gap:
                time.sleep(gap)
        r = rd_u32(ser, 15)
        if r != MORE:
            print(f"\n  ERROR: chunk at offset {sent} failed ({name(r)})")
            print("  try a smaller --burst or larger --gap")
            return 1
        sent += n
        el = time.time() - t0
        pct = 100 * sent // t_size
        print(f"\r  {sent}/{t_size} ({pct}%)  {sent/el/1024:.1f} KB/s  "
              f"ETA {(t_size-sent)/(sent/el)/60:.1f} min   ", end="", flush=True)

    r = rd_u32(ser, 20)
    print()
    if r == DONE:
        print("  upload OK (checksum matched)")
    else:
        print(f"  ERROR: final status {name(r)}")
        return 1

    if not args.no_boot:
        print("  booting...")
        ser.write(struct.pack("<I", BOOT))
        time.sleep(0.5)
        ser.timeout = 0.3
        end = time.time() + 90
        while time.time() < end:
            d = ser.read(4096)
            if d:
                sys.stdout.write(d.decode("latin-1"))
                sys.stdout.flush()
    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
