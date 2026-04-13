#!/bin/bash
#
# build-native.sh — build kloader with a local PSn00bSDK, no Docker, no root.
#
# The original build.sh drives PSn00bSDK through a `blackroo-psn00bsdk` Docker
# image, and Docker is no longer installed on this machine. This uses the
# upstream prebuilt SDK instead:
#
#   ~/projects/toolchains/psn00bsdk/sdk/PSn00bSDK-0.24-Linux   (SDK + toolchain)
#   ~/projects/toolchains/psn00bsdk/toolchain                  (gcc 12.3.0, standalone)
#
# from https://github.com/Lameguy64/PSn00bSDK/releases/tag/v0.24
#
# cmake and ninja are not installed system-wide either; unpack them locally:
#   apt-get download cmake cmake-data ninja-build librhash1 libuv1t64 libjsoncpp26 libcurl4t64
#   dpkg-deb -x <each>.deb <dir>
# and point BLACKROO_BUILDTOOLS at that <dir>.
#
# Attribution: New Blackroo work (2026, GPL v2)
#

set -e

BOOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT="$(cd "$BOOT_DIR/.." && pwd)/output"
SDK="${PSN00BSDK_ROOT:-$HOME/projects/toolchains/psn00bsdk/sdk/PSn00bSDK-0.24-Linux}"
TOOLS="${BLACKROO_BUILDTOOLS:-}"

[ -d "$SDK" ] || { echo "PSn00bSDK not found at $SDK"; exit 1; }

CMAKE=cmake
NINJA_PATH=""
if [ -n "$TOOLS" ] && [ -x "$TOOLS/usr/bin/cmake" ]; then
    CMAKE="$TOOLS/usr/bin/cmake"
    NINJA_PATH="$TOOLS/usr/bin"
    export LD_LIBRARY_PATH="$TOOLS/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"
elif ! command -v cmake >/dev/null; then
    echo "cmake not found — set BLACKROO_BUILDTOOLS (see header)"; exit 1
fi

export PATH="$SDK/bin:$NINJA_PATH:$PATH"
export PSN00BSDK_LIBDIR="$SDK/lib/libpsn00b"

echo "  SDK:    $SDK"
echo "  gcc:    $(mipsel-none-elf-gcc --version | head -1)"
echo ""

rm -rf "$BOOT_DIR/build-native"
"$CMAKE" -S "$BOOT_DIR" -B "$BOOT_DIR/build-native" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$SDK/lib/libpsn00b/cmake/sdk.cmake" \
    -DCMAKE_BUILD_TYPE=Debug >/dev/null
"$CMAKE" --build "$BOOT_DIR/build-native"

mkdir -p "$OUTPUT"
cp "$BOOT_DIR/build-native/bootloader.exe" "$OUTPUT/bootloader.exe"

python3 - "$OUTPUT/bootloader.exe" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read(0x40)
pc, gp, ta, ts = struct.unpack_from('<IIII', d, 0x10)
sp = struct.unpack_from('<I', d, 0x30)[0]
print(f"\n  -> output/bootloader.exe")
print(f"     magic {d[:8].decode()}  entry 0x{pc:08X}")
print(f"     load  0x{ta:08X}..0x{ta+ts:08X}  ({ts} bytes)  sp 0x{sp:08X}")
PY
