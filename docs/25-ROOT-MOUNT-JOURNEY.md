# The road to a root filesystem

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: BINFMT_FLAT.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> Running log of the attempt to get a real userspace process on Blackroo Linux.
> Started 2026-08-21, after Linux was already booting from CD with a monitor
> prompt and 381 KB of memory card storage.
>
> Status: **done.** A userspace shell runs on the console.

---

## 1. Why a root filesystem was never the first problem

The obvious plan — "make a filesystem, boot with `root=`" — is wrong on this
machine, and it is worth being explicit about why:

- This kernel builds **`CONFIG_BINFMT_FLAT` and no ELF loader**;
  `fs/binfmt_elf.o` does not exist in the tree.
- Every BusyBox in the tree is a **static ELF linked at `0x00400000`** — 4 MB,
  on a console with **2 MB** and an R3000A with **no MMU** to map it elsewhere.

So `execve()` could never succeed. Mounting a root filesystem would only move
the panic from "cannot mount root" to "no init found". **One runnable binary
had to come first.**

## 2. binfmt_fixed — the loader

Rather than chase bFLT (see §8), the approach was the one small no-MMU systems
have always used: link the program at a fixed address, reserve that memory so
the kernel never allocates it, copy the segments there, jump.

`fs/binfmt_fixed.c`, ~150 lines:

- validates the ELF (ET_EXEC, EM_MIPS)
- **checks every `PT_LOAD` lands inside the reserved window before touching
  anything** — a binary linked for the wrong address is rejected cleanly, not
  discovered halfway through overwriting the kernel
- `kernel_read()`s the segments to their `p_vaddr`, zeroes the bss tail
- builds argc/argv/envp on the stack, `start_thread()`

The window is reserved in `arch/mipsnommu/ps/prom/memory.c` by simply
declaring less RAM to the kernel than the machine has:

```c
#ifdef CONFIG_BINFMT_FIXED
    if (mem_size > BLACKROO_USER_RESERVE)
        mem_size -= BLACKROO_USER_RESERVE;
#endif
```

The allocator never sees that memory, so nothing else needs to know it is
special. It started at 256 KB, which left only 39 free pages (156 KB) —
absurd for a 1.2 KB shell — and was cut to **64 KB at `0x801f0000`**.

## 3. brsh — the first userspace program

`userland/brsh.c`: a freestanding MIPS binary, no libc, raw syscalls
(`write=4004`, `read=4003`, `exit=4001`), with its own `_start` that picks
argc/argv off the stack. Builtins: `help`, `echo`, `exit`.

Built with **`mipsel-none-elf-gcc` 12.3.0**, the toolchain that arrived with
PSn00bSDK — it targets bare metal, which is exactly right for code that makes
its own syscalls. **1,259 bytes.**

One wrinkle: linking with plain `-Ttext` left `.MIPS.abiflags` stranded at the
toolchain's default `0x00400000`, producing a second `PT_LOAD` outside the
window that the loader (correctly) rejected. `userland/blackroo.ld` places
everything in one segment and discards the MIPS metadata sections.

## 4. The initrd: four bugs in a row

Each of these produced the same user-visible symptom, and each hid the next.
The April-era note "Ramdisk driver reports 'Couldn't find valid RAM disk
image'" was all four of them at once.

### 4.1 Placed into BSS, then erased

`addpsexe_initrd` positioned the image after `t_addr + t_size` — the end of the
**loaded image**, text and data. The kernel searches for the `INRD` magic after
**`_end`**, which is past BSS: 128 KB further on. So the kernel looked in the
wrong place, *and* `head.S` zeroes BSS on entry, so the image was being erased
before anything could find it.

Fix: `build.sh` reads `_end` out of the ELF with `nm` and passes it to the
tool, which pads to the right place.

### 4.2 The tool then overflowed its own buffer

