# PS2 Microkernel Research — Source Index

All sources referenced in the research document, organized by category.
Every claim in the research is traced back to one of these sources.

---

## PS2 Hardware Documentation

| Source | URL | Covers |
|--------|-----|--------|
| ps2tek | https://psi-rockin.github.io/ps2tek/ | EE, GS, GIF, DMAC, VIF, VU, IPU, IOP, SIF, BIOS |
| ps2tek (israpps fork) | https://israpps.github.io/ps2tek/ | BIOS syscalls, COP0 memory management |
| PS2 Developer Wiki | https://www.psdevwiki.com/ps2/ | Memory map, GS, IOP, vulnerabilities |
| Copetti Architecture Analysis | https://www.copetti.org/writings/consoles/playstation-2/ | High-level architecture overview |
| PS2 Technical Specifications | https://en.wikipedia.org/wiki/PlayStation_2_technical_specifications | Specs summary |
| Emotion Engine Wikipedia | https://en.wikipedia.org/wiki/Emotion_Engine | R5900 core, MMI, die info |
| TX79 Architecture Manual | https://archive.org/details/manualzilla-id-5801237 | R5900 (TX79) instruction set |
| VU Instruction Manual | http://lukasz.dk/files/vu-instruction-manual.pdf | VU0/VU1 microcode |
| PS2 DMAC Basics (Fobes) | https://fobes.dev/ee/2024/02/02/ps2-dmac-basics.html | DMA programming guide |
| PS2 Cache Mapping (Fobes) | https://fobes.dev/ps2/ps2-cache-mapping | Data cache, scratchpad |
| PCSX2 MMU Mini-Series | https://wiki.pcsx2.net/index.php/PCSX2_Documentation/MMU_mini-series | TLB usage, page sizes |
| PCSX2 FPU Analysis | https://pcsx2.net/blog/2006/nightmare-on-floating-point-street/ | Non-IEEE754 FPU behavior |
| PCSX2 Source (Hw.h) | https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Hw.h | Register addresses |
| Writing PS2 BIOS in Rust | https://rust-console.github.io/ps2-bios-book/print.html | Boot process, COP0, EELOAD |
| SPU2 Documentation | https://www.psdevwiki.com/ps2/SPU2 | Sound processor |
| PCSX2 SPU2 Analysis | https://wiki.pcsx2.net/PCSX2_Documentation/SPU2_is_more_than_just_sound! | SPU2 timing uses |
| PS2 RDRAM | https://www.psdevwiki.com/ps2/Rambus_DRAM | Memory specs |
| Network Adaptor Wiki | https://www.psdevwiki.com/ps2/Network_Adaptor | Ethernet hardware |
| SPEED chip | https://www.psdevwiki.com/ps2/SPEED | MAC controller |
| DEV9C | https://www.psdevwiki.com/ps2/DEV9C | Expansion interface |
| IOP/Deckard | https://www.psdevwiki.com/ps2/IOP/Deckard | PPC IOP replacement |
| GS Privileged Registers | https://github.com/ps2dev/ps2sdk/blob/master/common/include/gs_privileged.h | GS register definitions |

## PS2 Development Tools

| Source | URL | Covers |
|--------|-----|--------|
| ps2sdk | https://github.com/ps2dev/ps2sdk | Core SDK, libraries, IOP modules |
| ps2sdk README | https://github.com/ps2dev/ps2sdk/blob/master/README.md | Feature list, license |
| ps2sdk API Docs | https://ps2dev.github.io/ps2sdk/ | API reference |
| ps2sdk Wiki | https://github.com/ps2dev/ps2sdk/wiki | Usage guides |
| ps2toolchain-ee | https://github.com/ps2dev/ps2toolchain-ee | EE GCC 15.2.0, binutils |
| ps2toolchain-iop | https://github.com/ps2dev/ps2toolchain-iop | IOP GCC 15.2.0 |
| ps2toolchain-dvp | https://github.com/ps2dev/ps2toolchain-dvp | VU assembler |
| ps2dev (all-in-one) | https://github.com/ps2dev/ps2dev | Build script, Docker |
| gsKit | https://github.com/ps2dev/gsKit | Graphics library |
| ps2sdk-ports | https://github.com/ps2dev/ps2sdk-ports | SDL, zlib, freetype, etc. |
| ps2link | https://github.com/ps2dev/ps2link | Network ELF loader |
| ps2client | https://github.com/ps2dev/ps2client | Host-side dev tool |
| ps2-packer | https://github.com/ps2dev/ps2-packer | ELF compression |
| ps2sdk crt0 source | https://ps2dev.github.io/ps2sdk/ee_2startup_2src_2crt0_8c_source.html | Startup code |
| NETMAN.txt | https://github.com/ps2dev/ps2sdk/blob/master/NETMAN.txt | Network manager docs |
| loadfile.h | https://github.com/ps2dev/ps2sdk/blob/master/ee/kernel/include/loadfile.h | Module loading API |
| ps2http.c | https://github.com/ps2dev/ps2sdk/blob/master/iop/fs/http/src/ps2http.c | HTTP client |

