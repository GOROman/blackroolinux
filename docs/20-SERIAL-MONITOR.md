# BRMON — the in-kernel serial monitor

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: BINFMT_FLAT.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> Added 2026-08-20. Source: `blackroo/arch/mipsnommu/ps/brmon.c`.
> Built when `CONFIG_BLACKROO_MONITOR=y` (on in all three defconfigs).

---

## Why it exists

The project has had a booting kernel since April but never an interactive
prompt, and the reason is a binary-format dead end:

- `CONFIG_BINFMT_FLAT=y`, and **no ELF loader is compiled** — `fs/binfmt_elf.o`
  is not built in this tree.
- `initrd/skeleton/bin/busybox` is a 1.5 MB statically linked **ELF**.
- So `execve("/bin/sh")` can never succeed, no matter how healthy the initrd
  is. The kernel reaches `init()`, fails every exec, and panics.

BRMON sidesteps the whole problem: it is an interactive shell that runs **in
kernel context**, talking to SIO1 directly. No userspace, no initrd, no root
filesystem — which also means a kernel-only PS-EXE (742 KB) is the entire
system, and that fits a stock 2 MB console with room to spare.

It is not a replacement for a real userspace. It is the tool for the job the
project actually needs next: reading and writing hardware registers on a live
machine while writing drivers for it.

---

## Getting to the prompt

The kernel's built-in command line (`arch/mipsnommu/ps/prom/cmdline.c`)
starts with `brmon`, and `init/main.c` checks for it **after `do_initcalls()`
but before `mount_root()`** — every driver is initialised, and no root
filesystem is required:

```
    do_initcalls();
    ...
    if (strstr(saved_command_line, "brmon"))
            brmon_main();       /* <- here */
    mount_root();
```

There is a second entry point: if an initrd *is* present but every `execve`
fails, `init()` drops into the monitor instead of `panic("No init found")`.

Host side:

```bash
python3 tools/host/blackroo-serial.py /dev/ttyUSB0 console      # Ctrl+] to exit
```

115200 8N1. The host **must** assert RTS and DTR (this tool does) or the PS1
will not transmit — see `docs/13-SIO1-HARDWARE-RESEARCH.md`. BRMON keeps its
own RTS asserted for the same reason, and re-programs MODE/CTRL itself on
entry rather than trusting the console driver's state.

---

## Commands

```
  md <addr> [words]        dump memory, 4 words + ASCII per line
  peek <addr> [b|h|w]      read one value
  poke <addr> <val> [b|h|w]  write one value
  fill <addr> <words> <val>  fill a range
  ram                      RAM_SIZE register + non-destructive mirror probe
  hw                       GPUSTAT, I_STAT, I_MASK, DPCR, JOY_STAT, SIO regs
  cpu                      COP0 PRId / SR / Cause / EPC, decoded
  mem                      kernel _ftext.._end, page counts, initrd, jiffies
  reboot                   jump to the BIOS reset vector (0xbfc00000)
  cont                     leave the monitor and carry on booting
```

`md` remembers where it stopped, so repeated `md` walks forward.

### Addresses

Type the bare address you would read in psx-spx; BRMON picks the right
segment:

| What you type | What it reads | Why |
|---|---|---|
| `1f801814` | `0xbf801814` (KSEG1) | hardware registers must be **uncached** |
| `10000` | `0x80010000` (KSEG0) | bare physical RAM, cached |
| `80010000` | as given | already a kernel segment |

So `md 1f801810 4` dumps the GPU registers and `md 80010000` dumps the start
of the kernel itself.

### Worked examples

```
blackroo> ram
RAM_SIZE (0x1f801060) = 00000888  [2 MB setting]
mirror probe          : 2 MB (mirrors at 2 MB)
kernel believes       : 2048 KB (512 pages)

blackroo> peek 1f801814
bf801814: 14802000

blackroo> poke 1f801070 0 h        # clear I_STAT
blackroo> md 80010000 8            # first 8 words of the kernel
```

---

## Verified in emulation, 2026-08-20

**DuckStation** boots the disc and shows the whole kernel boot on the GPU
console — `Memory: 1076k/2048k available`, `RAMDISK driver initialized`,
`PSX joined card: 2 cards joined, total size = 254 Kbytes`, then it stops at
`PSX serial port driver` with a blinking cursor. That stop *is* BRMON: this
DuckStation build has no SIO1 host bridge, so the monitor sits in `mon_getc()`
waiting for a byte that can never arrive. Use DuckStation to check the boot,
not the monitor.

**PCSX-Redux** has an SIO1 TCP server, and that is the way to drive BRMON
without hardware:

