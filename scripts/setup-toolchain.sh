#!/bin/bash
#
# setup-toolchain.sh — Install the EGCS 2.91.66 mipsel cross-compiler
#
# The toolchain is a 32-bit x86 binary from 1999. On modern 64-bit Linux
# it needs 32-bit compatibility libraries.
#
# Source: Original Runix/Blackroo toolchain archive
# Attribution: EGCS project (GNU), packaged for Runix

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "============================================"
echo " Blackroo Linux — Toolchain Setup"
echo "============================================"
echo ""

# Check if already installed
if command -v mipsel-linux-gcc &>/dev/null; then
    echo "[OK] Toolchain already installed:"
    echo "     $(which mipsel-linux-gcc)"
    mipsel-linux-gcc --version 2>&1 | head -1
    echo ""
    echo "To reinstall, remove /usr/local/bin/mipsel-linux-* first."
    exit 0
fi

# Install 32-bit support
echo "[1/3] Installing 32-bit compatibility libraries..."
echo "      (needs sudo)"
sudo dpkg --add-architecture i386
sudo apt-get update -qq
sudo apt-get install -y -qq libc6:i386 libstdc++5:i386 zlib1g:i386 2>/dev/null || \
    sudo apt-get install -y -qq libc6:i386 zlib1g:i386

# Install build dependencies
echo "[2/3] Installing build dependencies..."
sudo apt-get install -y -qq build-essential flex bison bc ncurses-dev genext2fs

# Extract toolchain
echo "[3/3] Extracting toolchain to /usr/local/..."
TARBALL="$PROJECT_ROOT/blackroo_mips_i586v1.tar.gz"

if [ ! -f "$TARBALL" ]; then
    # Try Archive location
    TARBALL=$(find "$PROJECT_ROOT/Archive" -name "*.tar.gz" -path "*mips*" 2>/dev/null | head -1)
fi

if [ -z "$TARBALL" ] || [ ! -f "$TARBALL" ]; then
    echo ""
    echo "[ERROR] Toolchain archive not found."
    echo "        Expected: blackroo_mips_i586v1.tar.gz"
    echo "        Place it in the project root directory."
    exit 1
fi

echo "  Using: $TARBALL"
sudo tar xzf "$TARBALL" -C /

# Verify
echo ""
if command -v mipsel-linux-gcc &>/dev/null; then
    echo "[OK] Toolchain installed successfully!"
    echo "     $(mipsel-linux-gcc --version 2>&1 | head -1)"
else
    echo "[ERROR] Toolchain installed but mipsel-linux-gcc not found in PATH."
    echo "        Try: export PATH=/usr/local/bin:\$PATH"
    exit 1
fi

echo ""
echo "You can now build the kernel:"
echo "  make kernel"
echo ""