With `kernel_end` now meaning `_end`, padding was still computed as
`initrd_offset - kernel_end`, but BSS occupies **no file space** — so the
output buffer was under-allocated by the size of BSS and `glibc` aborted with
`*** buffer overflow detected ***`. Padding must be measured from the end of
the data actually in the file.

### 4.3 A copy that reported success without copying

`rd.c` copied the initrd into `/dev/ram0` through the block device's file
write op — **and ignored the return value**:

```c
outfile.f_op->write(&outfile, (char *)initrd_start, size, &pos);
printk("BLACKROO: initrd copied to ram0, %lu bytes\n", size);   /* regardless */
```

A later `bread()` of block 1 returned uninitialised slab memory — the dump
contained the string `size-512`, which is a slab cache name.

### 4.4 The ramdisk was fine; proving it took a round trip

`rd.c` keeps ramdisk contents **in the buffer cache**: `getblk()` per block,
pinned with `mark_buffer_protected()`. A read that finds no matching cached
buffer silently returns a fresh one full of whatever was in that memory.

Rather than reason about which half was broken, a `blktest` command was added
to the monitor: write a signature through `getblk`, read it back with `bread`.

```
blackroo> blktest 1 0 1
writing signature to 1:0 block 1
  written, now reading it back
  first 8 bytes: 42 52 4d 4e b4 b5 b6 b7      ("BRMN")
  OK - write and read agree, the device works
```

So the device round-trips. The copy was rewritten to use that exact path —
block by block, `getblk` + `mark_buffer_protected` — **and to verify itself**
by reading block 1 back and checking for the ext2 magic.

### 4.5 The bootmem bitmap sat on top of the initrd

With verification in place, the truth came out:

```
BLACKROO: copying initrd 28672 bytes (28 blocks) into /dev/ram0
BLACKROO: initrd in ram0, superblock magic 0000 (NOT ext2)
```

Dumping the initrd memory from the monitor showed kernel data structures:

```
blackroo> md 8016d000 8
8016d000: 001e9000 0014a9cc 000000c0 0016d0c0  |................|
```

`arch/mipsnommu/kernel/setup.c` sets `start_pfn = PFN_UP(__pa(&_end))` and
hands that to `init_bootmem()` — which places its **bitmap on the first page
after the kernel**, which is exactly where the initrd is written. The existing
`reserve_bootmem()` call for the initrd came *after* the bitmap had already
been laid on top of it.

Fix: detect the `INRD` magic **before** `init_bootmem()` and start the
allocator above the image.

Result:

```
BLACKROO: copying initrd 28672 bytes (28 blocks) into /dev/ram0
BLACKROO: initrd in ram0, superblock magic ef53 (ext2 OK)
```

## 5. Building the filesystem without root

`genext2fs` is not installed and there is no root access. `mke2fs -d` populates
an image from a directory, and `debugfs` creates device nodes:

```bash
mke2fs -q -F -b 1024 -d build/rootfs -t ext2 -I 128 -E revision=0 -N 32 \
       output/initrd.img 64
printf 'cd /dev\nmknod console c 5 1\nmknod tty0 c 4 0\nmknod ram0 b 1 0\nquit\n' \
  | debugfs -w -f /dev/stdin output/initrd.img
```

Notes: `-r 0` was removed from modern mke2fs (use `-E revision=0`), and
`debugfs -R "mknod ..."` silently fails — the commands must come from a script
file with a `cd` first. Verify with `dumpe2fs -h`: revision 0, features
`(none)`.

## 6. Tools built along the way

Everything here was found by adding instrumentation, not by reasoning:

| Command | Purpose |
|---|---|
| `blk <maj> <min> <blk>` | read any block device through `bread()`, dump it, check ext2 magic |
| `blktest <maj> <min> <blk>` | write a signature and read it back — isolates "write stored nothing" from "read cannot find it" |
| `md` / `mem` | dump memory, show `_end`, initrd range, free pages |

