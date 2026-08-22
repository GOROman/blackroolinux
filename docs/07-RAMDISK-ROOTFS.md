# Blackroo Linux - Ramdisk and Root Filesystem Design

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: BINFMT_FLAT and a BusyBox userspace.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> InitRD configurations for 2MB, 4MB, and 8MB systems

---

## Overview

The root filesystem for Blackroo Linux uses a hybrid approach:
- **Primary:** InitRD (initial ramdisk) loaded into RAM at boot
- **Secondary:** Memory card RAID for persistent storage (optional)

The initrd contains BusyBox, essential device nodes, and an init script. On 8MB systems, the ramdisk can be large enough for comfortable interactive use. On 2MB systems, it must be stripped to the bare minimum.

---

## InitRD Architecture

### Boot Sequence

```
1. BIOS loads PS-EXE (bootloader or kernel+initrd bundle)
2. Kernel starts, finds initrd at embedded address
3. Kernel mounts initrd as temporary root filesystem
4. Kernel executes /linuxrc (or /init)
5. /linuxrc:
   a. Mount /proc
   b. Mount /dev (devfs or static nodes)
   c. Optionally mount memory cards
   d. Exec /bin/sh (interactive shell)
```

### InitRD Embedding

The initrd is embedded in the kernel binary using `addinitrd`:

```bash
# Create initrd
./scripts/make_initrd.sh

# Embed in ECOFF kernel
./tools/addinitrd output/linux.ecoff output/initrd.img output/linux.image.ecoff

# Convert to PS-EXE
./tools/elf2psx output/linux.image.ecoff output/linux.exe
```

### Kernel Configuration Required

```
CONFIG_BLK_DEV_INITRD=y    # Enable initrd support
CONFIG_BLK_DEV_RAM=y       # RAM disk support (for mounting initrd)
CONFIG_EXT2_FS=y           # Ext2 filesystem (initrd format)
CONFIG_BINFMT_FLAT=y       # Flat binary format (for BusyBox)
```

---

## Filesystem Configurations by RAM Size

### Micro InitRD (2MB System — 256KB budget)

**Constraints:** Kernel + initrd + user space must fit in 2MB total. After kernel (~600KB) and page tables (~200KB), only ~1,248KB remains. The initrd should be ~256KB compressed to leave ~848KB for runtime user space.

```
/ (root)
├── bin/
│   ├── busybox          # BusyBox (FLAT binary, ~150KB stripped)
│   ├── sh -> busybox    # Shell
│   ├── ls -> busybox    # List files
│   ├── cat -> busybox   # Print files
│   ├── echo -> busybox  # Echo
│   ├── mount -> busybox # Mount filesystems
│   ├── umount -> busybox
│   ├── mkdir -> busybox
│   ├── rm -> busybox
│   ├── cp -> busybox
│   ├── mv -> busybox
│   ├── ps -> busybox    # Process list
│   ├── kill -> busybox
│   ├── dmesg -> busybox # Kernel messages
│   └── df -> busybox    # Disk free
├── sbin -> bin           # Symlink
├── dev/
│   ├── console          # c 5 1
│   ├── null             # c 1 3
│   ├── zero             # c 1 5
│   ├── ram0             # b 1 0
│   ├── ttyS0            # c 4 64
│   ├── bu0              # b 60 0 (memory card slot 1)
│   └── bu1              # b 60 1 (memory card slot 2)
├── etc/
│   └── inittab          # "::sysinit:/bin/sh"
├── proc/                 # Mount point
├── tmp/                  # tmpfs mount point
└── linuxrc              # Init script
```

**linuxrc for 2MB:**
```sh
#!/bin/sh
echo "Blackroo Linux 0.2 (2MB)"
mount -t proc none /proc
echo "Type 'help' for commands"
exec /bin/sh
```

**Size estimate:**
```
busybox (FLAT, minimal config): ~100-150 KB
device nodes:                   <1 KB
linuxrc + etc:                  <1 KB
directory structure:            <1 KB
ext2 overhead:                  ~20 KB
Total uncompressed:             ~170 KB
Compressed (gzip -9):           ~80-100 KB
```

### Standard InitRD (4MB System — 500KB budget)

