#!/bin/bash
#
# build-iso.sh — Blackroo Linux bootable CD-ROM builder
#
# Produces two PS1 disc images in output/ :
#
#   blackroo.bin/.cue          BIOS boots KERNEL.EXE (the Linux kernel) and the
#                              kernel drops into the serial monitor. THIS is the
#                              one to burn: stock 2 MB console + modchip, no
#                              host PC needed to get running.
#
#   blackroo-kloader.bin/.cue  BIOS boots BLACKROO.EXE (the kloader menu)
#                              instead, with the kernels alongside on the disc.
#                              Menu, serial shell, memcard and PIO tools.
#
# Requires: mkpsxiso 2.x on PATH (or ~/bin/mkpsxiso)
#
# Attribution: New Blackroo work (2026, GPL v2)
#

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO_DIR="$PROJECT_ROOT/iso"
OUTPUT="$PROJECT_ROOT/output"

MKPSXISO="$(command -v mkpsxiso || echo "$HOME/bin/mkpsxiso")"
if [ ! -x "$MKPSXISO" ]; then
    echo "ERROR: mkpsxiso not found (tried PATH and ~/bin/mkpsxiso)"
    exit 1
fi

echo "============================================"
echo " Blackroo Linux — Boot CD builder"
echo "============================================"
echo ""

# ── Step 1: check payloads ────────────────────────────────────
echo "[1/3] Checking payloads..."
MISSING=0
for f in bootloader.exe blackroo.exe blackroo_noinitrd.exe; do
    if [ -f "$OUTPUT/$f" ]; then
        printf '  %-24s %s\n' "$f" "$(ls -lh "$OUTPUT/$f" | awk '{print $5}')"
    else
        echo "  $f  MISSING"
        MISSING=1
    fi
done
[ "$MISSING" = "1" ] && { echo ""; echo "  Build the kernel (./build.sh) and kloader (./bootloader/build.sh) first."; exit 1; }
echo ""

# ── Step 2: PS-EXE header sanity ──────────────────────────────
echo "[2/3] PS-EXE headers..."
python3 - "$OUTPUT" <<'PY'
import struct, sys, os
out = sys.argv[1]
for name in ("bootloader.exe", "blackroo.exe", "blackroo_noinitrd.exe"):
    p = os.path.join(out, name)
    d = open(p, 'rb').read(0x40)
    if d[:8] != b'PS-X EXE':
        print(f"  {name:24} INVALID — no PS-X EXE magic"); continue
    pc, gp, ta, ts = struct.unpack_from('<IIII', d, 0x10)
    sp = struct.unpack_from('<I', d, 0x30)[0]
    end = ta + ts
    print(f"  {name:24} entry=0x{pc:08X} load=0x{ta:08X}..0x{end:08X} "
          f"({ts/1024:.0f} KB) sp=0x{sp:08X}")
PY
echo ""
# This used to warn that kloader and the kernel both load at 0x80010000 and
# that kloader's "Boot from CD-ROM" therefore overwrites itself mid-copy. That
# was fixed by linking the kernel at 0x90000 (GR-008), and the addresses
# printed just above are the proof - but the warning stayed, contradicting
# them, until 2026-08-25. Read what the script prints, not what it claims.
echo "  Kernel loads at 0x80090000, clear of kloader (0x80010000..0x8001E800)"
echo "        and its heap. See LOADADDR in arch/mipsnommu/Makefile - NOT"
echo "        ld.script, which is generated and deleted by 'make clean'."
echo ""

# ── Step 3: build images ──────────────────────────────────────
echo "[3/3] Building disc images..."
cd "$ISO_DIR"

# Licence area. A console rejects a disc with a missing/zeroed licence area
# ("Please insert PlayStation CD-ROM") even with a modchip fitted — the chip
# answers the wobble check, this is a different one. See iso/LICENSE-README.md.
#   BLACKROO_LICENSE=... to override; LICENSEE.DAT = PAL, LICENSEA.DAT = NTSC-U.
# The disc now carries a root filesystem as well as the kernels, because
# root=/dev/psxcd mounts ROOT.IMG rather than a ramdisk (docs/28). Build it if
# it is missing or older than the shell it contains - the XML references it by
# path and mkpsxiso simply fails if it is not there.
if [ ! -f "$PROJECT_ROOT/output/ROOT.IMG" ] || \
   [ "$PROJECT_ROOT/userland/brsh.c" -nt "$PROJECT_ROOT/output/ROOT.IMG" ]; then
    echo "  Building the CD root filesystem (scripts/make-cdroot.sh)..."
    "$PROJECT_ROOT/scripts/make-cdroot.sh" >/dev/null || {
        echo "ERROR: make-cdroot.sh failed" >&2; exit 1; }
