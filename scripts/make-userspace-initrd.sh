#!/bin/bash
#
# make-userspace-initrd.sh — build the small ext2 initrd that carries brsh.
#
# This is NOT ./build.sh initrd. That target builds the old 2 MB BusyBox
# skeleton, which cannot run here: every busybox in the tree is a static ELF
# linked at 0x00400000 (4 MB) on a 2 MB machine. The image this makes holds one
# freestanding binary — userland/brsh — linked at the fixed address
# fs/binfmt_fixed.c reserves, plus the device nodes it needs.
#
# No root and no genext2fs (neither is available on this box): mke2fs builds an
# empty revision-0 image and debugfs populates it, mknod included.
#
# Written 2026-08-21 — the previous image had been assembled by hand, so there
# was no way to rebuild it after editing brsh.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOTFS="$HERE/build/rootfs"
IMG="$HERE/output/initrd.img"
BLOCKS=80          # 1 KB blocks, and every one of them is RAM: rd_load copies
                   # the whole image into the ramdisk, so image size IS cost on
                   # a 2 MB machine. brsh (16 KB) + lost+found (12 KB) + the
                   # directories and fs overhead come to ~46 KB, leaving room
                   # to mkdir and cp in /tmp. Raising this is not free.
                   #
                   # Went 64 -> 112 when the memory-card device nodes and
                   # /mnt/mcdrive landed: the image hit 32/32 inodes and
                   # 61/64 blocks, so a runtime mkdir would have failed with
                   # ENOSPC. Inodes went 32 -> 64 for the same reason - an
                   # ext2 image runs out of inodes long before it runs out
                   # of space when everything on it is small.
                   #
                   # Then 112 -> 80 after "Out of memory and no killable
                   # processes" at boot: on a 2 MB machine the initrd is paid
                   # for TWICE during startup - once in the PS-EXE the loader
                   # placed in RAM, and again in the ramdisk rd_load copies it
                   # into. 112 KB meant ~224 KB of peak footprint. Keep this
                   # as small as the contents allow.

echo "==> building userland"
"$HERE/userland/build.sh" brsh.c

echo "==> staging $ROOTFS"
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS/bin"
cp "$HERE/userland/brsh" "$ROOTFS/bin/sh"
chmod 755 "$ROOTFS/bin/sh"

# Something for ls, cat and hexdump to be pointed at on a machine whose root
# filesystem would otherwise contain exactly one file. /tmp is created empty so
# that mkdir/rm/cp have somewhere to work that is not /.
mkdir -p "$ROOTFS/etc"
cat > "$ROOTFS/etc/motd" <<'MOTD'
Blackroo Linux on the Sony PlayStation.

Linux 2.4, no MMU, MIPS R3000A at 33 MHz, 2 MB of RAM.
The shell you are typing at is the whole userland: one freestanding binary
with the file commands built in.  Type 'help'.
MOTD
printf 'Blackroo Linux 0.5.0 "Rootstock26"\nroot: /dev/ram0 - initrd\nbuilt: %s\n' \
    "$(date -u '+%Y-%m-%d')" > "$ROOTFS/etc/release"

echo "==> mke2fs (revision 0, no features — 2.4 ext2 reads nothing newer)"
mkdir -p "$HERE/output"
rm -f "$IMG"
mke2fs -q -F -r 0 -b 1024 -N 48 -I 128 "$IMG" "$BLOCKS"

echo "==> populating with debugfs"
cmds="$(mktemp)"
trap 'rm -f "$cmds"' EXIT
# debugfs resolves every path against its own cwd, so each name here is
# relative and the cd's do the placing. Absolute paths after a cd land in the
# wrong directory silently - /dev ended up as /bin/dev the first time.
cat > "$cmds" <<CMDS
mkdir /bin
cd /bin
write $ROOTFS/bin/sh sh
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
write $ROOTFS/etc/motd motd
write $ROOTFS/etc/release release
cd /
rm /lost+found
mkdir /tmp
mkdir /mnt
mkdir /mnt/mcdrive
quit
CMDS
debugfs -w -f "$cmds" "$IMG" >/dev/null 2>&1

# mke2fs and debugfs both stop writing at the last USED block, so the file
# comes out short - 42 KB for a filesystem whose superblock says 64. That was
# harmless while brsh had no way to write to the disc, but mkdir/cp/rm can now
# allocate a block past the end of the image, which after rd_load is past the
# end of the ramdisk device. Pad the file to the size the superblock claims.
#   e2fsck -fn output/initrd.img   should report no "physical size" complaint
truncate -s $((BLOCKS * 1024)) "$IMG"

echo "==> contents"
for d in / /bin /dev /etc; do
    echo "  $d"
    debugfs -R "ls -l $d" "$IMG" 2>/dev/null | sed 's/^/    /'
done
ls -l "$IMG"
