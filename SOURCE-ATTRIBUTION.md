# Source Attribution and Licensing

> Every piece of code in Blackroo Linux has a traceable origin. Nothing is a direct copy
> without attribution. This document exists so that contributors and users can understand
> where each component came from and under what terms it can be used.

---

## Principle

**All code in this project falls into one of three categories:**

1. **Original Linux kernel source** — GPL v2 licensed, from kernel.org
2. **Runix project modifications** — GPL v2 (derived from Linux kernel), original authors credited
3. **New Blackroo work** — GPL v2 (to maintain kernel license compatibility), authored by Chelson Aitcheson and contributors

We do not directly copy code from other projects without:
- Clear attribution of the source
- Verification that the license is compatible (GPL v2 for kernel code)
- Documentation of what was taken and what was modified

---

## Component Origins

### Linux 2.4 Kernel (Base)

| Component | Origin | License | Notes |
|-----------|--------|---------|-------|
| Core kernel (init/, kernel/, mm/, fs/, lib/, ipc/, net/) | Linux 2.4.x kernel.org | GPL v2 | Unmodified stock kernel subsystems |
| MIPS architecture (arch/mips/) | Linux 2.4.x kernel.org | GPL v2 | Base MIPS support, adapted for no-MMU |
| Filesystem support (fs/ext2/, fs/proc/) | Linux 2.4.x kernel.org | GPL v2 | Standard filesystem implementations |
| Block device layer (drivers/block/) | Linux 2.4.x kernel.org | GPL v2 | Standard block device framework |
| Build system (Makefile, scripts/) | Linux 2.4.x kernel.org | GPL v2 | Kernel build infrastructure |

### Runix Project Modifications (Pre-2007)

These files were created or substantially modified by the Runix project team for PlayStation 1 support. The original Runix authors are not fully known — the project was hosted on Google Code and is now archived.

| File | Origin | What It Does | Modifications |
|------|--------|-------------|---------------|
| `arch/mipsnommu/` | Runix project | No-MMU MIPS architecture | Entire directory is Runix work adapting Linux MIPS for uClinux |
| `arch/mipsnommu/ps/setup.c` | Runix | PS1 machine initialization | Sets up I/O port base, identifies CPU |
| `arch/mipsnommu/ps/irq.c` | Runix | PS1 interrupt handler | Maps 11 PS1 hardware IRQs to Linux IRQ subsystem |
| `arch/mipsnommu/ps/time.c` | Runix | PS1 system timer | Configures Timer 2 for kernel jiffies |
| `arch/mipsnommu/ps/siocon.c` | Runix | Serial console driver | SIO1 UART at 115200 baud for kernel console |
| `arch/mipsnommu/ps/reset.c` | Runix | System reset handler | PS1 reset/reboot implementation |
| `arch/mipsnommu/ps/prom/memory.c` | Runix | RAM initialization | Configures RAM_SIZE register (hardcoded 2MB) |
| `arch/mipsnommu/ps/prom/init.c` | Runix | PROM initialization | Boot-time setup |
| `arch/mipsnommu/ps/prom/cmdline.c` | Runix | Kernel command line | Passes boot arguments to kernel |
| `arch/mipsnommu/ps/prom/identify.c` | Runix | CPU identification | Identifies R3000A processor |
| `arch/mipsnommu/ps/kernel/head.S` | Runix | Boot entry point | Assembly: clear BSS, set stack, jump to start_kernel |
| `arch/mipsnommu/ps/kernel/entry.S` | Runix | Exception handlers | Assembly: syscall, interrupt, exception dispatch |
| `arch/mipsnommu/ps/math-emu/` | Runix (from Linux MIPS) | Software FPU | Adapted from standard Linux MIPS FPU emulator |
| `arch/mipsnommu/ps/boot/elf2ecoff.c` | Runix | Format converter | Converts ELF to ECOFF for PS1 boot |
| `arch/mipsnommu/ps/boot/addinitrd.c` | Runix | InitRD embedder | Appends initrd to ECOFF kernel |
| `drivers/block/bu.c` | Runix | Memory card block driver | SIO0 protocol, interrupt-driven, supports RAID |
| `drivers/block/bu.h` | Runix | Memory card definitions | Block sizes, register addresses, data structures |
| `include/asm-mipsnommu/ps/hwregs.h` | Runix | Hardware registers | PS1 I/O register address definitions |
| `include/asm-mipsnommu/ps/interrupts.h` | Runix | IRQ definitions | PS1 interrupt source mapping |
| `include/asm-mipsnommu/ps/sio.h` | Runix | SIO register defs | Serial I/O register addresses and bit definitions |
| `include/asm-mipsnommu/ps/timer.h` | Runix | Timer definitions | Timer register addresses |
| `include/asm-mipsnommu/ps/gpu.h` | Runix | GPU definitions | GPU register addresses |
| `Config` | Runix (modified by Blackroo) | Kernel configuration | Default config for PS1 build |
| `ld.script` | Runix | Linker script | Memory layout for PS1 kernel |

### New Blackroo Work (2024-Present)

These files are original work by Chelson Aitcheson and contributors, or are new implementations not derived from Runix.

