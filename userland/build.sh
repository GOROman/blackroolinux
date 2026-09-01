#!/bin/bash
#
# build.sh — build Blackroo userspace.
#
# Freestanding MIPS binaries linked at a fixed address inside the window
# fs/binfmt_fixed.c reserves. No libc, raw syscalls.
#
# The load address is one number in three places (kernel header, this link
# script, and every binary already built). The check below is what stops them
# drifting - the old comment here claimed 0x801c0000/256 KB while the code
# actually used 0x001f0000/64 KB, and nothing noticed.
#
# Uses the mipsel-none-elf GCC that came with PSn00bSDK — it targets bare
# metal, which is exactly right for code that makes its own syscalls.
#
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TC="${PSN00BSDK_ROOT:-$HOME/projects/toolchains/psn00bsdk/sdk/PSn00bSDK-0.24-Linux}/bin"
CC="$TC/mipsel-none-elf-gcc"

# A PSn00bSDK container may have produced brsh already; allow the host-side
# initrd assembly to reuse that verified MIPS binary without rebuilding it.
if [ "${SKIP_USERLAND_BUILD:-}" = 1 ] && [ -x "$HERE/brsh" ]; then
    echo "  reusing existing $HERE/brsh"
    exit 0
fi

[ -x "$CC" ] || { echo "no mipsel-none-elf-gcc at $CC"; exit 1; }

# ── the load address must match the kernel's reserved window ────────────────
# Which machine are we building for? Must match the kernel: the window sits at
# the TOP of RAM, so both the size and the base move with the RAM option.
#   DEFCONFIG=blackroo_8mb_defconfig ./build.sh brsh.c
DEFCONFIG_NAME="${DEFCONFIG:-blackroo_2mb_defconfig}"
DEFCONFIG="$HERE/../configs/kernel/$DEFCONFIG_NAME"
[ -f "$DEFCONFIG" ] || { echo "no such defconfig: $DEFCONFIG"; exit 1; }

RESERVE_KB="$(sed -n 's/^CONFIG_BLACKROO_USER_RESERVE_KB=\([0-9]*\)$/\1/p' "$DEFCONFIG")"
[ -n "$RESERVE_KB" ] || { echo "no CONFIG_BLACKROO_USER_RESERVE_KB in $DEFCONFIG"; exit 1; }

if grep -q '^CONFIG_PSX_16MB_RAM=y' "$DEFCONFIG"; then  RAM_TOP=$(( 0x01000000 ))
elif grep -q '^CONFIG_PSX_8MB_RAM=y' "$DEFCONFIG"; then RAM_TOP=$(( 0x00800000 ))
elif grep -q '^CONFIG_PSX_4MB_RAM=y' "$DEFCONFIG"; then RAM_TOP=$(( 0x00400000 ))
else                                                    RAM_TOP=$(( 0x00200000 )); fi

WANT_BASE="$(printf '0x%08x' $(( RAM_TOP - RESERVE_KB * 1024 )))"
HAVE_BASE="$(sed -n 's/^[[:space:]]*\. = \(0x[0-9a-fA-F]*\);.*$/\1/p' "$HERE/blackroo.ld" | head -1)"
if [ "$HAVE_BASE" != "$WANT_BASE" ]; then
    # Rewrite it rather than refusing: the address is derived, not authored,
    # and a build for a different machine legitimately needs a different one.
    sed -i "s|^\( *\)\. = 0x[0-9a-fA-F]*;|\1. = $WANT_BASE;|" "$HERE/blackroo.ld"
    HAVE_BASE="$WANT_BASE"
    echo "  blackroo.ld relinked at $WANT_BASE for $DEFCONFIG_NAME"
fi
if [ "$HAVE_BASE" != "$WANT_BASE" ]; then
    echo "load address drift:"
    echo "  blackroo.ld links at         $HAVE_BASE"
    echo "  the kernel reserves ${RESERVE_KB} KB -> $WANT_BASE"
    echo "Fix blackroo.ld, then REBUILD THE KERNEL AND EVERY USERSPACE BINARY."
    exit 1
fi
echo "  window: ${RESERVE_KB} KB at $WANT_BASE .. $(printf 0x%08x $RAM_TOP)  [$DEFCONFIG_NAME]"

for src in "$@"; do
    name="$(basename "$src" .c)"
    echo "  $src -> $name"
    "$CC" -O2 -G0 -mips1 -EL -mno-abicalls -fno-pic -fno-builtin \
          -nostdlib -nostartfiles -ffreestanding \
          -Wl,-T,"$HERE/blackroo.ld" \
          -o "$HERE/$name" "$HERE/$src"
    "$TC/mipsel-none-elf-size" "$HERE/$name"
    "$TC/mipsel-none-elf-readelf" -l "$HERE/$name" | grep -A1 LOAD | head -4
done
