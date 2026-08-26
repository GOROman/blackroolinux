#!/bin/bash
# ============================================================================
#  make-cdroot.sh — the ext2 root filesystem that lives on the disc
# ============================================================================
#  docs/28: the CD is the system disk, the memory cards are the writable
#  volume. This builds the read-only half - the image mkpsxiso puts on the
#  disc as ROOT.IMG, which drivers/block/psxcd.c finds by walking the ISO9660
#  root directory and which the kernel mounts with root=/dev/psxcd.
#
#  TWO constraints, both non-negotiable:
#
#    -b 2048   psxcd sets hardsect_size to 2048, and this tree's
#              fs/ext2/super.c refuses any filesystem whose blocksize is below
#              the hardware sector size. A 1024-byte image is simply rejected.
#              (docs/24 §5.3)
#
#    rev 0     2.4's ext2 reads nothing newer, and mke2fs defaults to features
#              it has never heard of.
#
#  As with the initrd: no root, no genext2fs. mke2fs makes an empty image and
#  debugfs populates it, mknod included.
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE="$HERE/build/cdroot"
IMG="$HERE/output/ROOT.IMG"
BLOCKS="${CDROOT_BLOCKS:-2048}"      # x 2048 bytes = 4 MB
BS=2048

echo "==> building userland"
"$HERE/userland/build.sh" brsh.c

echo "==> staging $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/etc"
cp "$HERE/userland/brsh" "$STAGE/bin/sh"
chmod 755 "$STAGE/bin/sh"

cat > "$STAGE/etc/motd" <<'MOTD'
Blackroo Linux, running from the compact disc.

This root filesystem is ROOT.IMG on the CD-R, found by psxcd walking the
ISO9660 root directory - no LBA is baked into the kernel.  The disc is
read-only; the memory cards are the writable volume.

Type 'help'.
MOTD
printf 'Blackroo Linux 0.5.0 "Rootstock26"\nroot: /dev/psxcd - ROOT.IMG on this disc\nbuilt: %s\n' \
    "$(date -u '+%Y-%m-%d')" > "$STAGE/etc/release"

echo "==> mke2fs -b $BS (revision 0), $BLOCKS blocks = $((BLOCKS*BS/1024)) KB"
mkdir -p "$HERE/output"
rm -f "$IMG"
mke2fs -q -F -E revision=0 -b "$BS" -N 64 -I 128 "$IMG" "$BLOCKS"

echo "==> populating with debugfs"
cmds="$(mktemp)"
trap 'rm -f "$cmds"' EXIT
# Paths are relative and the cd's do the placing - debugfs resolves against
# its own cwd, so an absolute path after a cd lands somewhere else silently.
cat > "$cmds" <<CMDS
mkdir /bin
cd /bin
write $STAGE/bin/sh sh
cd /
mkdir /dev
cd /dev
mknod console c 5 1
mknod brcon c 60 0
mknod tty0 c 4 0
mknod tty1 c 4 1
mknod null c 1 3
mknod ram0 b 1 0
mknod bul b 208 0
mknod bu0 b 207 0
mknod bu1 b 207 1
mknod bu2 b 207 2
mknod bu3 b 207 3
mknod psxcd b 209 0
cd /
mkdir /etc
cd /etc
write $STAGE/etc/motd motd
write $STAGE/etc/release release
cd /
mkdir /tmp
mkdir /mnt
mkdir /mnt/mcdrive
quit
CMDS
debugfs -w -f "$cmds" "$IMG" >/dev/null 2>&1

# mke2fs/debugfs stop writing at the last used block, so the file comes out
# shorter than its superblock claims. Harmless on a CD (the driver reads by
# LBA and the ISO pads), but e2fsck calls it corrupt and it would bite the
# moment anything wrote. Same trap as the initrd - see GR-020.
truncate -s $((BLOCKS * BS)) "$IMG"

echo "==> fsck"
e2fsck -fn "$IMG" 2>&1 | tail -4

echo "==> contents"
for d in / /bin /dev /etc; do
    echo "  $d"
    debugfs -R "ls -l $d" "$IMG" 2>/dev/null | sed 's/^/    /'
done
ls -l "$IMG"
