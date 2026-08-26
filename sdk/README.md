# sdk/ — Bundled MIPS Cross-Toolchain

This directory contains the **complete cross-compiler** used to build the
Blackroo Linux kernel. It is bundled on purpose: the kernel is a Linux 2.4 tree
that only builds cleanly with the period-correct compiler, and shipping it means
**anyone can build with zero toolchain setup** beyond 32-bit library support.

```
sdk/
└── toolchain/                 EGCS 2.91.66, target mipsel-linux (little-endian MIPS-I / R3000)
    ├── bin/                   mipsel-linux-gcc, -as, -ld, -ar, -objcopy, ...
    ├── lib/gcc-lib/mipsel-linux/egcs-2.91.66/   cc1, libgcc, internal headers
    ├── mipsel-linux/          target binutils + libs
    └── include/  man/  info/
```

## What it is

| | |
|---|---|
| Compiler | **EGCS 2.91.66** (the egcs fork of GCC, ~1999 vintage) |
| Target triple | `mipsel-linux` (little-endian MIPS) |
| CPU | R3000A, MIPS-I (`-mcpu=r3000 -mips1`) |
| Origin | The original Runix/PSXLinux cross-tools, preserved from the project archive |
| Host | 32-bit x86 binary → **requires `libc6:i386` on a 64-bit host** |

## How `build.sh` uses it

`build.sh` never touches your system compiler for the kernel. It sets:

```sh
SDK_DIR="$PROJECT_ROOT/sdk/toolchain"
export GCC_EXEC_PREFIX="$SDK_DIR/lib/gcc-lib/"     # lets the relocated gcc find cc1/as/ld
PATH="$SDK_DIR/bin:$PATH"                          # during the kernel make only
```

`GCC_EXEC_PREFIX` is the key: it makes this EGCS build **relocatable**, so the
toolchain works from `sdk/toolchain/` regardless of where you clone the repo.
(The host-side converter tools — `elf2psexe`, `addpsexe_initrd`, `mkmemcard` —
are built with your **native** `gcc`, with `GCC_EXEC_PREFIX` unset.)

## Verify it runs

```bash
GCC_EXEC_PREFIX="$PWD/sdk/toolchain/lib/gcc-lib/" \
  ./sdk/toolchain/bin/mipsel-linux-gcc --version
# -> egcs-2.91.66
```

If that prints an interpreter / "No such file" error on a 64-bit host, you are
missing 32-bit support:

```bash
sudo apt install libc6:i386
```

## Why not a modern `gcc-mips-linux-gnu`?

This Linux 2.4 tree predates many modern GCC changes (stricter aliasing, header
layout, inline-asm constraints, default PIC). Modern MIPS GCC produces a long
tail of build breaks. The bundled EGCS is the compiler the code was written
against, so it builds clean and produces a known-good kernel. Porting the tree
to a modern toolchain is a possible future task — see `roadmap.md`.

## License

EGCS/GCC is GPL. binutils is GPL. These are redistributed under those terms.
See the individual `COPYING` files within the toolchain tree.
