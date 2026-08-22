# Blackroo Linux - Docker Build System and PSn00bSDK

> Containerized build environment for kernel compilation and PS1 homebrew development

---

## Overview

The Blackroo build system uses Docker containers to provide reproducible build environments:

1. **Kernel Build Container** — EGCS 2.91.66 mipsel cross-compiler for Linux 2.4 kernel
2. **PSn00bSDK Container** — Modern PS1 homebrew SDK for bootloader and tools
3. **InitRD Builder** — Creates root filesystem images

---

## Container 1: Kernel Build Environment

### Dockerfile

```dockerfile
# Dockerfile.kernel - Blackroo Linux Kernel Build Environment
#
# Provides the EGCS 2.91.66 mipsel-linux cross-compiler
# and all dependencies needed to build the Linux 2.4 kernel.
#
# Usage:
#   docker build -t blackroo-kernel -f Dockerfile.kernel .
#   docker run -v $(pwd):/build blackroo-kernel make

FROM ubuntu:22.04

LABEL maintainer="Blackroo Linux Project"
LABEL description="Linux 2.4 kernel build environment for PlayStation 1 (MIPS R3000)"

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
# Note: We need 32-bit support because the EGCS toolchain is a 32-bit binary
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y \
        build-essential \
        flex \
        bison \
        bc \
        ncurses-dev \
        libncurses5-dev \
        libc6:i386 \
        libstdc++5:i386 \
        zlib1g:i386 \
        genext2fs \
        gzip \
        cpio \
        wget \
        git \
        python3 \
        file \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Copy and install the EGCS mipsel cross-compiler toolchain
# The toolchain extracts to /usr/local/bin/, /usr/local/lib/gcc-lib/, etc.
COPY Archive/toolchain/mipsel/ /tmp/toolchain/
RUN cd /tmp/toolchain && \
    if [ -f *.tar.gz ]; then \
        for f in *.tar.gz; do tar xzf "$f" -C /; done; \
    fi && \
    rm -rf /tmp/toolchain

# Alternatively, if using the packaged toolchain tarball:
# COPY tools/blackroo_mips_i586v1.tar.gz /tmp/
# RUN tar xzf /tmp/blackroo_mips_i586v1.tar.gz -C / && rm /tmp/*.tar.gz

# Set up environment
ENV PATH="/usr/local/bin:${PATH}"
ENV GCC_EXEC_PREFIX="/usr/local/lib/gcc-lib/"
ENV CROSS_COMPILE="mipsel-linux-"

# Verify toolchain
RUN mipsel-linux-gcc --version || echo "WARNING: Toolchain not installed correctly"

# Copy host tool sources and build them
COPY tools/elf2psx.c /usr/local/src/
COPY tools/addpsexe_initrd.c /usr/local/src/
RUN cd /usr/local/src && \
    gcc -o /usr/local/bin/elf2psx elf2psx.c -Wall 2>/dev/null || true && \
    gcc -o /usr/local/bin/addpsexe_initrd addpsexe_initrd.c -Wall 2>/dev/null || true

# Working directory
WORKDIR /build

# Default command: build the kernel
CMD ["bash", "-c", "cd blackroo && make dep && make"]
```

### Usage

```bash
# Build the Docker image
docker build -t blackroo-kernel -f Dockerfile.kernel .

# Build the kernel
docker run --rm -v $(pwd):/build blackroo-kernel

# Run make menuconfig (needs terminal)
docker run --rm -it -v $(pwd):/build blackroo-kernel \
    bash -c "cd blackroo && cp Config .config && make menuconfig"

# Clean build
docker run --rm -v $(pwd):/build blackroo-kernel \
    bash -c "cd blackroo && make mrproper"

# Build kernel + convert to PS-EXE
docker run --rm -v $(pwd):/build blackroo-kernel \
    bash -c "cd blackroo && make dep && make && \
             elf2psx -p linux ../output/kernel.exe"

# Build initrd
docker run --rm -v $(pwd):/build blackroo-kernel \
    bash -c "./scripts/make_initrd_noroot.sh"

# Interactive shell
docker run --rm -it -v $(pwd):/build blackroo-kernel bash
```

