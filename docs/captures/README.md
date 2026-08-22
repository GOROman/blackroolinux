# Raw captures

Unedited serial output, kept as evidence for the write-ups. These are what the
machine actually said, not a summary of it. Hardware unless the filename says
`-emulator`.

| File | What it shows |
|---|---|
| `2026-08-21-card-detection-sweep.txt` | Boot-time memory card detection across all 8 minors, with four formatted cards in a multitap. The run that revealed both remaining faults. |
| `2026-08-21-cards-working-381kb.txt` | The same sweep after all four card fixes: three cards joined, 381 Kbytes, `DEBUG` off. |
| `2026-08-21-userspace-shell-emulator.txt` | **Emulator (PCSX-Redux).** `brsh` running as pid 1 on an ext2 initrd root, answering `help` and `echo`. The milestone in docs/25 had no capture until this one. |
| `2026-08-21-userspace-shell-hardware.txt` | **The console.** The same shell as pid 1 on a real PAL SCPH-750x: 912 KB uploaded to kloader at 10.4 KB/s, ext2 root mounted, 381 KB of memory cards, interactive prompt. |
| `2026-08-25-cdrom-first-sector.txt` | **The first CD-ROM sector, and DMA.** Polled/PIO probe in BRMON reading our own disc's ISO9660 volume descriptor - `CD001`, volume id `BLACKROO`. No PS1 Linux port had done this before. Also carries the DMA results, the cache-coherency answer, and the KUSEG addressing mistake that hung the console. |
| `2026-08-21-keyboard-driver.txt` | **Typing on the PlayStation.** The kernel driver finding the adapter behind a multitap, `brsh` driven from the keyboard, and the frame trace that settled whether the protocol is events or state. |
| `2026-08-21-keyboard-scancodes.txt` | **A keyboard on SIO0.** An Apple Bluetooth keyboard through BlueRetro's Lightspan emulation: the `ff 96 5a nn` frame, ACK on every byte, and live Set 2 make/break codes while typing. No driver — BRMON's polled probe. |
| `2026-08-21-tty-layer-gpu-output.txt` | **Userspace on the television.** `/dev/console` resolving to tty 4:1 (the VT) instead of 4:64 (ttyS0), `brsh` writing to it without hanging, and a typing session that appeared on the TV and on serial at once. |

## Reading the detection sweep

Minor numbers map to hardware as `minor = (port << 2) | floor`, so slots 1-4 on
screen are port 1 tap sub-ports A-D, and 5-8 are port 2.

```
slot 1 (floor 0):  127 Kbytes card found                    <- worked
slot 2 (floor 1):  card in slot 2 not found                 <- probe failed
slot 3 (floor 2):  Bad card sequence - found 2 instead 1    <- READ FINE, rejected
slot 4 (floor 3):  card in slot 4 not found                 <- probe failed
slots 5-8:         not found                                <- nothing plugged in
driver initialized: 8 cards joined, total size = 127 Kbytes
```

Two separate faults are visible here, and it took the full sweep to see them:

1. **Alternating probe failures.** Floors 0 and 2 succeed, 1 and 3 fail. All
   four cards had just been formatted and read back through this same tap
   minutes earlier, so the cards are not at fault — the multitap needs a gap
   between back-to-back transactions.

2. **A cascading sequence check.** Slot 3's header was read correctly
   (`number=2`) and then thrown away, because the check compared against a
   running count of cards found so far rather than the card's own number. Slot
   2's failure had left that counter at 1. One flaky probe cost three cards.

Fixes: a 5 ms settle before each probe, and a sequence check that accepts any
card whose number is in range and unclaimed.


## The Heisenbug worth knowing about

After the cascading sequence check was fixed, detection still found cards on
multitap sub-ports A and C but not B — while the monitor's polled probe talked
to every sub-port and had just formatted all of them successfully.

Three theories were tried and were wrong: a settle delay *between* slot probes,
the sequence cascade (real, but not the whole story), and a stuck `/JOYn` left
asserted after a successful transfer.

What found it: enabling the driver's own `#ifdef DEBUG` printks — **and
detection started working**. Nothing else changed. That immediately reframed
the problem from logic to timing: the print latency was supplying a delay the
driver needed and did not have.

The real fix is a 2 ms settle after asserting `/JOYn` in `bu_rd_state0()`,
which is what the monitor's `mc_select()` had been doing all along — the reason
the polled code could reach sub-ports the driver could not. Verified at
381 Kbytes with `DEBUG` back off.

Moral: when a driver only works with its debugging enabled, the bug is timing,
and the debugging is a delay in disguise.
