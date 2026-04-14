#!/usr/bin/env python3
"""
redux-sio1.py — terminal for BRMON over PCSX-Redux's SIO1 TCP server.

The emulator equivalent of:
    python3 tools/host/blackroo-serial.py /dev/ttyUSB0 console

PCSX-Redux emulates the PlayStation's SIO1 port and bridges it to a TCP
socket, so the in-kernel monitor can be driven without hardware.

Redux setup (Configuration -> Emulation, or ~/.config/pcsx-redux/pcsx.json):

    "SIO1Server":     true
    "SIO1ServerPort": 6699
    "SIO1Mode":       1      <-- 1 = Raw. 0 is Protobuf and will silently
                                 wrap every byte, so nothing you type ever
                                 reaches the monitor.

Then:  pcsx-redux -iso output/blackroo.cue -run
       python3 tools/host/redux-sio1.py

Ctrl+] to exit.

Attribution: New Blackroo work (2026, GPL v2)
"""

import argparse
import select
import socket
import sys
import termios
import tty


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6699)
    ap.add_argument("-c", "--command", action="append", default=[],
                    help="send a command and print the reply, then exit "
                         "(repeatable, non-interactive)")
    ap.add_argument("--wait", type=float, default=6.0,
                    help="seconds to wait for a reply in --command mode")
    args = ap.parse_args()

    try:
        s = socket.create_connection((args.host, args.port), timeout=10)
    except OSError as e:
        sys.exit(f"cannot reach Redux SIO1 server at {args.host}:{args.port} — {e}\n"
                 f"is 'Enable SIO1 Server' on, and the emulator running?")

    if args.command:
        run_commands(s, args.command, args.wait)
        return

    print(f"BRMON via Redux SIO1 {args.host}:{args.port} — Ctrl+] to exit")
    print("-" * 52)

    old = termios.tcgetattr(sys.stdin)
    try:
        tty.setraw(sys.stdin.fileno())
        s.setblocking(False)
        while True:
            r, _, _ = select.select([s, sys.stdin], [], [], 0.05)
            if s in r:
                data = s.recv(4096)
                if not data:
                    break
                sys.stdout.write(data.decode("latin-1"))
                sys.stdout.flush()
            if sys.stdin in r:
                ch = sys.stdin.read(1)
                if ch == "\x1d":            # Ctrl+]
                    break
                s.sendall(ch.encode("latin-1"))
    finally:
        termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old)
        s.close()
        print("\r\n[disconnected]")


def run_commands(s, commands, wait):
    import time
    s.settimeout(0.2)

    def drain(seconds):
        out = b""
        end = time.time() + seconds
        while time.time() < end:
            try:
                d = s.recv(4096)
                if d:
                    out += d
                    end = time.time() + 0.8
            except socket.timeout:
                pass
        return out.decode("latin-1")

    drain(1.0)
    for cmd in commands:
        # BRMON echoes per character; feed it at a human-ish rate
        for ch in cmd:
            s.sendall(ch.encode())
            time.sleep(0.15)
        s.sendall(b"\r")
        print(f"blackroo> {cmd}")
        print(drain(wait).replace("\r\n", "\n").replace(cmd, "", 1).strip())
        print("-" * 52)
    s.close()


if __name__ == "__main__":
    main()
