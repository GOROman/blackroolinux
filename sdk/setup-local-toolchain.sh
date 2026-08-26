#!/bin/bash
#
# setup-local-toolchain.sh — make the bundled 32-bit EGCS toolchain runnable
# on a 64-bit host that has no 32-bit loader and no root access.
#
# The toolchain in sdk/toolchain/ is 32-bit x86 and asks for
# /lib/ld-linux.so.2. The supported fix is `sudo apt install libc6:i386`.
# Where that isn't possible, this script instead:
#
#   1. downloads the libc6-i386 package (plain user, no dpkg install),
#   2. unpacks its /usr/lib32 into  sdk/i386-runtime/,
#   3. copies sdk/toolchain -> sdk/toolchain-local, resolving the dangling
#      absolute symlinks (as/ld/ar/nm/... pointed into an old Archive/ path),
#   4. patchelf's every 32-bit binary to use sdk/i386-runtime/ld-linux.so.2.
#
# build.sh picks up sdk/toolchain-local automatically when the system 32-bit
# loader is missing. Re-run this script after moving the project directory —
# the interpreter path baked into the binaries is absolute.
#
# Attribution: New Blackroo work (2026, GPL v2)
#

set -e

SDK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME="$SDK_DIR/i386-runtime"
SRC="$SDK_DIR/toolchain"
DST="$SDK_DIR/toolchain-local"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "[1/4] Fetching 32-bit runtime (libc6-i386)..."
if [ -f "$RUNTIME/ld-linux.so.2" ]; then
    echo "  already present: $RUNTIME"
else
    ( cd "$WORK" && apt-get download libc6-i386 >/dev/null )
    dpkg-deb -x "$WORK"/libc6-i386_*.deb "$WORK/i386root"
    mkdir -p "$RUNTIME"
    cp -a "$WORK/i386root/usr/lib32/." "$RUNTIME/"
    echo "  -> $RUNTIME"
fi

echo "[2/4] Fetching patchelf..."
PATCHELF="$(command -v patchelf || true)"
if [ -z "$PATCHELF" ]; then
    ( cd "$WORK" && apt-get download patchelf >/dev/null )
    dpkg-deb -x "$WORK"/patchelf_*.deb "$WORK/pe"
    PATCHELF="$WORK/pe/usr/bin/patchelf"
fi
echo "  using: $PATCHELF"

echo "[3/4] Copying toolchain -> $(basename "$DST")..."
rm -rf "$DST"
cp -a "$SRC" "$DST"

echo "[4/4] Retargeting binaries..."
# Dangling absolute symlinks (as, ld, ar, nm, objcopy, objdump, ranlib, strip)
for l in $(find "$DST" -type l); do
    tgt="$(readlink -f "$l" || true)"
    if [ -f "$tgt" ]; then
        cp --remove-destination "$tgt" "$l"
    else
        echo "  WARNING: dangling symlink, no source: $l"
    fi
done

n=0
while read -r f; do
    "$PATCHELF" --set-interpreter "$RUNTIME/ld-linux.so.2" --set-rpath "$RUNTIME" "$f"
    n=$((n + 1))
done < <(find "$DST" -type f -exec file {} + | grep "ELF 32-bit LSB executable, Intel" | cut -d: -f1)
echo "  patched $n binaries"

echo ""
GCC_EXEC_PREFIX="$DST/lib/gcc-lib/" "$DST/bin/mipsel-linux-gcc" --version | head -1
echo "Local toolchain ready: $DST"