fi
# stat, not du: truncate leaves the image sparse, so du reports the blocks
# actually allocated (132K) rather than the 4 MB the filesystem believes in
# and that mkpsxiso will pack onto the disc.
echo "  Root fs: $(( $(stat -c%s "$PROJECT_ROOT/output/ROOT.IMG") / 1024 )) KB ROOT.IMG"

# GPL v2 §3(a): the disc carries its own complete corresponding source, so the
# XML references output/SOURCE.TGZ and mkpsxiso simply fails without it. Build
# it if missing - on a fresh checkout nothing has made it yet.
if [ ! -f "$PROJECT_ROOT/output/SOURCE.TGZ" ] || \
   [ "$PROJECT_ROOT/README.md" -nt "$PROJECT_ROOT/output/SOURCE.TGZ" ]; then
    echo "  Building the source archive (scripts/make-source-dist.sh)..."
    "$PROJECT_ROOT/scripts/make-source-dist.sh" >/dev/null || {
        echo "ERROR: make-source-dist.sh failed" >&2; exit 1; }
fi
echo "  Source:  $(( $(stat -c%s "$PROJECT_ROOT/output/SOURCE.TGZ") / 1024 )) KB SOURCE.TGZ + COPYING.TXT"

LICENSE="${BLACKROO_LICENSE:-$ISO_DIR/LICENSEE.DAT}"
if [ ! -f "$LICENSE" ]; then
    LICENSE="$ISO_DIR/license.dat"
    echo "  WARNING: no real licence file — using the zero-filled placeholder."
    echo "           The disc will run in emulators and be REJECTED by a console."
else
    echo "  Licence: $(basename "$LICENSE")"
fi

"$MKPSXISO" -y -q -l BLACKROO blackroo_direct_cd.xml
echo "  -> output/blackroo.bin + .cue         (BIOS -> KERNEL.EXE -> serial monitor)"
"$MKPSXISO" -y -q -l BLACKROO blackroo_cd.xml
echo "  -> output/blackroo-kloader.bin + .cue (BIOS -> BLACKROO.EXE, kloader menu)"
echo ""

# Licence-area transplant.
#
# mkpsxiso 2.20 writes the licence file's Form-2 tail (disc sectors 12-15)
# differently from the older build that produced the homebrew discs this
# console actually boots: 2.20 leaves those sector bodies empty, the older
# one starts each with 00 00 08 00 00 00 08 00. Sectors 4-11 are identical
# either way. Since the first 16 sectors are pure licence area and carry
# nothing of ours, copy them verbatim from a reference image so our discs
# are byte-identical to a known-booting one. See iso/LICENSE-README.md.
if [ -f "$ISO_DIR/license-area.bin" ]; then
    for img in "$OUTPUT/blackroo.bin" "$OUTPUT/blackroo-kloader.bin"; do
        [ -f "$img" ] && python3 - "$img" "$ISO_DIR/license-area.bin" <<'PYX'
import sys
img, lic = sys.argv[1], sys.argv[2]
blob = open(lic, 'rb').read()
with open(img, 'r+b') as f:
    f.seek(0)
    f.write(blob)
PYX
    done
    echo "  Licence area: transplanted first 16 sectors from license-area.bin"
fi

ls -lh "$OUTPUT"/blackroo*.bin | awk '{printf "  %-40s %s\n", $9, $5}'
echo ""
echo "  Test:  <emulator> output/blackroo.cue"
echo "  Burn:  cdrecord -v -dao dev=/dev/sr0 cuefile=output/blackroo.cue"
echo ""
echo "  Burned CD-R needs a modchip (or tonyhax/FreePSXBoot) — license.dat"
echo "  here is a zero-filled placeholder, not a Sony region stamp."
echo ""
echo "  Then talk to it:  python3 tools/host/blackroo-serial.py /dev/ttyUSB0 console"
echo "============================================"