## 7. Where it stands — the open problem

The filesystem is on the device and verified. The failure is now **earlier than
the mount**:

```
VFS: Cannot open root device "ram0" or 01:00
Kernel panic: VFS: Unable to mount root fs on 01:00
```

Reading `fs/super.c`, that message comes from a failed
`blkdev_get(bdev, mode, 0, BDEV_FS)` — the **open**, before any superblock is
read. So this is no longer a data problem.

Prime suspect, unverified: `rd_open()` in `drivers/block/rd.c` has

```c
if (rd_inode[DEVICE_NR(inode->i_rdev)] == NULL) {
        if (!inode->i_bdev) return -ENXIO;
```

If the inode `blkdev_get()` passes in has no `i_bdev` at that moment, `rd_open`
returns `-ENXIO` and the open fails exactly as observed. Next step is to print
the `retval` from `blkdev_get()` and instrument `rd_open()`, rather than guess
again.

## 8. Roads not taken (and why)

**bFLT with GOT relocation** — the "correct" uClinux answer. Experiments with
the bundled EGCS 2.91.66 showed plain builds emit `R_MIPS_26` absolute call
targets that bFLT relocations cannot patch, and `-membedded-pic` is overridden
by the target's `-mabicalls` default. Possible, but a research project.

**Rebuilding BusyBox linked low** — needs BusyBox source plus a Linux-targeting
MIPS toolchain, and still lands on the same loader question. Do it after a
loader works.

**Root on the CD-ROM** — the best long-term answer (700 MB the console already
reads, against 420 KB of RAM) but there is **no CD driver and no `fs/isofs`**
in the tree; both were stripped. The kernel also cannot use BIOS calls, because
`prom_free_prom_memory()` hands the BIOS scratch area to the page allocator.
That is a driver project of its own — see `docs/24-CDROM-DRIVER-RESEARCH.md`.

## 9. Lessons that keep repeating

- **Verify, do not report.** Three separate stages here claimed success while
  doing nothing: a copy that ignored its return value, a placement computed
  from the wrong symbol, a reservation that came too late.
- **Round-trip tests beat reasoning.** `blktest` settled in one boot what two
  cycles of theorising could not.
- **Each fix reveals the next bug, not the finish line.** Four initrd faults
  stacked, all presenting as the same message.


---

## 10. The rest of the way (the same evening)

### 10.1 The root device would not open

With the filesystem verified on `/dev/ram0`, `mount_root()` still failed. The
message says "Cannot open root device", and reading `fs/super.c` showed that
comes from `blkdev_get()` - the **open**, before any superblock is read. So a
printk of the errno went in, and `rd_open()` was traced:

```
RAMDISK: rd_open(minor 0)
RAMDISK: rd_open(minor 0): no i_bdev, returning -ENXIO
VFS: blkdev_get(01:00) failed, retval = -6
```

`rd_open()` refused any inode without `i_bdev` - but `blkdev_get()`, which is
how `mount_root()` opens the root device, passes a fake inode that has none.
The check guards only the "immunize against `invalidate_buffers()`"
optimisation, so it now skips that step instead of failing the open. Upstream
2.4 uses `bdget()` here rather than leaning on the inode.

```
VFS: Mounted root (ext2 filesystem).
```

### 10.2 The syscall numbers were from a different kernel

`binfmt_fixed` loaded `/bin/sh` and jumped to it, and nothing happened. A raw
SIO1 marker at `_start` - possible because the process could reach the
hardware directly - proved the program *was* executing.

The program was using `write = 4004`, the standard Linux/MIPS o32 number.
**This tree sets `__NR_Linux = 0`** and indexes `sys_call_table` with `v0`
directly, so the numbers are the small ones: `exit 1`, `read 3`, `write 4`.
4004 exceeds `MAX_SYSCALL_NO` (216), lands in `illegal_syscall`, and returns
`-ENOSYS` silently.