---

## Container 2: PSn00bSDK Build Environment

### About PSn00bSDK

[PSn00bSDK](http://lameguy64.net/?page=psn00bsdk) is an open-source PlayStation 1 SDK that provides:
- GCC-based MIPS cross-compiler (modern GCC, not the ancient EGCS)
- PlayStation hardware libraries (GPU, SPU, CD, SIO, controller, memory card)
- Linker scripts for PS-EXE generation
- CMake-based build system
- No proprietary Sony SDK files required

### Dockerfile

```dockerfile
# Dockerfile.psn00bsdk - PSn00bSDK Build Environment
#
# Provides PSn00bSDK for building PlayStation 1 homebrew applications
# (bootloader, memory card tools, etc.)
#
# Usage:
#   docker build -t blackroo-psn00bsdk -f Dockerfile.psn00bsdk .
#   docker run -v $(pwd)/bootloader:/build blackroo-psn00bsdk

FROM ubuntu:22.04

LABEL maintainer="Blackroo Linux Project"
LABEL description="PSn00bSDK development environment for PS1 homebrew"

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && \
    apt-get install -y \
        build-essential \
        cmake \
        ninja-build \
        git \
        wget \
        python3 \
        python3-pip \
        texinfo \
        libgmp-dev \
        libmpfr-dev \
        libmpc-dev \
        pkg-config \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Clone and build PSn00bSDK
# This builds a complete MIPS toolchain + PS1 libraries from source
RUN git clone https://github.com/Lameguy64/PSn00bSDK.git /opt/psn00bsdk-src && \
    cd /opt/psn00bsdk-src && \
    git submodule update --init --recursive

# Build the PSn00bSDK toolchain
# This compiles GCC + binutils targeting mipsel-none-elf
RUN cd /opt/psn00bsdk-src && \
    cmake --preset default \
        -DPSN00BSDK_TC=/opt/psn00bsdk/toolchain \
        -DCMAKE_INSTALL_PREFIX=/opt/psn00bsdk && \
    cmake --build ./build && \
    cmake --install ./build

# Set up environment
ENV PSN00BSDK=/opt/psn00bsdk
ENV PATH="/opt/psn00bsdk/toolchain/bin:/opt/psn00bsdk/bin:${PATH}"

# Verify installation
RUN mipsel-none-elf-gcc --version && \
    echo "PSn00bSDK installed successfully"

WORKDIR /build

# Default command: build project with CMake
CMD ["bash", "-c", "cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PSN00BSDK/lib/libpsn00b/cmake/sdk.cmake && cmake --build build"]
```

### PSn00bSDK Project Template

```cmake
# bootloader/CMakeLists.txt
cmake_minimum_required(VERSION 3.21)

# PSn00bSDK toolchain setup
include($ENV{PSN00BSDK}/lib/libpsn00b/cmake/sdk.cmake)

project(blackroo-bootloader LANGUAGES C ASM)

psn00bsdk_add_executable(bootloader GPREL
    src/main.c
    src/gpu.c
    src/serial.c
    src/memcard.c
    src/cdrom.c
    src/inflate.c
    src/hardware.c
)

# Link PSn00bSDK libraries
target_link_libraries(bootloader PRIVATE
    psxgpu      # GPU rendering
    psxsio      # Serial I/O
    psxpad      # Controller input
    psxcd       # CD-ROM access
    psxspu      # Sound (for silence)
    psxapi      # BIOS API calls
    c           # C standard library
)

# Output PS-EXE
psn00bsdk_add_cd_image(bootloader-cd bootloader
    ${CMAKE_CURRENT_SOURCE_DIR}/iso/iso.xml
    OPTIONAL
)
```

### Usage

```bash
# Build PSn00bSDK container
docker build -t blackroo-psn00bsdk -f Dockerfile.psn00bsdk .

# Build bootloader
docker run --rm -v $(pwd)/bootloader:/build blackroo-psn00bsdk

# Interactive development
docker run --rm -it -v $(pwd)/bootloader:/build blackroo-psn00bsdk bash

# Build with custom CMake options
docker run --rm -v $(pwd)/bootloader:/build blackroo-psn00bsdk \
    bash -c "cmake -S . -B build -G Ninja \
             -DCMAKE_TOOLCHAIN_FILE=\$PSN00BSDK/lib/libpsn00b/cmake/sdk.cmake \
             -DCMAKE_BUILD_TYPE=Release && \
             cmake --build build"
```

---

## Container 3: Combined Build (Convenience)

### docker-compose.yml

```yaml
# docker-compose.yml - Blackroo Linux Build System
#
# Usage:
#   docker compose build          # Build all containers
#   docker compose run kernel     # Build kernel
#   docker compose run bootloader # Build bootloader
#   docker compose run initrd     # Build initrd
#   docker compose run all        # Build everything

services:
  kernel:
    build:
      context: .
      dockerfile: Dockerfile.kernel
    volumes:
      - .:/build
    command: >
      bash -c "
        cd blackroo &&
        [ ! -f .config ] && cp Config .config;
        [ ! -L include/asm ] && ln -sf asm-mipsnommu include/asm;
        make dep && make &&
        cp linux /build/output/linux.elf &&
        elf2psx -p /build/output/linux.elf /build/output/kernel.exe
      "

  bootloader:
    build:
      context: .
      dockerfile: Dockerfile.psn00bsdk
    volumes:
      - ./bootloader:/build
    command: >
      bash -c "
        cmake -S . -B build -G Ninja
          -DCMAKE_TOOLCHAIN_FILE=\$PSN00BSDK/lib/libpsn00b/cmake/sdk.cmake &&
        cmake --build build &&
        cp build/bootloader.exe /build/output/
      "

  initrd:
    build:
      context: .
      dockerfile: Dockerfile.kernel
    volumes:
      - .:/build
    command: ./scripts/make_initrd_noroot.sh

  all:
    build:
      context: .
      dockerfile: Dockerfile.kernel
    volumes:
      - .:/build
    command: >
      bash -c "
        echo '=== Building Kernel ===' &&
        cd blackroo &&
        [ ! -f .config ] && cp Config .config;
        [ ! -L include/asm ] && ln -sf asm-mipsnommu include/asm;
        make dep && make &&
        cp linux /build/output/linux.elf &&
        echo '=== Creating InitRD ===' &&
        cd /build && ./scripts/make_initrd_noroot.sh &&
        echo '=== Converting to PS-EXE ===' &&
        elf2psx -p output/linux.elf output/kernel.exe &&
        echo '=== Build Complete ===' &&
        ls -lh output/
      "
```

### Usage

```bash
# Build everything
docker compose run all

# Just the kernel
docker compose run kernel

# Just the initrd
docker compose run initrd

# Just the bootloader (requires PSn00bSDK container)
docker compose run bootloader
```

---

## CI/CD Pipeline (GitHub Actions)

### .github/workflows/build.yml

```yaml
name: Build Blackroo Linux

on:
  push:
    branches: [main]
  pull_request:
    branches: [main]

jobs:
  build-kernel:
    runs-on: ubuntu-latest
    container:
      image: ubuntu:22.04

    steps:
      - uses: actions/checkout@v4

      - name: Install dependencies
        run: |
          dpkg --add-architecture i386
          apt-get update
          apt-get install -y build-essential flex bison bc \
            ncurses-dev libc6:i386 genext2fs gzip

      - name: Install toolchain
        run: |
          # Extract toolchain from Archive
          # (specific extraction depends on how toolchain is packaged)
          tar xzf Archive/toolchain/mipsel/*.tar.gz -C / 2>/dev/null || true

      - name: Build kernel
        run: |
          export PATH="/usr/local/bin:$PATH"
          export GCC_EXEC_PREFIX="/usr/local/lib/gcc-lib/"
          cd blackroo
          cp Config .config
          ln -sf asm-mipsnommu include/asm
          make dep
          make

      - name: Convert to PS-EXE
        run: |
          gcc -o tools/elf2psx tools/elf2psx.c -Wall
          tools/elf2psx -p blackroo/linux output/kernel.exe

      - name: Build InitRD
        run: ./scripts/make_initrd_noroot.sh

      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: blackroo-build
          path: |
            output/linux.elf
            output/kernel.exe
            output/initrd.img
```

---

## Build Targets Reference

### Quick Reference

| Target | Command | Output |
|--------|---------|--------|
| Kernel (ELF) | `make` in blackroo/ | `blackroo/linux` |
| Kernel (PS-EXE) | `elf2psx -p linux.elf kernel.exe` | `output/kernel.exe` |
| Kernel (ECOFF) | `elf2ecoff linux linux.ecoff` | `output/linux.ecoff` |
| Kernel+InitRD | `addinitrd linux.ecoff initrd.img linux.image.ecoff` | `output/linux.image.ecoff` |
| InitRD | `make_initrd_noroot.sh` | `output/initrd.img` |
| Bootloader | CMake + PSn00bSDK | `bootloader/build/bootloader.exe` |
| ISO image | `mkpsxiso` | `output/blackroo.bin` + `.cue` |

### Full Build Pipeline Script

```bash
#!/bin/bash
# build_all.sh - Complete Blackroo Linux build pipeline

set -e

echo "=== Blackroo Linux Full Build ==="

# Step 1: Build kernel
echo "[1/5] Building kernel..."
cd blackroo
[ ! -f .config ] && cp Config .config
[ ! -L include/asm ] && ln -sf asm-mipsnommu include/asm
make dep
make
cd ..

# Step 2: Build host tools
echo "[2/5] Building tools..."
gcc -o tools/elf2psx tools/elf2psx.c -Wall 2>/dev/null || true
gcc -o tools/elf2ecoff blackroo/arch/mipsnommu/boot/elf2ecoff.c -Wall 2>/dev/null || true
gcc -o tools/addinitrd blackroo/arch/mipsnommu/boot/addinitrd.c -Wall 2>/dev/null || true

# Step 3: Create initrd
echo "[3/5] Creating initrd..."
./scripts/make_initrd_noroot.sh

# Step 4: Convert kernel formats
echo "[4/5] Converting kernel..."
mkdir -p output
cp blackroo/linux output/linux.elf
tools/elf2psx -p output/linux.elf output/kernel.exe
tools/elf2ecoff output/linux.elf output/linux.ecoff 2>/dev/null || true
tools/addinitrd output/linux.ecoff output/initrd.img output/linux.image.ecoff 2>/dev/null || true

# Step 5: Summary
echo "[5/5] Build complete!"
echo ""
echo "Output files:"
ls -lh output/
echo ""
echo "Upload to PS1: nops /exe output/kernel.exe COM3"
echo "Upload (fast): nops /fast /exe output/kernel.exe COM3"
```

---

## Toolchain Compatibility Notes

### EGCS 2.91.66 Limitations

The EGCS toolchain is from 1999 and has specific requirements:

| Requirement | Details |
|-------------|---------|
| Host architecture | x86 (32-bit binary) |
| 32-bit libs | `libc6:i386`, `libstdc++5:i386` on 64-bit hosts |
| C standard | Pre-C99 (no `//` comments in some configs, no `long long` in some places) |
| Kernel version | Linux 2.4 only (API mismatch with 2.6+) |
| Output format | ELF 32-bit MIPS little-endian |

### PSn00bSDK GCC (Modern)

PSn00bSDK uses a modern GCC (12+) targeting `mipsel-none-elf`:

| Feature | EGCS (kernel) | PSn00bSDK GCC (bootloader) |
|---------|--------------|---------------------------|
| GCC version | 2.91.66 | 12+ |
| Target | mipsel-linux | mipsel-none-elf |
| C standard | C89/C90 | C17/C23 |
| Output | ELF (Linux) | PS-EXE (bare metal) |
| Libraries | uClibc/kernel | PSn00bSDK libpsn00b |
| Use case | Linux kernel | PS1 homebrew apps |

**Important:** The kernel MUST be built with EGCS 2.91.66 (or a compatible era compiler). PSn00bSDK's modern GCC is only for the bootloader and PS1-side tools.

---

*Blackroo Linux Build System Documentation*
