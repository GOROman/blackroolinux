#!/bin/bash
#
# build.sh — Blackroo Bootloader Build System
#
# Always performs a clean build. Removes all previous build
# artifacts before compiling to ensure a known-good binary.
#
# Usage:
#   ./bootloader/build.sh              Clean build
#   ./bootloader/build.sh test         Build and launch in DuckStation
#   ./bootloader/build.sh version      Show version and exit
#
# Requires: docker (blackroo-psn00bsdk image)
#

set -e

VERSION="0.5.0"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOOT_DIR="$PROJECT_ROOT/bootloader"
OUTPUT="$PROJECT_ROOT/output"
BUILD_NUM_FILE="$BOOT_DIR/.build_number"
EXE_NAME="bootloader.exe"

# ── Version query ─────────────────────────────────────────────
if [ "$1" = "version" ]; then
    echo "Blackroo Bootloader v${VERSION}"
    exit 0
fi

# ── Increment build number ────────────────────────────────────
if [ -f "$BUILD_NUM_FILE" ]; then
    BUILD_NUM=$(cat "$BUILD_NUM_FILE")
else
    BUILD_NUM=0
fi
BUILD_NUM=$((BUILD_NUM + 1))
echo "$BUILD_NUM" > "$BUILD_NUM_FILE"
BUILD_DATE=$(date '+%Y-%m-%d %H:%M')

echo "============================================"
echo " Blackroo Bootloader v${VERSION}"
echo " Build #${BUILD_NUM} — ${BUILD_DATE}"
echo "============================================"
echo ""

# ── Step 1: Clean all artifacts ───────────────────────────────
echo "[1/5] Cleaning build artifacts..."
rm -rf "$BOOT_DIR/build"
rm -f  "$OUTPUT/$EXE_NAME"
echo "  Removed: bootloader/build/"
echo "  Removed: output/$EXE_NAME"
echo ""

# ── Step 2: Compile ──────────────────────────────────────────
echo "[2/5] Compiling (PSn00bSDK via Docker)..."
docker run --rm -v "$BOOT_DIR:/work" blackroo-psn00bsdk bash -c "
    cmake -S . -B build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/psn00bsdk/PSn00bSDK-0.24-Linux/lib/libpsn00b/cmake/sdk.cmake \
        -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -1
    cmake --build build 2>&1
" 2>&1

if [ ! -f "$BOOT_DIR/build/$EXE_NAME" ]; then
    echo ""
    echo "  BUILD FAILED — no output binary produced."
    exit 1
fi

echo ""

# ── Step 3: Install to output ────────────────────────────────
echo "[3/5] Installing..."
mkdir -p "$OUTPUT"
cp "$BOOT_DIR/build/$EXE_NAME" "$OUTPUT/$EXE_NAME"
FILE_SIZE=$(ls -lh "$OUTPUT/$EXE_NAME" | awk '{print $5}')
echo "  -> output/$EXE_NAME ($FILE_SIZE)"
echo ""

# ── Step 4: Validate PS-EXE ─────────────────────────────────
echo "[4/5] Validating PS-EXE..."
echo ""

python3 -c "
import struct, sys
with open('$OUTPUT/$EXE_NAME', 'rb') as f:
    data = f.read(0x40)
magic = data[0:8]
pc = struct.unpack_from('<I', data, 0x10)[0]
t_addr = struct.unpack_from('<I', data, 0x18)[0]
t_size = struct.unpack_from('<I', data, 0x1c)[0]
sp = struct.unpack_from('<I', data, 0x30)[0]

ok = True
print(f'  Magic:   {\"OK\" if magic == b\"PS-X EXE\" else \"INVALID\"}')
print(f'  Entry:   0x{pc:08X}')
print(f'  Load:    0x{t_addr:08X}')
print(f'  Size:    {t_size} bytes ({t_size/1024:.0f} KB)')
print(f'  Stack:   0x{sp:08X}', end='')
if sp == 0:
    print(' (WARNING: no stack)')
    ok = False
else:
    print(' (OK)')
print()
sys.exit(0 if ok else 1)
" || echo "  VALIDATION WARNINGS — binary may not run correctly"

# Show key functions
echo "  Exported functions:"
docker run --rm -v "$BOOT_DIR:/work" blackroo-psn00bsdk bash -c "
    mipsel-none-elf-nm -n build/bootloader.elf 2>/dev/null | grep ' T ' | awk '{print \"    \" \$3}'
" 2>&1 | head -20

echo ""

# Check for undefined symbols
UNDEF=$(docker run --rm -v "$BOOT_DIR:/work" blackroo-psn00bsdk bash -c "
    mipsel-none-elf-nm build/bootloader.elf 2>/dev/null | grep ' U '
" 2>&1)
if [ -z "$UNDEF" ]; then
    echo "  Undefined symbols: none (OK)"
else
    echo "  Undefined symbols:"
    echo "$UNDEF" | head -10
    echo "  WARNING: unresolved symbols!"
fi

echo ""

# ── Step 5: Done ─────────────────────────────────────────────
echo "[5/5] Build #${BUILD_NUM} complete"
echo ""
echo "  Binary:  output/$EXE_NAME ($FILE_SIZE)"
echo "  Version: v${VERSION}"
echo ""
echo "  To test:   ./psxboot.sh bootloader"
echo "  To upload: blackroo-serial /dev/ttyUSB0 upload output/$EXE_NAME"
echo ""
echo "============================================"

# ── Build log ─────────────────────────────────────────────────
echo "[$BUILD_DATE] v${VERSION} Build #${BUILD_NUM} — ${FILE_SIZE}" >> "$BOOT_DIR/BUILDLOG.txt"

# ── Optional: launch emulator ─────────────────────────────────
if [ "$1" = "test" ]; then
    echo ""
    echo "Launching DuckStation..."
    "$PROJECT_ROOT/psxboot.sh" bootloader
fi
