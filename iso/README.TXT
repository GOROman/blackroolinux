BLACKROO LINUX 0.5.0 "Rootstock26"
Linux 2.4 (uClinux, no-MMU) for the Sony PlayStation, MIPS R3000A, 2 MB.

================================================================
 YOUR RIGHTS, AND THE SOURCE CODE
================================================================

This disc carries FREE SOFTWARE. You may use, study, modify and
redistribute it under the GNU General Public License, version 2.

The complete corresponding source code for every program on this
disc is ON THIS DISC, in the file:

    SOURCE.TGZ

That is not a courtesy copy or a snapshot. It is the exact source
these binaries were built from, and it includes the build scripts
needed to reproduce them. Extract it anywhere and run:

    ./bootstrap.sh

The full licence text is on this disc as COPYING.TXT.

GPL v2 section 3 lets a distributor either ship the source or
promise it later. Promises are harder to keep than facts, and this
disc had 675 MB spare, so it ships the source.

----------------------------------------------------------------
 WHAT IS ON THE DISC
----------------------------------------------------------------

  SYSTEM.CNF      what the PlayStation BIOS reads first
  BLACKROO.EXE    "kloader", the boot menu           GPL v2
  KERNEL.EXE      Linux + a ramdisk built in         GPL v2
  LINUX.EXE       Linux alone, honours root=         GPL v2
  ROOT.IMG        the ext2 root filesystem           GPL v2
  SOURCE.TGZ      the corresponding source           GPL v2
  COPYING.TXT     the GNU GPL version 2
  README.TXT      this file

----------------------------------------------------------------
 COMPONENTS AND THEIR LICENCES
----------------------------------------------------------------

Linux kernel 2.4 (uClinux/no-MMU), with PlayStation support
  GPL v2. Source in SOURCE.TGZ under blackroo/.
  Includes work from the abandoned Runix/PSXLinux project (~2007)
  and new drivers written for Blackroo: the CD-ROM block device,
  the memory-card block device, the GPU console, the SIO0
  keyboard, and the fixed-address ELF loader.

kloader (BLACKROO.EXE) and brsh (the shell inside ROOT.IMG)
  GPL v2. Written for this project. Source in SOURCE.TGZ under
  bootloader/ and userland/.

PSn00bSDK
  kloader links the PSn00bSDK runtime libraries, which are
  MOZILLA PUBLIC LICENSE 2.0, not GPL. Blackroo does not modify
  them, so MPL 2.0 section 3.2 is satisfied by telling you where
  they are, which is here:

      https://github.com/Lameguy64/PSn00bSDK

  Their licence text ships with the SDK at
  share/psn00bsdk/doc/LICENSE.md.

The cross-compiler
  Blackroo is built with EGCS 2.91.66 (a 1999 GCC), which is GPL.
  It is a TOOL, not part of this work, and it is deliberately not
  on this disc - shipping GCC binaries would oblige this project
  to supply GCC's source too. SOURCE.TGZ explains how to obtain a
  toolchain; sdk/README.md has the details.

mkpsxiso
  Used to author this disc. GPL v2 or later, a separate project,
  not distributed here. https://github.com/Lameguy64/mkpsxiso

The PlayStation BIOS is NOT on this disc and is not part of this
project. It stays in your console, where Sony put it.

----------------------------------------------------------------
 NO WARRANTY
----------------------------------------------------------------

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See section
11 of COPYING.TXT.

It writes to memory cards when you ask it to. It has no memory
protection of any kind. Keep backups of anything you care about.

----------------------------------------------------------------
 BOOTING IT
----------------------------------------------------------------

A burned CD-R needs a modchip, or a soft exploit such as tonyhax
or FreePSXBoot. That is a property of the console, not this disc.

From kloader's menu:

  Root filesystem on the DISC (self-contained):
      Settings -> Root: /dev/psxcd (this disc)
      Boot from CD-ROM -> LINUX.EXE - uses Root setting

  Root filesystem in RAM (no disc read after boot):
      Settings -> Root: /dev/ram0 (ramdisk)
      Boot from CD-ROM -> KERNEL.EXE - built-in ramdisk

KERNEL.EXE ignores the Root setting: its ramdisk is compiled in
and takes over. Only LINUX.EXE honours root=.

To tell which one you got:  cat /etc/release

----------------------------------------------------------------
 https://github.com/  - see SOURCE.TGZ for full attribution in
 SOURCE-ATTRIBUTION.md, and CHANGELOG.md for how this was built.
