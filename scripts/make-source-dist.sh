#!/bin/bash
# ============================================================================
#  make-source-dist.sh — the corresponding source, for the disc
# ============================================================================
#  Blackroo ships a Linux kernel and a bootloader as binaries on a CD-R. Those
#  binaries are GPL v2, and GPL v2 section 3 says that distributing them means
#  also distributing "the complete corresponding machine-readable source code"
#  - which explicitly includes "all the source code for all modules it
#  contains, plus any associated interface definition files, plus the scripts
#  used to control compilation and installation of the executable".
#
#  There are three ways to satisfy that. (b), a written offer good for three
#  years, and (c), passing along an offer you received, are both promises. (a),
#  putting the source next to the binary, is a fact. The disc has 700 MB and
#  the binaries use about one, so this project does (a).
#
#  WHAT GOES IN
#    - blackroo/       the kernel: every .c .h .S, Makefiles, configs, COPYING
#    - bootloader/     kloader's source and its CMake build
#    - userland/       brsh.c and its link script
#    - scripts/ tools/ iso/ build.sh configs/
#                      "the scripts used to control compilation" - required,
#                      not optional, and the part people most often omit
#    - sdk/*.sh        how to obtain the cross toolchain
#
#  GENERATED BUILD STATE IS NOT SOURCE, AND SHIPPING IT IS ACTIVELY HARMFUL
#    2.4's Rules.make records the flags each object was built with in
#    .<obj>.o.flags and tracks headers in .depend, and build.sh generates
#    include/linux/autoconf.h and include/config/ from the defconfig. Ship
#    those and the recipient's build is steered by *this* machine's history:
#    a source archive that had them produced a kernel 31,232 bytes larger than
#    the same source in this tree, from byte-identical .c and .h files. The
#    same source must give the same binary, or "corresponding source" is not
#    corresponding to anything.
#
#    ONE EXCEPTION, and it is not optional: .depend files DO ship. They look
#    like generated state and in a stock kernel they are, but this tree had
#    the unused driver subdirectories (acpi/, atm/, ide/, scsi/ ...) deleted,
#    and 2.4's `make dep` recurses into every name its Makefiles mention.
#    Without the pre-generated .depend the build runs fastdep and dies on
#    "acpi: No such file or directory". build.sh says as much in cmd_kernel.
#    Here they are source.
#
#  WHAT STAYS OUT, AND WHY IT IS NOT A GPL PROBLEM
#    - sdk/toolchain*, sdk/i386-runtime
#         EGCS/GCC binaries. Not part of this work - a tool used to build it.
#         Shipping the binaries would create a *separate* obligation to supply
#         GCC's source, for no benefit: sdk/setup-local-toolchain.sh and
#         bootstrap.sh fetch it instead. GPL v2 section 3's final paragraph
#         makes the same distinction for compilers.
#    - tools/busybox-*
#         Third-party GPL binaries this project did not build and does not
#         ship on the disc. Including them here would mean owing BusyBox
#         source as well.
#    - *.o *.a, build/, output/, logs/, .git
#         Build products. Not source, and they are what the recipient is
#         about to regenerate.
#
#  The result is ONE file with an 8.3 name, because ISO9660 level 1 is what a
#  PlayStation reads: SOURCE.TGZ.
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$HERE/output"
STAGE="$HERE/build/srcdist"
NAME="blackroo-$(sed -n 's/.*BLACKROO_VERSION  *"\([^"]*\)".*/\1/p' \
        "$HERE/bootloader/src/version.h" | head -1)"
[ -n "$NAME" ] || NAME="blackroo-src"

echo "==> staging $NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE/$NAME"

# rsync is not guaranteed present; tar with excludes is, and it preserves modes
tar -C "$HERE" -cf - \
    --exclude='./sdk/toolchain' \
    --exclude='./sdk/toolchain-local' \
    --exclude='./sdk/i386-runtime' \
    --exclude='busybox' --exclude='busybox-*' \
    --exclude='mkmemcard' \
    --exclude='elf2psexe' \
    --exclude='addpsexe_initrd' \
    --exclude='__pycache__' \
    --exclude='build' --exclude='build-native' --exclude='build-native-high' \
    --exclude='./output' \
    --exclude='./logs' \
    --exclude='./carts' \
    --exclude='.git' \
    --exclude='*.o' --exclude='*.obj' --exclude='*.a' --exclude='*.so' \
    --exclude='*.elf' --exclude='*.exe' --exclude='*.bin' --exclude='*.img' \
    --exclude='*.iso' --exclude='*.mcd' --exclude='*.res' \
    --exclude='offset.s' --exclude='offset.h' \
    --exclude='.*.flags' --exclude='.version' --exclude='.ver' \
    --exclude='System.map' --exclude='compile.h' --exclude='autoconf.h' \
    --exclude='modules' --exclude='config' \
    --exclude='.config' --exclude='.config.old' \
    --exclude='./blackroo/linux' \
    --exclude='./userland/brsh' \
    . | tar -C "$STAGE/$NAME" -xf -

# Excludes are easy to get subtly wrong - an anchored './build' does not match
# 'bootloader/build' or 'bootloader/build-native', and a BusyBox binary sitting
# in initrd/skeleton/bin is a GPL obligation this project has no reason to take
# on. So do not trust the patterns. Find every ELF that actually landed, drop
# it, and SAY which ones - every binary in this tree is a build product that
# the recipient regenerates, but that is an assumption worth printing rather
# than burying. If something here ever needs to ship as a blob, this is where
# it will announce itself.
echo "==> stripping build products that the excludes missed"
found=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    echo "    dropped $(realpath --relative-to="$STAGE/$NAME" "$f")"
    rm -f "$f"
    found=$((found+1))
done < <(find "$STAGE/$NAME" -type f -exec file {} + 2>/dev/null \
         | grep -E ':.*(\bELF\b|ECOFF|current ar archive)' | cut -d: -f1)
[ "$found" -eq 0 ] && echo "    (none)"

# Nothing that names a tool, a person's home directory, or a scratch path
# leaves this archive either. These crept back in once already: a sync from the
# working tree overwrote files that had been cleaned, and nobody re-checked.
# build.sh rewrites HoangFlag in place on every build, so a tree that has been
# built carries this machine's toolchain path in it. Put the placeholder back
# in the archive; build.sh will fill it in again on the recipient's machine.
sed -i 's|^HoangFlag = .*|HoangFlag =\t# rewritten by build.sh to point at sdk/toolchain-*/include|' \
    "$STAGE/$NAME/blackroo/Makefile" 2>/dev/null || true

echo "==> checking for host paths and tool references"
leak=0
# Patterns are built at runtime rather than written out, so this script does
# not itself contain the strings it is looking for - otherwise every scan of
# the repository trips over the scanner.
#
# They are also specific on purpose. A bare '/home/' matches upstream Linux 2.4
# all over the place - ftape's CVS keywords say $Source: /homes/cvs/ - and
# '/home/root' is a legitimate path in our own documentation. What matters is
# THIS machine's home directory and scratch space leaking into a public tree.
# Only host paths here. Commit-message trailers are a property of the git
# history, not of the files in the archive, and belong in a hook rather than
# in a check that has to name them to look for them.
for pat in "$HOME" "/home/$(id -un)" "/tmp/$(id -un)"; do
    hits="$(grep -rIl "$pat" "$STAGE/$NAME" 2>/dev/null \
            | grep -vE '/(Documentation|scripts/make-source-dist\.sh)' || true)"
    if [ -n "$hits" ]; then
        echo "REFUSING TO SHIP - '$pat' appears in:" >&2
        printf '    %s\n' $hits >&2
        leak=1
    fi
done
[ "$leak" -eq 0 ] || exit 1
echo "    clean"

# Belt and braces: nothing ELF-shaped leaves this script.
# ELF is not the only shape a compiled artefact takes here: a 1.4 MB MIPSEL
# ECOFF kernel from 2023 sat in arch/mipsnommu/boot/ for years and sailed past
# an ELF-only check, host paths and all.
leaked="$(find "$STAGE/$NAME" -type f -exec file {} + 2>/dev/null \
          | grep -E ':.*(\bELF\b|ECOFF|\bcore file\b|current ar archive)' \
          | cut -d: -f1 || true)"
if [ -n "$leaked" ]; then
    echo "REFUSING TO SHIP - binaries still present:" >&2
    printf '    %s\n' $leaked >&2
    exit 1
fi

# Empty directories left behind by the strip add nothing to a source archive.
find "$STAGE/$NAME" -type d -empty -delete 2>/dev/null || true

# The kernel's own COPYING is the licence for the whole distribution; put a
# copy where nobody has to go looking for it.
cp "$HERE/blackroo/COPYING" "$STAGE/$NAME/COPYING"

cat > "$STAGE/$NAME/BUILDING.txt" <<'BUILD'
Building Blackroo Linux from this source
========================================

    ./bootstrap.sh

That is the whole thing: it checks what the host is missing, fetches the
cross toolchain if it is not already here, and builds the kernel, the
userland, the bootloader and the disc images.

Nothing needs root. Nothing is installed system-wide. If a dependency is
missing, bootstrap.sh prints the exact apt line and stops rather than
guessing.

What comes out, in output/:

    linux.elf              the kernel
    blackroo.exe           kernel + initrd, as a PS-EXE
    blackroo_noinitrd.exe  kernel alone, for root= on a real device
    ROOT.IMG               the ext2 root filesystem the disc carries
    blackroo-kloader.bin   the disc image to burn (with .cue and .toc)

Burn it with:

    cdrdao write --device /dev/sr0 --driver generic-mmc-raw -n --eject \
        output/blackroo-kloader.toc

A burned CD-R needs a modchip, or a soft exploit such as tonyhax or
FreePSXBoot. This is a property of the console, not of this disc.

WHY THE TOOLCHAIN IS NOT IN THIS ARCHIVE
----------------------------------------
The kernel is built with EGCS 2.91.66, which is a 1999 compiler and the last
one this 2.4 no-MMU tree builds cleanly with. Shipping its binaries here would
mean also shipping GCC's source to stay within the GPL, for a tool that is not
part of this work. bootstrap.sh obtains it instead - see sdk/README.md.
BUILD

echo "==> bootstrap.sh"
cat > "$STAGE/$NAME/bootstrap.sh" <<'BOOT'
#!/bin/bash
#
# bootstrap.sh — install what is missing, then build everything.
#
# One command, per BUILDING.txt. Deliberately does nothing as root: it reports
# what to install and stops, rather than running apt itself.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

say()  { printf '\033[0;34m[*]\033[0m %s\n' "$1"; }
ok()   { printf '\033[0;32m[+]\033[0m %s\n' "$1"; }
bad()  { printf '\033[0;31m[x]\033[0m %s\n' "$1" >&2; }

miss=()
need() { command -v "$1" >/dev/null 2>&1 || miss+=("$2"); }

say "checking the host"
need gcc        build-essential
need make       build-essential
need python3    python3
need mke2fs     e2fsprogs
need debugfs    e2fsprogs
need truncate   coreutils

if [ ${#miss[@]} -gt 0 ]; then
    bad "missing: ${miss[*]}"
    echo "    sudo apt install $(printf '%s ' "${miss[@]}" | tr ' ' '\n' | sort -u | tr '\n' ' ')"
    exit 1
fi
ok "host tools present"

# ── the cross toolchain ────────────────────────────────────────────────────
# Not shipped (see BUILDING.txt). Either it is already unpacked here, or the
# setup script fetches it.
if [ -x sdk/toolchain/bin/mipsel-linux-gcc ] || \
   [ -x sdk/toolchain-local/bin/mipsel-linux-gcc ]; then
    ok "cross toolchain present"
elif [ -x sdk/setup-toolchain.sh ]; then
    say "fetching the cross toolchain (sdk/setup-toolchain.sh)"
    ./sdk/setup-toolchain.sh
else
    bad "no cross toolchain and no sdk/setup-toolchain.sh"
    echo "    See sdk/README.md — Blackroo builds with EGCS 2.91.66 (mipsel-linux)."
    exit 1
fi

# The bundled toolchain is a 32-bit x86 binary. On a host without 32-bit
# support this relinks a copy against a local loader, no root required.
if [ ! -f /lib/ld-linux.so.2 ] && [ ! -f /lib32/ld-linux.so.2 ] && \
   [ ! -x sdk/toolchain-local/bin/mipsel-linux-gcc ] && \
   [ -x sdk/setup-local-toolchain.sh ]; then
    say "host has no 32-bit loader — building sdk/toolchain-local"
    ./sdk/setup-local-toolchain.sh
fi

say "kernel";        ./build.sh kernel
say "root fs";       ./scripts/make-cdroot.sh >/dev/null
say "initrd";        ./scripts/make-userspace-initrd.sh >/dev/null
say "PS-EXE";        ./build.sh convert

if [ -x bootloader/build-native.sh ]; then
    say "bootloader"
    ./bootloader/build-native.sh || \
        bad "bootloader build failed (needs PSn00bSDK — see bootloader/README.md); continuing"
fi

if [ -x iso/build-iso.sh ] && [ -f output/bootloader.exe ]; then
    say "disc images"; ./iso/build-iso.sh
fi

echo
ok "done — see output/"
ls -la output/ 2>/dev/null | sed 's/^/    /'
BOOT
chmod +x "$STAGE/$NAME/bootstrap.sh"

echo "==> tarball"
mkdir -p "$OUT"
tar -C "$STAGE" -czf "$OUT/SOURCE.TGZ" "$NAME"
echo "    $(ls -la "$OUT/SOURCE.TGZ" | awk '{print $5}') bytes"

# The licence has to be readable without extracting anything.
cp "$HERE/blackroo/COPYING" "$OUT/COPYING.TXT"

echo "==> contents"
tar -tzf "$OUT/SOURCE.TGZ" | wc -l | sed 's/^/    files: /'
tar -tzf "$OUT/SOURCE.TGZ" | grep -cE '\.(c|h|S)$' | sed 's/^/    C and asm sources: /'
