#!/bin/bash
#
# upload.sh — Upload kernel to PlayStation 1 via serial
#
# Requires: nops (NOTPSXSerial) and UniROM running on the PS1
# Serial connection via PicoMemcard USB or FTDI/CH341 adapter
#
# Usage:
#   ./scripts/upload.sh                     # Auto-detect port, standard speed
#   ./scripts/upload.sh /dev/ttyUSB0        # Specify port
#   ./scripts/upload.sh /dev/ttyACM0 fast   # PicoMemcard USB, fast mode

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
KERNEL_EXE="$PROJECT_ROOT/output/kernel.exe"

PORT="${1:-}"
SPEED="${2:-standard}"

# Auto-detect serial port
if [ -z "$PORT" ]; then
    # Try common ports in order of likelihood
    for p in /dev/ttyACM0 /dev/ttyUSB0 /dev/ttyACM1 /dev/ttyUSB1; do
        if [ -e "$p" ]; then
            PORT="$p"
            break
        fi
    done
fi

if [ -z "$PORT" ]; then
    echo "ERROR: No serial port found."
    echo ""
    echo "Connect your serial adapter or PicoMemcard and try again."
    echo "Usage: $0 [port] [standard|fast]"
    echo ""
    echo "Common ports:"
    echo "  /dev/ttyACM0  — PicoMemcard (USB CDC)"
    echo "  /dev/ttyUSB0  — FTDI/CH341 serial adapter"
    exit 1
fi

if [ ! -f "$KERNEL_EXE" ]; then
    echo "ERROR: Kernel not found at $KERNEL_EXE"
    echo "Run 'make' first to build the kernel."
    exit 1
fi

# Find nops
NOPS=""
for n in nops "$PROJECT_ROOT/vendor/unirom/nops" "$HOME/bin/nops"; do
    if command -v "$n" &>/dev/null || [ -x "$n" ]; then
        NOPS="$n"
        break
    fi
done

if [ -z "$NOPS" ]; then
    echo "ERROR: nops (NOTPSXSerial) not found."
    echo ""
    echo "Install it from: https://github.com/JonathanDotCel/NOTPSXSerial"
    echo "Place the binary in your PATH or vendor/unirom/nops"
    exit 1
fi

SIZE=$(stat -c%s "$KERNEL_EXE")
SIZE_KB=$((SIZE / 1024))

echo "============================================"
echo " Blackroo Linux — Upload to PS1"
echo "============================================"
echo ""
echo "  Kernel:  $KERNEL_EXE ($SIZE_KB KB)"
echo "  Port:    $PORT"
echo "  Speed:   $SPEED"
echo ""

if [ "$SPEED" = "fast" ]; then
    echo "  Estimated time: ~$((SIZE_KB / 50)) seconds (518400 baud)"
    echo ""
    echo "Uploading (fast mode)..."
    "$NOPS" /fast /exe "$KERNEL_EXE" "$PORT"
else
    echo "  Estimated time: ~$((SIZE_KB / 11)) seconds (115200 baud)"
    echo ""
    echo "Uploading..."
    "$NOPS" /exe "$KERNEL_EXE" "$PORT"
fi

echo ""
echo "Upload complete. Kernel should be booting."
echo ""
echo "To monitor serial output:"
echo "  screen $PORT 115200"
echo "  (Ctrl-A K to exit screen)"
