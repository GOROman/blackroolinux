#!/usr/bin/env python3
"""
brmon.py — talk to the in-kernel Blackroo monitor over SIO1.

The kernel's BRMON prompt (arch/mipsnommu/ps/brmon.c) is plain 115200 8N1 with
RTS asserted — no protocol, just a shell. This drives it non-interactively so
commands and replies can be captured:

    brmon.py /dev/ttyUSB1 -c ram -c cpu -c "md 80010000 8"
    brmon.py /dev/ttyUSB1              # dump whatever is on the wire

Attribution: New Blackroo work (2026, GPL v2)
"""
import argparse, sys, time, serial

ap = argparse.ArgumentParser()
ap.add_argument("port")
ap.add_argument("-c", "--command", action="append", default=[])
ap.add_argument("--wait", type=float, default=6.0)
ap.add_argument("--listen", type=float, default=0)
a = ap.parse_args()

s = serial.Serial(a.port, 115200, timeout=0.2)
s.rts = True
s.dtr = True

def rx(sec):
    out = b""
    end = time.time() + sec
    while time.time() < end:
        d = s.read(4096)
        if d:
            out += d
            end = time.time() + 0.8
    return out.decode("latin-1")

if a.listen:
    print(rx(a.listen).replace("\r\n", "\n"))
    sys.exit(0)

pending = rx(1.5)
if pending.strip():
    print("[on the wire]")
    print(pending.replace("\r\n", "\n"))

for cmd in a.command:
    for ch in cmd:
        s.write(ch.encode()); s.flush(); time.sleep(0.12)
    s.write(b"\r"); s.flush()
    out = rx(a.wait).replace("\r\n", "\n")
    print("=" * 56)
    print(f"blackroo> {cmd}")
    body = out.split("\n", 1)[1] if "\n" in out else out
    print(body.strip())
s.close()
