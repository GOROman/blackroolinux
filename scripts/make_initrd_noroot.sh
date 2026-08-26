#!/bin/bash
#
# Create initrd for Blackroo Linux (no root required)
# Uses genext2fs instead of loop mounting
#

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INITRD_DIR="$PROJECT_ROOT/build/initrd_root"
INITRD_IMG="$PROJECT_ROOT/output/initrd.img"
DEVICE_TABLE="$PROJECT_ROOT/scripts/device_table.txt"
SIZE_KB=2048

echo "=== Blackroo Linux InitRD Creator (no root) ==="
echo ""

# Check for genext2fs
if ! command -v genext2fs &> /dev/null; then
    echo "genext2fs not found. Please install it:"
    echo "  sudo apt install genext2fs"
    echo ""
    echo "Or use the root-based script:"
    echo "  sudo ./scripts/make_initrd.sh"
    exit 1
fi

# Check if initrd_root exists
if [ ! -d "$INITRD_DIR" ]; then
    echo "Error: $INITRD_DIR not found"
    echo "Run ./build.sh first to create initrd structure"
    exit 1
fi

# Check if busybox exists and is valid
if [ ! -f "$INITRD_DIR/bin/busybox" ]; then
    echo "Error: busybox not found in $INITRD_DIR/bin/"
    exit 1
fi

BUSYBOX_SIZE=$(stat -c%s "$INITRD_DIR/bin/busybox")
if [ "$BUSYBOX_SIZE" -lt 100000 ]; then
    echo "Error: busybox seems too small ($BUSYBOX_SIZE bytes)"
    echo "Please download MIPS busybox:"
    echo "  wget https://busybox.net/downloads/binaries/1.28.1-defconfig-multiarch/busybox-mipsel -O tools/busybox-mipsel"
    exit 1
fi

# Check device table exists
if [ ! -f "$DEVICE_TABLE" ]; then
    echo "Error: Device table not found: $DEVICE_TABLE"
    exit 1
fi

echo "Creating ${SIZE_KB}KB ext2 initrd..."
echo "Source: $INITRD_DIR"
echo "Output: $INITRD_IMG"
echo ""

# Create output directory
mkdir -p "$(dirname "$INITRD_IMG")"

# Create ext2 filesystem with genext2fs
# -b: size in blocks (1KB each)
# -d: source directory
# -D: device table file
# -N: number of inodes
genext2fs \
    -b $SIZE_KB \
    -d "$INITRD_DIR" \
    -D "$DEVICE_TABLE" \
    -N 256 \
    -m 0 \
    "$INITRD_IMG"

echo ""
echo "InitRD created successfully!"
echo ""
ls -lh "$INITRD_IMG"
file "$INITRD_IMG"
echo ""

# Show contents using debugfs
echo "Contents (via debugfs):"
debugfs -R "ls -l" "$INITRD_IMG" 2>/dev/null || true
echo ""

echo "Now run: ./build.sh convert"
echo "to create the final PS-EXE with initrd embedded."