```
/ (root)
├── bin/
│   ├── busybox          # BusyBox (FLAT, more applets)
│   ├── sh -> busybox
│   ├── ash -> busybox
│   ├── ls -> busybox
│   ├── cat -> busybox
│   ├── echo -> busybox
│   ├── mount -> busybox
│   ├── umount -> busybox
│   ├── mkdir -> busybox
│   ├── rmdir -> busybox
│   ├── rm -> busybox
│   ├── cp -> busybox
│   ├── mv -> busybox
│   ├── ln -> busybox
│   ├── ps -> busybox
│   ├── kill -> busybox
│   ├── dmesg -> busybox
│   ├── df -> busybox
│   ├── du -> busybox
│   ├── free -> busybox
│   ├── head -> busybox
│   ├── tail -> busybox
│   ├── grep -> busybox
│   ├── find -> busybox
│   ├── sort -> busybox
│   ├── wc -> busybox
│   ├── vi -> busybox    # Text editor
│   ├── chmod -> busybox
│   ├── chown -> busybox
│   ├── date -> busybox
│   ├── dd -> busybox
│   ├── hexdump -> busybox
│   └── mkfs.ext2 -> busybox  # Format memory cards
├── sbin -> bin
├── dev/
│   ├── console          # c 5 1
│   ├── null             # c 1 3
│   ├── zero             # c 1 5
│   ├── ram0             # b 1 0
│   ├── ram1             # b 1 1
│   ├── ttyS0            # c 4 64
│   ├── tty0             # c 4 0
│   ├── bu0              # b 60 0
│   ├── bu1              # b 60 1
│   ├── bu2              # b 60 2 (multi-tap)
│   ├── bu3              # b 60 3
│   ├── bu4              # b 60 4
│   ├── bu5              # b 60 5
│   ├── bu6              # b 60 6
│   ├── bu7              # b 60 7
│   └── bul              # b 61 0 (joined large device)
├── etc/
│   ├── inittab
│   ├── passwd           # "root::0:0:root:/:/bin/sh"
│   ├── group            # "root:x:0:"
│   ├── fstab            # Mount points
│   ├── profile          # Shell profile
│   └── init.d/
│       └── rcS           # Startup script
├── proc/
├── sys/
├── tmp/
├── mnt/
│   └── card/            # Memory card mount point
├── home/
│   └── root/
└── linuxrc
```

**linuxrc for 4MB:**
```sh
#!/bin/sh
echo ""
echo "  Blackroo Linux 0.2 (4MB)"
echo "  ========================"
echo ""

# Mount virtual filesystems
mount -t proc none /proc
mount -t tmpfs none /tmp

# Try to mount memory card
if [ -b /dev/bu0 ]; then
    echo "Mounting memory card..."
    mount -t ext2 /dev/bu0 /mnt/card 2>/dev/null && \
        echo "Memory card mounted at /mnt/card" || \
        echo "Memory card not formatted or not present"
fi

echo ""
echo "Welcome to Blackroo Linux!"
echo "Type 'help' for BusyBox commands"
echo ""

exec /bin/sh
```

### Full InitRD (8MB System — 1MB+ budget)

```
/ (root)
├── bin/
│   ├── busybox          # Full BusyBox (FLAT, all useful applets)
│   ├── [all standard symlinks + extras]
│   ├── awk -> busybox
│   ├── sed -> busybox
│   ├── tr -> busybox
│   ├── xargs -> busybox
│   ├── wget -> busybox  # (if networking ever added)
│   ├── tar -> busybox
│   ├── gzip -> busybox
│   ├── unzip -> busybox
│   └── stty -> busybox  # Serial port config
├── sbin/
│   ├── init -> ../bin/busybox
│   ├── halt -> ../bin/busybox
│   ├── reboot -> ../bin/busybox
│   ├── swapon -> ../bin/busybox
│   ├── swapoff -> ../bin/busybox
│   ├── mkswap -> ../bin/busybox
│   ├── fsck -> ../bin/busybox
│   └── mke2fs -> ../bin/busybox
├── dev/
│   ├── [all from 4MB config]
│   ├── random           # c 1 8
│   ├── urandom          # c 1 9
│   └── kmem             # c 1 2
├── etc/
│   ├── inittab          # Full init config
│   ├── passwd
│   ├── group
│   ├── shadow           # Hashed passwords
│   ├── fstab
│   ├── profile
│   ├── hostname         # "blackroo"
│   ├── issue            # Login banner
│   ├── motd             # Message of the day
│   ├── init.d/
│   │   ├── rcS          # System init
│   │   ├── S01mount     # Mount filesystems
│   │   ├── S02memcard   # Detect and mount memory cards
│   │   └── S99local     # Local customization
│   └── blackroo/
│       └── boot.conf    # Boot configuration
├── proc/
├── sys/
├── tmp/
├── var/
│   ├── log/             # (tmpfs - in RAM)
│   ├── run/
│   └── tmp/
├── mnt/
│   ├── card/            # Primary memory card RAID
│   └── card2/           # Secondary card mount
├── home/
│   └── root/
│       └── .profile     # User shell profile
├── usr/
│   └── share/
│       └── blackroo/
│           └── welcome.txt  # Project info
└── init                 # Main init (symlink to /sbin/init or script)
```