`tools/host/mkinit.c` - the FLAT init experiment from April - has the same
wrong numbers, so it could never have worked either.

### 10.3 write() to the console never returned

With the numbers fixed, `sys_write` was reached with the right arguments and
the machine hung inside it. `/dev/console` goes through the tty layer, and
this port's `serial_psx.c` has no working transmit completion path (it also
compiles with an implicit declaration of `sio_ready()`), so the process slept
in the line discipline forever.

Fixing the tty layer is a project. `drivers/char/brcon.c` is the small thing
that unblocked userspace: a polled SIO1 character device, no tty, no
interrupts, using exactly the approach the in-kernel monitor has used
reliably all along. It provides **input** as well, with echo and line
handling. The shell opens `/dev/brcon` and uses it for stdin and stdout.

### 10.4 Kernel-mode processes broke `current`

Because the program was linked in KSEG0 (`0x801f0000`), which user mode cannot
touch, `start_thread` was changed to leave processes in **kernel mode**. That
seemed right for a no-MMU port, and it is wrong:

On MIPS, `current` is derived from the kernel stack pointer, and a kernel-mode
trap does not switch stacks. So a syscall from such a process computed
`current` from the *program's own stack* and got garbage - visible as
`pid=0` in a debug print - and crashed inside `sys_open` before reaching any
driver. The crash landed execution back in kloader, still resident at
`0x80010000`, which is why the console started emitting `BK>>` beacons.

The correct arrangement: **user mode, with the program in KUSEG.** The PS1
maps KUSEG straight to main RAM (`0x00000000-0x001FFFFF` mirrors
`0x80000000-0x801FFFFF`) with no TLB involved, so the same reserved physical
memory is simply linked and loaded at `0x001f0000`. User mode reaches it, and
the trap path switches stacks as it expects to.

`pid 1` in the output confirms `current` resolves properly now.

(This also disproves the comment in `head.S` claiming KUSEG access needs TLB
entries. This CPU has no TLB, so that refill code has never done anything.)

### 10.5 Result

```
binfmt_fixed: /bin/sh at 0x1f0000..0x1f0840, entry 0x1f03fc, stack 0x1fffb8
brcon: open by pid 1
brsh: open returned 0x00000003
brcon: first write, 1 bytes
brsh: userspace is alive on the PlayStation.
  argc=0x00000001 argv0=init
  a process, not the monitor.

$ help
builtins: help, echo <x>, exit
$ echo hello from a real process
hello from a real process
```

Nine faults between "no userspace has ever run here" and that prompt: the
initrd placed into BSS, a tool buffer overflow, a copy that ignored its return
value, a bootmem bitmap on top of the image, a device open that refused fake
inodes, syscall numbers from a different kernel, a tty layer that never
returns, kernel-mode processes breaking `current`, and a program linked in a
segment user mode cannot reach.

## 11. What is still missing

- **The tty layer** (`serial_psx.c`) still cannot carry a userspace write.
  `/dev/console` is unusable from a process; `/dev/brcon` is the workaround.
- **Memory-card root is untested.** `bul` *is* in `root_dev_names[]`
  (`init/main.c`, `{ "bul", (BU_LARGE_MAJOR << MINORBITS) }`, under
  `CONFIG_PSX_MEM_CARD`/`CONFIG_PSX_LARGE_CARD`, both set), so `root=/dev/bul`
  resolves. What is unproven is **writing** through the driver - only reads via
  `bread()` have ever been exercised - and there is no ext2 image on the cards
  to mount. Corrected 2026-08-21 by inspection; the earlier claim here that the
  name did not resolve was wrong.
- **One process at a time.** `binfmt_fixed` loads at a fixed address, so there
  is no `fork`+`exec` of a second program. Enough for a shell with builtins;
  not enough for pipelines.
- **No real utilities** - the shell has builtins only. Anything more wants
  either a bigger custom program or the bFLT work.
