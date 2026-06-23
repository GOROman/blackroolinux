# The target consoles — measured, 2026-08-21

Two machines, both profiled over SIO1. **Console #2 has the cheat cart fitted**,
which makes it the one for cart-resident boot work.

Everything here was read off the machine itself over SIO1, with kloader's
serial shell running from a burned CD. No guessing.

```bash
python3 tools/host/blackroo-serial.py /dev/ttyUSB1 dump 0xBFC7FF00 256 /tmp/v.bin
python3 tools/host/blackroo-serial.py /dev/ttyUSB1 peek 0x1F801060
```

---

## Identity

| Property | Value | Source |
|---|---|---|
| BIOS | **System ROM Version 4.1 12/16/97 E** | string at `0xBFC7FF32` |
| BIOS copyright | `Copyright 1993-1997 (C) Sony Computer Entertainment Inc.` | `0xBFC7FF00` region |
| Machine string | `CEX-3000/1001/1002 by K.S.` | `0xBFC00100` region |
| Header date field | `04 12 95 19` (BCD) | `0xBFC00100` |
| Region | **PAL / Europe** — the `E` suffix | BIOS version string |
| Model class | SCPH-750x (PAL). Has the SIO1 serial port | BIOS 4.1 + serial working |

**Why the BIOS version matters:** `docs/18-CART-RESIDENT-BOOT-RESEARCH.md` needs
per-BIOS-version addresses, because at EXP1 pre-boot time the A0/B0/C0 vectors
are not installed yet and Caetla-style stubs hard-code addresses per version.
**4.1 E** is the version to target for this console.

## Configuration

| Register | Value | Meaning |
|---|---|---|
| `RAM_SIZE` `0x1F801060` | `0x0888` | **2 MB — stock, no 8 MB mod** |
| `GPUSTAT` `0x1F801814` | `0x9412220A` | bit 20 set → **PAL video** |
| `I_MASK` `0x1F801074` | `0x000B` | VBlank + GPU + DMA enabled (BIOS/kloader state) |
| `I_STAT` `0x1F801070` | `0x0000` | nothing pending |
| EXP1 `0x1F000000` | all `0xFF` | **no cheat cart fitted** |

Reads of 16-bit registers come back with `0x1F80` in the upper half (open-bus
address echo) — mask to 16 bits.

## What this constrains

1. **2 MB is the target, full stop.** Build with `CONFIG_PSX_2MB_RAM`; the PS-EXE
   stack must be `0x801FFF00` (`build.sh convert` picks this from the config).
   A kernel + 2 MB initrd (2.8 MB) can never load here — it needs the 8 MB mod.
   Current kernel is 742 KB, leaving ~1.2 MB.
2. **PAL is already handled** in the GPU console: `drivers/video/psxcon.c` passes
   `mode = 0x08000009` to `InitGPU`, and GP1(08h) bit 3 = 1 is 50 Hz PAL, with
   bits 0-1 = 01 for 320-wide. No NTSC assumption to fix.
3. **PAL licence data** (`LICENSEE.DAT`) is the correct choice for discs — matches
   the `E` BIOS. An NTSC licence would be rejected.
4. **The IRQs `head.S` now masks are exactly the live ones** — `I_MASK = 0x000B`
   is VBlank/GPU/DMA, which the BIOS leaves running when it hands off from a CD.
5. **The GameShark slot is empty.** Cart-resident boot needs the cart fitted
   before any of `docs/18` can be tested.

## Verified working on this machine

- Modchip: burned CD-Rs boot (with a correct licence area — see docs/19)
- CD boot: kloader runs off disc, menu and controller working
- SIO1 serial: bidirectional at 115200 — `BK>>` beacons, `PONG` answered
- kloader "Boot from CD-ROM": **hangs**, as predicted — kloader and the kernel
  both load at `0x80010000` and the copy loop overwrites itself (docs/19 §3)


---

## Console #2 — same machine, plus a cheat cart

Profiled the same way, over SIO1 from kloader's serial shell.

| Property | Console #1 | Console #2 |
|---|---|---|
| BIOS | System ROM Version 4.1 12/16/97 **E** | **identical** — 4.1 12/16/97 E |
| Region | PAL | PAL (`GPUSTAT 0x1412220A`, bit 20 set) |
| RAM | 2 MB (`0x0888`) | 2 MB (`0x0888`) — **no 8 MB mod on either** |
| EXP1 | empty (`0xFF`) | **POWER REPLAY III (Y2K VER.)**, 256 KB |
| Serial | working | working |

Two identical PAL consoles, so one disc build and one kernel build serve both.
Neither has the RAM mod, which settles the 2 MB question for good.

### The cart — `^_^ POWER REPLAY III (Y2K VER.) ^_^`

An Action Replay clone, 256 KB flash, ~118 KB used. Full backup and details in
`carts/README.md`; the image itself is `carts/powerreplay3-256k-backup.bin`
(md5 in the accompanying `.md5` — **the only way back from a bad flash**).

Its header confirms, on real hardware, what `docs/18-CART-RESIDENT-BOOT-RESEARCH.md`
described from sources:

```
0x1F000080  20 01 00 1f  ->  0x1F000120
0x1F000084  "Licensed by Sony Computer Entertainment Inc."
```

The header appears twice — also at `0x1F000000` pointing to `0x1F0001D0`.

### What this unblocks

`docs/18` was written with no cart to test against. Now there is one:

1. The signature/entry layout is verified rather than assumed.
2. `bootloader/src/pioflash.c` already has erase/program/verify — it can be
   pointed at a real device.
3. 256 KB with ~118 KB used leaves room for kloader (58 KB) without disturbing
   the existing cheat firmware, if the free region is actually erasable.
4. The BIOS version to target is pinned: **4.1 E** on both consoles.

Not yet known: the flash chip type/ID (needs a JEDEC ID read through
`pioflash.c`), and whether the cart write-protects itself.