**init for 8MB (using BusyBox init):**

```
# /etc/inittab
::sysinit:/etc/init.d/rcS
::respawn:/bin/sh
ttyS0::respawn:/bin/sh
::shutdown:/bin/umount -a -r
```

**/etc/init.d/rcS:**
```sh
#!/bin/sh
echo ""
echo "  ____  _            _                      _ _"
echo " | __ )| | __ _  ___| | ___ __ ___   ___   | (_)_ __  _   ___  __"
echo " |  _ \\| |/ _\` |/ __| |/ / '__/ _ \\ / _ \\  | | | '_ \\| | | \\ \\/ /"
echo " | |_) | | (_| | (__|   <| | | (_) | (_) | | | | | | | |_| |>  <"
echo " |____/|_|\\__,_|\\___|_|\\_\\_|  \\___/ \\___/  |_|_|_| |_|\\__,_/_/\\_\\"
echo ""
echo " PlayStation 1 Linux - $(cat /proc/meminfo 2>/dev/null | head -1)"
echo ""

# Mount virtual filesystems
mount -t proc none /proc
mount -t tmpfs none /tmp
mount -t tmpfs none /var/log
mount -t tmpfs none /var/run

# Set hostname
hostname blackroo

# Show memory info
echo "RAM: $(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null || echo 'unknown') KB"
echo ""

# Detect and mount memory cards
echo "Detecting memory cards..."
for i in 0 1 2 3 4 5 6 7; do
    if [ -b /dev/bu$i ]; then
        size=$(cat /proc/partitions 2>/dev/null | grep "bu$i" | awk '{print $3}')
        if [ -n "$size" ] && [ "$size" -gt 0 ]; then
            echo "  Card $i: ${size}KB"
        fi
    fi
done

# Try to mount joined card device
if [ -b /dev/bul ]; then
    echo "Mounting memory card RAID..."
    mount -t ext2 -o ro /dev/bul /mnt/card 2>/dev/null && \
        echo "  Mounted /mnt/card ($(df -h /mnt/card 2>/dev/null | tail -1 | awk '{print $2}'))" || \
        echo "  Memory card RAID not available"
fi

echo ""
echo "System ready. Type 'help' for commands."
echo ""
```

---

## BusyBox Cross-Compilation

### BusyBox Configuration for PS1

BusyBox must be compiled as a BINFMT_FLAT binary for uClinux:

```bash
# Prerequisites
# - mipsel-linux-gcc (EGCS 2.91.66 or compatible)
# - BusyBox source (1.x series recommended for 2.4 kernel compatibility)
# - elf2flt tool (ELF to FLAT converter)

# Download BusyBox
wget https://busybox.net/downloads/busybox-1.1.1.tar.bz2
tar xjf busybox-1.1.1.tar.bz2
cd busybox-1.1.1

# Configure
make CROSS_COMPILE=mipsel-linux- defconfig

# Customize for size
make CROSS_COMPILE=mipsel-linux- menuconfig
# Disable: Networking (unless needed)
# Disable: Large file support
# Disable: Unicode support
# Enable: Static linking
# Set: Installation prefix

# Build
make CROSS_COMPILE=mipsel-linux- \
     CFLAGS="-Os -mips1 -EL" \
     LDFLAGS="-static -elf2flt"

# Result: busybox (FLAT binary)
```

