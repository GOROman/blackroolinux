#!/bin/bash
#
# Create initrd for Blackroo Linux
# Run with sudo: sudo ./scripts/make_initrd.sh
#

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INITRD_DIR="$PROJECT_ROOT/build/initrd_root"
INITRD_IMG="$PROJECT_ROOT/output/initrd.img"
MOUNT_POINT="$PROJECT_ROOT/build/initrd_mount"
SIZE_KB=2048

echo "=== Blackroo Linux InitRD Creator ==="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run with sudo: sudo $0"
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

echo "Creating ${SIZE_KB}KB ext2 initrd..."

# Create empty image
dd if=/dev/zero of="$INITRD_IMG" bs=1k count=$SIZE_KB 2>/dev/null

# Create ext2 filesystem
/sbin/mke2fs -F -m 0 -b 1024 -N 256 "$INITRD_IMG" >/dev/null 2>&1

# Mount it
mkdir -p "$MOUNT_POINT"
mount -o loop "$INITRD_IMG" "$MOUNT_POINT"

echo "Copying files to initrd..."

# Copy everything from initrd_root
cp -a "$INITRD_DIR"/* "$MOUNT_POINT/"

# Create device nodes
echo "Creating device nodes..."
mknod "$MOUNT_POINT/dev/console" c 5 1 2>/dev/null || true
mknod "$MOUNT_POINT/dev/null" c 1 3 2>/dev/null || true
mknod "$MOUNT_POINT/dev/zero" c 1 5 2>/dev/null || true
mknod "$MOUNT_POINT/dev/tty" c 5 0 2>/dev/null || true
mknod "$MOUNT_POINT/dev/tty0" c 4 0 2>/dev/null || true
mknod "$MOUNT_POINT/dev/tty1" c 4 1 2>/dev/null || true
mknod "$MOUNT_POINT/dev/ram0" b 1 0 2>/dev/null || true
mknod "$MOUNT_POINT/dev/mem" c 1 1 2>/dev/null || true

# PlayStation memory card devices
mknod "$MOUNT_POINT/dev/bu0" b 240 0 2>/dev/null || true
mknod "$MOUNT_POINT/dev/bu1" b 240 1 2>/dev/null || true

# Set permissions
chmod 755 "$MOUNT_POINT/bin/busybox"
chmod 755 "$MOUNT_POINT/init"
chmod 755 "$MOUNT_POINT/etc/init.d/rcS"

# Show contents
echo ""
echo "InitRD contents:"
ls -la "$MOUNT_POINT/"
echo ""
df -h "$MOUNT_POINT"

# Unmount
sync
umount "$MOUNT_POINT"
rmdir "$MOUNT_POINT"

echo ""
echo "InitRD created: $INITRD_IMG"
ls -lh "$INITRD_IMG"
echo ""
echo "Now run: ./build.sh convert"
echo "to create the final PS-EXE with initrd embedded."
