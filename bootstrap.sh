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