### BusyBox Applet Selection by RAM Level

| Applet | 2MB | 4MB | 8MB | Size Impact |
|--------|-----|-----|-----|-------------|
| sh/ash | Yes | Yes | Yes | ~20 KB |
| ls | Yes | Yes | Yes | ~5 KB |
| cat | Yes | Yes | Yes | ~2 KB |
| echo | Yes | Yes | Yes | ~1 KB |
| mount/umount | Yes | Yes | Yes | ~5 KB |
| mkdir/rmdir | Yes | Yes | Yes | ~2 KB |
| cp/mv/rm | Yes | Yes | Yes | ~3 KB |
| ps | Yes | Yes | Yes | ~3 KB |
| kill | Yes | Yes | Yes | ~1 KB |
| dmesg | Yes | Yes | Yes | ~2 KB |
| df | Yes | Yes | Yes | ~2 KB |
| grep | No | Yes | Yes | ~5 KB |
| find | No | Yes | Yes | ~5 KB |
| vi | No | Yes | Yes | ~15 KB |
| head/tail | No | Yes | Yes | ~3 KB |
| sort/wc | No | Yes | Yes | ~4 KB |
| awk | No | No | Yes | ~15 KB |
| sed | No | No | Yes | ~8 KB |
| tar/gzip | No | No | Yes | ~10 KB |
| dd | No | Yes | Yes | ~3 KB |
| hexdump | No | Yes | Yes | ~3 KB |
| mkfs.ext2 | No | No | Yes | ~10 KB |
| stty | No | No | Yes | ~3 KB |
| free | No | Yes | Yes | ~2 KB |
| init | No | No | Yes | ~5 KB |
| **Total (est.)** | **~45 KB** | **~85 KB** | **~130 KB** | |

### Pre-Built BusyBox Binary

A pre-built MIPSEL BusyBox binary exists in the project at `tools/busybox-mipsel` (1.5MB). However, this may be an ELF binary rather than FLAT format. It needs to be verified:

```bash
file tools/busybox-mipsel
# Should show: BFLT (binary FLAT) for it to work on uClinux
# If it shows: ELF 32-bit LSB, it won't work without BINFMT_ELF
```

If the existing binary is ELF, we need either:
1. Enable `CONFIG_BINFMT_ELF=y` in the kernel (adds ~10KB to kernel)
2. Recompile BusyBox with `-elf2flt` to produce FLAT format

---

## InitRD Creation Scripts

### Using genext2fs (No Root Required)