These live in the **`emulator.Debug` object** of
`~/.config/pcsx-redux/pcsx.json` — nested one level down, next to
`GdbServer`/`GdbServerPort`. Putting them directly in `emulator` looks
plausible and does nothing: Redux ignores them and silently drops the keys when
it rewrites the file on exit, so the TCP port never opens.

| `pcsx.json` key | Value |
|---|---|
| `emulator.Debug.SIO1Server` | `true` |
| `emulator.Debug.SIO1ServerPort` | `6699` |
| `emulator.Debug.SIO1Mode` | **`1`** — Raw |

Confirm it took: `ss -ltn | grep 6699` must show a listener once Redux is up.

`SIO1Mode: 0` is Protobuf: output still arrives, but every byte is wrapped
(`\n\x03\n\x01<char>\x05`) and **nothing you type reaches the monitor**. That
one setting was the whole difference between "the prompt appears but is dead"
and a working session. The BIOS is irrelevant — verified identical on
SCPH1001 (US) and SCPH5502 (PAL).

```bash
pcsx-redux -loadexe output/blackroo.exe -run    # PS-EXE directly, no disc build
pcsx-redux -iso output/blackroo.cue -run
python3 tools/host/redux-sio1.py                 # interactive, Ctrl+] to exit
python3 tools/host/redux-sio1.py -c ram -c cpu   # scripted
```

Real session output:

```
blackroo> cpu
PRId  = 00000002  (rev 0.2)
SR    = 10000401  IEc=1 KUc=0 IsC=0 BEV=0
Cause = 00000400  ExcCode=0
EPC   = 00071470

blackroo> hw
GPUSTAT  0x1f801814 = 5416200b        (d416200b under the PAL BIOS)
I_STAT   0x1f801070 = 0001
I_MASK   0x1f801074 = 01c0
DPCR     0x1f8010f0 = 00009899
JOY_STAT 0x1f801044 = 0207
SIO_STAT 0x1f801054 = 0185  MODE 004e  CTRL 0027  RATE 0012

blackroo> ram
RAM_SIZE (0x1f801060) = 00000888  [2 MB setting]
mirror probe          : 2 MB or less (4 MB write wrapped to 0)
kernel believes       : 2048 KB (512 pages)

blackroo> mem
kernel text starts   00010000
kernel _end          000e8a20
physical pages       512 (2048 KB)
free pages           233
initrd               00000000..00000000
jiffies              8763
```

`MODE 004e` and `CTRL 0027` are exactly the values `docs/13` says real silicon
requires (16x baud multiplier, RTS asserted) — the monitor is reporting its own
correctly-configured link back to us.

Two cosmetic things this session exposed, not yet fixed:
- `mem` prints `_ftext`/`_end` as bare physical addresses (`00010000`), because
  the no-MMU linker script uses physical symbols. Add the KSEG0 bit for display.
- `md 80010000 4` reads as zeroes: the first words of the loaded image are
  padding, not code. Worth confirming against `System.map` rather than assuming.

---

## Cautions

- **The monitor owns the CPU.** `mon_getc()` spins on the SIO1 status
  register with interrupts still enabled but nothing else scheduled. Timer
  ticks continue; nothing else progresses.
- **`poke` has no guard rails.** Writing an unaligned or unmapped address
  raises a CPU exception and the kernel will panic — that is the intended
  behaviour of a monitor, but it means a bad `poke` ends the session.
- **`fill` and `poke` can overwrite the kernel.** `md 80010000` is fine;
  `fill 80010000 ...` is not.
- **`ram` writes and restores** three words at 0, 2 MB and 4 MB through
  KSEG1. It restores the originals, but it is still a write to live memory.

---

## What's next for it

1. **Memory card commands** — `card probe`, `card rd <slot> <block>`,
   `card wr`, and a `raid` view over `CONFIG_PSX_LARGE_CARD`, driving
   `drivers/block/bu.c` through the block layer now that `do_initcalls()`
   has run before we get control.
2. **Exception trap** — catch the address-error/bus-error exception during
   `peek`/`poke` and print "fault at ..." instead of panicking.
3. **Upload/download over the same link** so a driver under test can be
   patched in without a reboot.

---

## Sources

- psx-spx, Serial Interfaces (SIO1 MODE/CTRL/STAT semantics, CTS/RTS) —
  <https://psx-spx.consoledev.net/serialinterfacessio/>
- psx-spx, Memory Control (`RAM_SIZE` 0x1f801060) —
  <https://psx-spx.consoledev.net/memorycontrol/>
- This tree: `arch/mipsnommu/ps/siocon.c` (register usage), `init/main.c`
  (boot sequence), `docs/13-SIO1-HARDWARE-RESEARCH.md` (hardware findings)