| File | Author | What It Does |
|------|--------|-------------|
| `build_simple.sh` | Blackroo | Modern build script with toolchain detection |
| `build.sh` | Blackroo | Comprehensive build script |
| `scripts/make_initrd.sh` | Blackroo | InitRD creation (root method) |
| `scripts/make_initrd_noroot.sh` | Blackroo | InitRD creation (genext2fs method) |
| `scripts/device_table.txt` | Blackroo | Device table for genext2fs |
| `scripts/diagnose.sh` | Blackroo | Build diagnostic tool |
| `tools/elf2psx.c` | Blackroo (new implementation) | ELF to PS-EXE converter |
| `tools/addpsexe_initrd.c` | Blackroo (new implementation) | Add initrd to PS-EXE |
| `readme.md` | Blackroo | Project documentation |
| `roadmap.md` | Blackroo | Technical roadmap |
| `CHANGELOG.md` | Blackroo | Change history |
| `SOURCE-ATTRIBUTION.md` | Blackroo | This file |
| `docs/` | Blackroo | All technical documentation |

### External Tools (Included as Binaries)

| Tool | Source | License | Notes |
|------|--------|---------|-------|
| `tools/busybox-mipsel` | BusyBox project (busybox.net) | GPL v2 | Pre-built MIPSEL binary |
| `tools/mkpsxiso` | Lameguy64 | GPL v2 | PlayStation ISO creator |
| `tools/dumpsxiso` | Lameguy64 | GPL v2 | PlayStation ISO extractor |
| `tools/edcre` | Community tool | Unknown | CD-ROM EDC/ECC tool |
| `tools/elf2ecoff` | Built from Runix source | GPL v2 | Compiled from elf2ecoff.c |
| `tools/addinitrd` | Built from Runix source | GPL v2 | Compiled from addinitrd.c |

### External Projects Referenced (Not Included)

These projects are referenced in documentation but their code is NOT copied into this repository:

| Project | URL | Relationship |
|---------|-----|-------------|
| UniROM 8 | https://github.com/JonathanDotCel/unirom8_bootdisc_and_firmware_for_ps1 | Used as bootloader, not modified |
| NOTPSXSerial (nops) | https://github.com/JonathanDotCel/NOTPSXSerial | Used as upload tool, not modified |
| FreePSXBoot | https://github.com/brad-lin/FreePSXBoot | Memory card exploit, not modified |
| PicoMemcard | https://github.com/dangiu/PicoMemcard | RP2040 card emulator, custom firmware planned |
| PSn00bSDK | http://lameguy64.net/?page=psn00bsdk | PS1 SDK for bootloader, separate build |
| PS2 kernelloader | https://sourceforge.net/p/kernelloader/kernelloader/ci/master/tree/ | Design inspiration only, no code taken |
| PS1 8MB RAM mod (PU-22) | https://github.com/hkzlab/PS1_PU22_8MB_mod | Hardware reference, no code |
| psx-spx specifications | https://psx-spx.consoledev.net/ | Hardware documentation reference |

---

## License

The Linux kernel is licensed under GPL v2. All Runix modifications and new Blackroo code maintain this license. See `blackroo/COPYING` for the full GPL v2 text.

The bootloader (kloader) is GPL v2, for consistency with the kernel. It links
**PSn00bSDK**, whose core libraries are **MPL 2.0** — *not* MIT, as this file
claimed until 2026-08-26. Checked against the SDK's own
`share/psn00bsdk/doc/LICENSE.md`, which states it plainly:

> The PSn00bSDK core (libraries, CMake scripts and some command-line tools) is
> licensed under the MPL 2.0. The license allows you to use the SDK in a
> closed-source project ("larger work") but requires you to share any changes
> you make to the SDK itself.

Blackroo does not modify PSn00bSDK, so nothing has to be published under MPL —
but §3.2 still requires that recipients of a binary that includes MPL-covered
code be told where to get it. `iso/README.TXT` does that, and ships on the disc.

`mkpsxiso` and `dumpsxiso` are a separate project under **GPL v2 or later**.
They build the disc images and are never distributed by this project, so no
obligation attaches to them here.

The **EGCS 2.91.66 cross-toolchain** in `sdk/toolchain/` is GPL. It is a build
tool, not part of this work, and it is deliberately **not** placed on the disc:
shipping GCC binaries would oblige this project to supply GCC's source as well.
PSn00bSDK takes the same position about its own bundled toolchain.

Tools and utilities may have their own licenses as noted above.

---

## How to Verify Attribution

1. **Runix-origin files:** Check git blame or file headers for "PlayStation" or "PSX" references
2. **Stock Linux files:** Compare against Linux 2.4 kernel source from kernel.org
3. **New Blackroo files:** These files did not exist in Runix and have no equivalent in stock Linux 2.4
4. **Every change going forward:** Documented in CHANGELOG.md with source references

---

## Contributing

When adding code to this project:
1. **Never copy code without attribution** — document where it came from
2. **Ensure license compatibility** — GPL v2 for kernel code
3. **Add a CHANGELOG entry** — describe what, why, and where from
4. **If implementing from a spec** — cite the specification (e.g., psx-spx page URL)
5. **If inspired by another project** — note "inspired by [project]" but implement cleanly

---

*Blackroo Linux — Because penguins like to play too*