```bash
#!/bin/bash
# make_initrd_8mb.sh - Create full initrd for 8MB systems

INITRD_ROOT="./build/initrd_root"
INITRD_IMG="./output/initrd.img"
BUSYBOX="./tools/busybox-mipsel"
SIZE_KB=1024  # 1MB initrd

# Create directory structure
mkdir -p $INITRD_ROOT/{bin,sbin,dev,etc/init.d,proc,sys,tmp,mnt/card,var/log,var/run,home/root,usr/share/blackroo}

# Install BusyBox
cp $BUSYBOX $INITRD_ROOT/bin/busybox
chmod 755 $INITRD_ROOT/bin/busybox

# Create applet symlinks
cd $INITRD_ROOT/bin
for cmd in sh ash ls cat echo mount umount mkdir rmdir rm cp mv ln \
           ps kill dmesg df du free head tail grep find sort wc vi \
           chmod chown date dd hexdump awk sed tr xargs tar gzip stty; do
    ln -sf busybox $cmd
done
cd -

cd $INITRD_ROOT/sbin
for cmd in init halt reboot swapon swapoff mkswap fsck mke2fs; do
    ln -sf ../bin/busybox $cmd
done
cd -

# Create init scripts
cat > $INITRD_ROOT/etc/inittab << 'EOF'
::sysinit:/etc/init.d/rcS
::respawn:/bin/sh
ttyS0::respawn:/bin/sh
::shutdown:/bin/umount -a -r
EOF

cat > $INITRD_ROOT/etc/init.d/rcS << 'INITEOF'
#!/bin/sh
# [rcS content as shown above]
INITEOF
chmod 755 $INITRD_ROOT/etc/init.d/rcS

# Create /etc files
echo "root::0:0:root:/home/root:/bin/sh" > $INITRD_ROOT/etc/passwd
echo "root:x:0:" > $INITRD_ROOT/etc/group
echo "blackroo" > $INITRD_ROOT/etc/hostname
echo "Blackroo Linux 0.2 \\n \\l" > $INITRD_ROOT/etc/issue

# Create linuxrc (fallback init)
cat > $INITRD_ROOT/linuxrc << 'EOF'
#!/bin/sh
exec /sbin/init
EOF
chmod 755 $INITRD_ROOT/linuxrc

# Device table for genext2fs
cat > /tmp/device_table_8mb.txt << 'EOF'
/dev        d  755  0  0  -  -  -  -  -
/dev/console    c  600  0  0  5  1  -  -  -
/dev/null       c  666  0  0  1  3  -  -  -
/dev/zero       c  666  0  0  1  5  -  -  -
/dev/random     c  444  0  0  1  8  -  -  -
/dev/urandom    c  444  0  0  1  9  -  -  -
/dev/ram0       b  600  0  0  1  0  -  -  -
/dev/ram1       b  600  0  0  1  1  -  -  -
/dev/ttyS0      c  666  0  0  4  64 -  -  -
/dev/tty0       c  666  0  0  4  0  -  -  -
/dev/bu0        b  660  0  0  60 0  -  -  -
/dev/bu1        b  660  0  0  60 1  -  -  -
/dev/bu2        b  660  0  0  60 2  -  -  -
/dev/bu3        b  660  0  0  60 3  -  -  -
/dev/bu4        b  660  0  0  60 4  -  -  -
/dev/bu5        b  660  0  0  60 5  -  -  -
/dev/bu6        b  660  0  0  60 6  -  -  -
/dev/bu7        b  660  0  0  60 7  -  -  -
/dev/bul        b  660  0  0  61 0  -  -  -
EOF

# Create ext2 image
genext2fs -b $SIZE_KB -d $INITRD_ROOT -D /tmp/device_table_8mb.txt -N 256 -m 0 $INITRD_IMG

echo "InitRD created: $INITRD_IMG (${SIZE_KB}KB)"
ls -lh $INITRD_IMG
```

### Size Comparison

| InitRD Type | Uncompressed | Compressed | Target RAM |
|-------------|-------------|------------|------------|
| Micro | ~170 KB | ~80-100 KB | 2 MB |
| Standard | ~350 KB | ~150-200 KB | 4 MB |
| Full | ~700 KB | ~300-400 KB | 8 MB |

---

## Persistent Storage Integration

### Hybrid Boot Strategy

```
Phase 1: Boot from initrd (RAM)
  - Kernel mounts initrd as /
  - All system binaries in RAM (fast)
  - /proc, /sys, /tmp in RAM (tmpfs)

Phase 2: Mount persistent storage (memory cards)
  - Detect memory cards
  - Mount ext2 on /mnt/card
  - Bind-mount persistent directories:
      mount --bind /mnt/card/home /home
      mount --bind /mnt/card/etc/local /etc/local

Phase 3: Interactive use
  - System files: RAM (fast, read-only)
  - User data: memory card (slow, writable)
  - Temp files: RAM (tmpfs)
```

### fstab Example

```
# /etc/fstab for Blackroo Linux (8MB)
#
# <device>     <mount>      <type>  <options>       <dump> <pass>
proc           /proc        proc    defaults        0      0
tmpfs          /tmp         tmpfs   size=512k       0      0
tmpfs          /var/log     tmpfs   size=64k        0      0
tmpfs          /var/run     tmpfs   size=32k        0      0
/dev/bul       /mnt/card    ext2    ro,noatime      0      0
```

---

## Kernel Command Line Options

### Examples

```bash
# 8MB system, boot from ramdisk, serial console
"console=ttyS0,115200 root=/dev/ram0 rw init=/linuxrc"

# 8MB system, boot from ramdisk, mount memory card later
"console=ttyS0,115200 root=/dev/ram0 rw init=/sbin/init"

# 2MB system, minimal boot
"console=ttyS0,115200 root=/dev/ram0 rw init=/bin/sh"

# Boot directly from memory card (if filesystem exists)
"console=ttyS0,115200 root=/dev/bu0 ro init=/sbin/init"
```

---

*Blackroo Linux Ramdisk and Root Filesystem Design*