## PS2 Homebrew Projects

| Source | URL | Covers |
|--------|-----|--------|
| Open PS2 Loader | https://github.com/ps2homebrew/Open-PS2-Loader | Game loader, networking, IOP management |
| wLaunchELF | https://github.com/ps2homebrew/wLaunchELF | File manager, FTP server |
| smbLaunchELF | https://www.psx-place.com/resources/smblaunchelf.1190/ | SMBv2/v3, NFSv3/v4 |
| OSD-Initialization-Libraries | https://github.com/ps2homebrew/OSD-Initialization-Libraries | Proper PS2 init sequence |
| FreeMCBoot Installer | https://israpps.github.io/FreeMcBoot-Installer/test/6_FAQ.html | FMCB FAQ |
| ps2homebrew GitHub org | https://github.com/ps2homebrew | Community projects |
| DOSBox PS2 port | https://www.psx-place.com/threads/dosbox-by-belek666.19030/ | x86 emulation on PS2 |

## PS2 Linux and OS Projects

| Source | URL | Covers |
|--------|-----|--------|
| Linux for PS2 Wikipedia | https://en.wikipedia.org/wiki/Linux_for_PlayStation_2 | Official Sony kit |
| Sony PS2 Linux kernel (2.2.1) | https://github.com/jur/linux-2.2.1-ps2 | Original kernel source |
| Linux 2.4.17 PS2 | https://github.com/rickgaiser/linux-2.4.17-ps2 | Updated kernel |
| Fredrik Noring 120 patches | https://lore.kernel.org/linux-mips/cover.1567326213.git.noring@nocrew.org/T/ | Modern PS2 Linux upstream |
| frno7/linux | https://github.com/frno7/linux | PS2 Linux builds |
| frno7/iopmod | https://github.com/frno7/iopmod | IOP module tools |
| joaco05/linuxPS2 | https://github.com/joaco05/linuxPS2 | Linux 2.2-5.x builds |
| kernelloader | https://github.com/rickgaiser/kernelloader | PS2 Linux bootloader |
| kernelloader SourceForge | https://kernelloader.sourceforge.net/ | Documentation, releases |
| NetBSD/playstation2 | http://www.jp.netbsd.org/ports/playstation2/ | NetBSD port (abandoned) |
| PS2 U-Boot | https://github.com/jur/ps2-u-boot | U-Boot port |
| ps2eth SMAP driver | https://github.com/ps2dev/ps2eth | Ethernet driver (Linux + homebrew) |
| lwIP PS2 fork | https://github.com/ps2dev/lwip/tree/ps2-v2.0.3 | TCP/IP stack |

## Microkernel Theory and MIPS Implementations

| Source | URL | Covers |
|--------|-----|--------|
| L4 Microkernel Family | https://en.wikipedia.org/wiki/L4_microkernel_family | L4 history, IPC design |
| L4/MIPS (UNSW) | http://www.cse.unsw.edu.au/~disy/L4/MIPS/ | MIPS microkernel, 86-cycle IPC |
| Inside L4/MIPS (paper) | https://citeseerx.ist.psu.edu/document?repid=rep1&type=pdf&doi=f42646e5c154108ec851423378aa68e99cbe5b1c | Technical details |
| L4/MIPS SourceForge | https://l4mips.sourceforge.net/ | Source code |
| seL4 Whitepaper | https://sel4.systems/About/seL4-whitepaper.pdf | Formal verification |
| seL4 Supported Platforms | https://docs.sel4.systems/Hardware/ | No MIPS support |
| seL4 Technical Deep Dive | https://www.maxwellseefeld.org/sel4/ | Architecture analysis |
| HelenOS | https://www.helenos.org/ | MIPS32 microkernel OS |
| HelenOS GitHub | https://github.com/HelenOS/helenos | Source code |
| MINIX 3 | https://en.wikipedia.org/wiki/Minix_3 | Self-healing microkernel |
| QNX Architecture | https://www.qnx.com/developers/docs/qnx_4.25_docs/qnx4/sysarch/intro.html | Commercial microkernel |
| L4Re Performance | https://l4re.org/performance.html | IPC benchmarks |
| From L3 to seL4 (paper) | https://flint.cs.yale.edu/cs428/doc/L3toseL4.pdf | Microkernel evolution |
| Mach kernel | https://en.wikipedia.org/wiki/Mach_(kernel) | First-gen microkernel |

