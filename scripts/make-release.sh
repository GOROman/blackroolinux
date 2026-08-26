#!/bin/bash
# ============================================================================
#  make-release.sh — build a named Blackroo release for one memory size
# ============================================================================
#  Every artefact a release needs, from source to a burnable disc, for one of
#  the three supported machines:
#
#    2mb   "Little Joey"   a stock console, no modification
#    8mb   "Blackbelt"     the common 4-chip RAM mod
#    16mb  "Big Skippah"   COMING SOON - unverified, see docs/30
#
#    ./scripts/make-release.sh 2mb
#    ./scripts/make-release.sh all
#
#  Output lands in output/release-<size>/ so the three do not overwrite each
#  other, which they otherwise would - every one of them builds a file called
#  blackroo.exe.
#
#  WHAT MOVES WITH THE MEMORY SIZE
#    - the kernel config (CONFIG_PSX_*MB_RAM)
#    - the PS-EXE stack pointer, which the BIOS honours (GR-003)
#    - the userspace window, which sits at the TOP of RAM, so both its size
#      AND its address change - which means brsh must be relinked per variant
#      (see userland/build.sh). One binary does NOT serve all three.
#
#  16 MB IS UNVERIFIED. The memory controller's RAM_SIZE register documents
#  2, 4 and 8 MB only (0x0888 / 0x0988 / 0x0B88); there is no published
#  encoding for 16. That variant therefore uses the RUNTIME PROBE, so the
#  kernel measures what is actually fitted rather than being told a number
#  that might be a lie. A kernel that believes in memory the hardware does not
#  have does not fail loudly - it corrupts (GR-004).
#
#  Serial console and keyboard are enabled in ALL variants: a real machine
#  should always be reachable both ways.
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

variant="${1:-}"
case "$variant" in
    2mb)  CODENAME="Little Joey" ;;
    8mb)  CODENAME="Blackbelt" ;;
    16mb) CODENAME="Big Skippah"
          echo "NOTE: 16 MB is COMING SOON, not a release."
          echo "      The RAM_SIZE register has no published 16 MB encoding, so this"
          echo "      builds with the runtime probe and has never run on 16 MB hardware."
          echo "      See docs/30-BLOCKERS-AND-HARDWARE-NOTES.md."
          echo ;;
    all)  for v in 2mb 8mb 16mb; do "$0" "$v"; done; exit 0 ;;
    *)    echo "usage: $0 {2mb|8mb|16mb|all}"; exit 1 ;;
esac

DEFCONFIG="blackroo_${variant}_defconfig"
[ -f "configs/kernel/$DEFCONFIG" ] || { echo "no such config: $DEFCONFIG"; exit 1; }

OUT="$HERE/output/release-$variant"
VER="$(sed -n 's/.*BLACKROO_VERSION  *"\([^"]*\)".*/\1/p' bootloader/src/version.h | head -1)"

echo "============================================================"
echo " Blackroo $VER \"$CODENAME\"  —  $variant"
echo "============================================================"

# The codename is compiled into kloader and drawn on its menu, so a burned
# disc always says which variant it is. Restore version.h whatever happens.
cp bootloader/src/version.h "$HERE/.version.h.orig"
trap 'mv -f "$HERE/.version.h.orig" bootloader/src/version.h 2>/dev/null || true' EXIT
sed -i "s/^#define BLACKROO_CODENAME .*/#define BLACKROO_CODENAME       \"$CODENAME\"/" \
    bootloader/src/version.h

export DEFCONFIG
export PATH="$HERE/sdk/toolchain-local/bin:$HERE/sdk/toolchain/bin:$PATH"

echo "[1/7] userland (relinked for this machine's window)"
./userland/build.sh brsh.c | sed 's/^/      /'

echo "[2/7] kernel"
./build.sh kernel >/dev/null

echo "[3/7] root filesystem + initrd"
./scripts/make-cdroot.sh >/dev/null
./scripts/make-userspace-initrd.sh >/dev/null

echo "[4/7] PS-EXE"
./build.sh convert >/dev/null

echo "[5/7] bootloader"
./bootloader/build-native.sh >/dev/null 2>&1 || echo "      (bootloader build failed - needs PSn00bSDK)"

echo "[6/7] corresponding source (GPL v2 section 3a)"
./scripts/make-source-dist.sh >/dev/null

echo "[7/7] disc images + licence area"
./iso/build-iso.sh >/dev/null

mkdir -p "$OUT"
for f in linux.elf blackroo.exe blackroo_noinitrd.exe initrd.img ROOT.IMG \
         SOURCE.TGZ COPYING.TXT blackroo-kloader.bin blackroo-kloader.cue \
         blackroo-kloader.toc blackroo.bin blackroo.cue bootloader.exe; do
    [ -f "output/$f" ] && cp -p "output/$f" "$OUT/"
done

# A disc should be able to say what it is without being booted.
{
    echo "Blackroo Linux $VER \"$CODENAME\""
    echo "variant:   $variant"
    echo "config:    $DEFCONFIG"
    echo "built:     $(date -u '+%Y-%m-%d %H:%M UTC')"
    echo
    grep -E '^CONFIG_(PSX_[0-9]+MB_RAM|PSX_RAM_AUTO|BLACKROO_USER_RESERVE_KB)=' \
        "configs/kernel/$DEFCONFIG" | sed 's/^/  /'
    echo
    echo "  serial console: $(grep -q '^CONFIG_SERIAL_PSX_CONSOLE=y' "configs/kernel/$DEFCONFIG" && echo on || echo OFF)"
    echo "  keyboard:       $(grep -q '^CONFIG_PSX_KEYB=y' "configs/kernel/$DEFCONFIG" && echo on || echo OFF)"
    echo
    python3 - "$OUT/blackroo.exe" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
pc, gp, load, size = struct.unpack('<IIII', d[0x10:0x20])
sp, = struct.unpack('<I', d[0x30:0x34])
print("  PS-EXE  load 0x%08x  entry 0x%08x  sp 0x%08x" % (load, pc, sp))
PY
} > "$OUT/RELEASE.txt"

echo
sed 's/^/  /' "$OUT/RELEASE.txt"
echo "  -> $OUT"
