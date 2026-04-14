#!/usr/bin/env python3
"""
iso-find.py — run psxcd's ISO9660 lookup on the host, against a real disc image.

drivers/block/psxcd.c finds the root filesystem by reading the volume
descriptor at LBA 16, walking the root directory and taking the extent. That
code only runs on a PlayStation, and a wrong answer there costs a CD-R and a
boot cycle to discover. This is the same algorithm - same offsets, same
both-endian handling, same ";1" stripping - runnable on the .bin before it is
burned.

    tools/host/iso-find.py output/blackroo-kloader.bin [ROOT.IMG]

Exits non-zero if the file is missing or does not look like the ext2 image the
kernel is about to try to mount.

Attribution: New Blackroo work (2026, GPL v2)
"""
import sys

SECT = 2048
RAW  = 2352          # mkpsxiso writes raw Mode2 sectors; the drive hands the
                     # kernel the 2048-byte user area, at +24 in each.
ISO_PVD_LBA, ISO_ROOT_RECORD = 16, 156
REC_LEN, REC_EXTENT, REC_SIZE, REC_FLAGS, REC_NAMELEN, REC_NAME = 0, 2, 10, 25, 32, 33
FLAG_DIR = 0x02


def le32(b, o):
    """Little-endian half of a both-endian field. Assembled byte-wise because
    these sit at odd offsets, and on MIPS an unaligned word load traps."""
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 2
    path = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 else "ROOT.IMG"

    img = open(path, "rb").read()
    raw = len(img) % RAW == 0
    step, skip = (RAW, 24) if raw else (SECT, 0)

    def sector(lba):
        off = lba * step + skip
        return img[off:off + SECT]

    print("%s: %d bytes, %s sectors" %
          (path, len(img), "raw 2352-byte" if raw else "2048-byte"))

    b = sector(ISO_PVD_LBA)
    if b[0] != 1 or b[1:6] != b"CD001":
        print("  no ISO9660 primary volume descriptor at LBA %d" % ISO_PVD_LBA)
        return 1

    root_lba = le32(b, ISO_ROOT_RECORD + REC_EXTENT)
    root_len = le32(b, ISO_ROOT_RECORD + REC_SIZE)
    print("  root directory: LBA %d, %d bytes" % (root_lba, root_len))

    found = None
    print("  files:")
    for s in range((root_len + SECT - 1) // SECT):
        d, off = sector(root_lba + s), 0
        while off < SECT:
            reclen = d[off + REC_LEN]
            if reclen == 0 or off + reclen > SECT:
                break
            nl = d[off + REC_NAMELEN]
            nm = d[off + REC_NAME:off + REC_NAME + nl]
            if nl and not (d[off + REC_FLAGS] & FLAG_DIR):
                lba, size = le32(d, off + REC_EXTENT), le32(d, off + REC_SIZE)
                print("    %-16s LBA %6d  %9d bytes" %
                      (nm.decode("ascii", "replace"), lba, size))
                if nm.split(b";")[0].upper() == want.upper().encode():
                    found = (lba, size)
            off += reclen

    if not found:
        print("  %s NOT FOUND — root=/dev/psxcd would not mount" % want)
        return 1

    lba, size = found
    print("\n  psxcd would report: image at LBA %d, %d KB" % (lba, size >> 10))

    # The lookup finding *something* is not the same as it finding a
    # filesystem. Check the superblock the kernel is about to read.
    sb = (sector(lba) + sector(lba + 1))[1024:1024 + 128]
    magic = sb[56] | (sb[57] << 8)
    bs = 1024 << le32(sb, 24)
    ok = magic == 0xEF53 and bs >= SECT
    print("  ext2 magic 0x%04x %s, block size %d %s" %
          (magic, "OK" if magic == 0xEF53 else "BAD",
           bs, "OK" if bs >= SECT else "TOO SMALL for a 2048-byte device"))
    if not ok:
        print("  -> this image will NOT mount (docs/24 §5.3: mke2fs -b 2048)")
        return 1
    print("  -> looks mountable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