## 8086/DOS Emulation

| Source | URL | Covers |
|--------|-----|--------|
| 8086tiny | https://github.com/adriancable/8086tiny | MIT, single-file 8086 emulator |
| 8086tiny docs | https://pushbx.org/ecm/8086tiny/doc.html | Documentation |
| Faux86-remake | https://github.com/ArnoldUK/Faux86-remake | Bare-metal x86 on RPi |
| Faux86 (original) | https://github.com/jhhoward/Faux86 | Bare-metal x86 emulator |
| FreeDOS kernel | https://github.com/FDOS/kernel | GPL DOS kernel |
| FreeDOS boot sequence | https://allthingsopen.org/articles/freedos-boot-sequence | How FreeDOS boots |
| FreeCOM | https://github.com/FDOS/freecom | COMMAND.COM replacement |
| DOSBox PS2 (VOGONS) | https://www.vogons.org/viewtopic.php?t=20120 | PS2 port discussion |
| DOSBox PS2 (ps2dev) | https://forums.ps2dev.org/viewtopic.php?t=9564&start=450 | Dev forum thread |
| SeaBIOS | https://github.com/coreboot/seabios | Open source x86 BIOS |
| BIOS Interrupt Calls | https://en.wikipedia.org/wiki/BIOS_interrupt_call | INT handler reference |

## Legal and Licensing

| Source | URL | Covers |
|--------|-----|--------|
| ps2sdk LICENSE | https://github.com/ps2dev/ps2sdk/blob/master/LICENSE | AFL 2.0 text |
| Sony Trademark Notice | https://www.playstation.com/en-us/legal/copyright-and-trademark-notice/ | Protected marks |
| Sony v. Connectix | https://en.wikipedia.org/wiki/Sony_Computer_Entertainment,_Inc._v._Connectix_Corp. | Fair use ruling |
| Sega v. Accolade | https://en.wikipedia.org/wiki/Sega_v._Accolade | RE fair use |
| Bleem! | https://en.wikipedia.org/wiki/Bleem! | Pyrrhic victory |
| Nominative Use | https://en.wikipedia.org/wiki/Nominative_use | Trademark fair use |
| INTA Fair Use | https://www.inta.org/fact-sheets/fair-use-of-trademarks-intended-for-a-non-legal-audience/ | Trademark guidance |
| MIPS Technologies | https://en.wikipedia.org/wiki/MIPS_Technologies | MIPS trademark |
| MIPS Trademark | https://trademarks.justia.com/788/78/mips-78878135.html | Registration details |
| R5900 LinuxMIPS | https://www.linux-mips.org/wiki/R5900 | "Toshiba, not MIPS" |
| DMCA Section 1201 | https://www.law.cornell.edu/uscode/text/17/1201 | Interoperability exception |
| Clean-room Design | https://en.wikipedia.org/wiki/Clean-room_design | RE methodology |
| Academic Free License | https://en.wikipedia.org/wiki/Academic_Free_License | License analysis |
| GPL FAQ | https://www.gnu.org/licenses/gpl-faq.html | GPL app separation |

## Community and Tutorials

| Source | URL | Covers |
|--------|-----|--------|
| ps2dev forums | https://forums.ps2dev.org/ | Development forum |
| PS2-HOME | https://www.ps2-home.com/forum/ | Homebrew community |
| PSX-Place | https://www.psx-place.com/ | PlayStation modding |
| glampert PS2 Homebrew | https://glampert.com/2015/03-23/ps2-homebrew-hardware-and-ps2dev-sdk/ | Hardware + SDK guide |
| Lukasz PS2 Programming | http://www.lukasz.dk/playstation-2-programming/an-introduction-to-ps2dev/ | Development intro |
| Jacob Harris PS2 Homebrew | https://www.jacobleeharris.dev/introduction/devlog/homebrew/2024/11/23/ps2-homebrew-1.html | 2024 tutorial |
| ConsoleMods Wiki PS2 | https://consolemods.org/wiki/PS2:FMCB | FMCB, MechaPwn |
| Retro Reversing IRX | https://www.retroreversing.com/irx-ps2 | IRX module format |
| Initializing PS2/PSX | https://sites.google.com/view/ysai187/home/projects/initializing-the-ps2psx | Init sequence |
| PS2 Kernel Patches | https://www.ps2-home.com/forum/viewtopic.php?t=7475 | Known BIOS bugs |

---

*Last updated: 2026-04-16*
