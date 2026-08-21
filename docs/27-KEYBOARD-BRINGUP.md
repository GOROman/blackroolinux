# Keyboard bring-up — BlueRetro + Lightspan emulation

> Started 2026-08-21, straight after the tty layer began drawing userspace on
> the TV. Output is done; this is the input half, and the last thing standing
> between the console and not needing a host PC.

## The decision, and why

Three routes were on the table: a PS/2 socket wired to the Pico, USB host on
the Pico via PIO-USB, or **BlueRetro** — an ESP32 adapter that already
implements PS1 Lightspan keyboard emulation and takes input from any Bluetooth
HID keyboard (`docs/14` Finding 4; added in BlueRetro v0.9.1, December 2020).

All three converge on the *same* kernel work: an SIO0 driver that polls address
`0x01`, expects `96 5A nn`, and feeds PS/2 Scan Code Set 2 bytes to
`handle_scancode()`. What differs is whether we must also write and debug an
RP2040 keyboard emulator first.

BlueRetro removes that half — but the reason to pick it is better than saving
time. **It makes the kernel driver the only variable.** Debugging a new driver
against new firmware means two unknowns and no way to bisect; this project has
already lost days to exactly that shape of problem (see GR-010, and the memory
card driver that only worked with `DEBUG` printks on). A known-good device on
the other end means a failure is *ours*.

If the Pico later takes over both jobs — storage and keyboard on one device,
which `docs/15` shows is possible because SIO0 selects by address byte — it
becomes a firmware exercise against a kernel side that is already proven.

## The protocol

```
TX:  01 42 00 00 00 00 00 00 00 00 00 00 00 00 06
RX: HiZ 96 5A nn  d  d  d  d  d  d  d  d  d  d  d
```

- `0x01` — controller address class. **Not** `0x81`, which is the memory card
  class. This is why a keyboard and the cards coexist on one bus.
- `0x96` — keyboard device id. A pad answers `0x41`, a card `5A 5D`.
- `0x5A` — ready marker.
- `nn` — count of valid scancode bytes, `0x00`–`0x0B`; `0xFF` means the adapter
  is there but no keyboard is attached to it. That distinction is worth having:
  it separates "wiring wrong" from "keyboard not paired".
- `d` — raw **PS/2 Scan Code Set 2**, up to 11 bytes per frame.

The adapter is a pass-through, not a translator: a PS/2 keyboard already emits
Set 2. Eleven bytes per frame exist because a fast typist produces several
make/break codes between two polls.

Set 2 encoding: make code is one byte (`0xE0` prefix for extended keys), break
code is `0xF0` + make (`0xE0 0xF0` + byte when extended).

## Order of work

### 1. Hardware — the adapter

**Done 2026-08-21** — flashed with v25.04 `BlueRetro_hw1_universal.bin`. Full
detail, including the backup of what was on it and the two tooling traps, is in
`hardware/blueretro/README.md`. In short:

- The adapter arrived carrying a custom Apr-2023 build **hardcoded to PS2**,
  which could never have answered a Lightspan probe — BlueRetro fixes the
  console at compile time and `0x96` is a PS1 device. A healthy adapter that
  looks exactly like bad wiring.
- `universal` was chosen over `playstation` so the board stays usable on other
  consoles; it auto-detects, so it must be attached to a powered console to
  settle on a system.
- The BlueRetro log is at **921600** baud, not 115200.
- The pipx `esptool` on this box is broken (Python 3.14 venv over
  `cpython-312` binaries); use `~/.pyenv/versions/3.11.10/bin/python` to build
  a working one.
- Wiring to PS1 **controller port 1**: DATA→IO19, CMD→IO32, ATT/CS→I34,
  CLK→IO33, ACK→IO21, GND→GND, 8V (pin 3) through an LT1117IST-5 LDO for 5V.
- **33 Ω inline resistors + TVS diodes (5VWM 9.2VC DO214AA) on the signal
  lines.** Not optional: without them the PS1 gets random phantom button
  presses, and the BlueRetro wiki calls this out specifically for PS1.
- If Player 2 is not wired, tie IO5/IO26/IO27 to 3.3 V rather than leaving them
  floating.

Then pair a Bluetooth keyboard to it.

### 2. Prove it in BRMON — before any driver

`kbd` in the monitor, added 2026-08-21:

```
blackroo> kbd            one poll of slot 0, raw frame plus a decode
blackroo> kbd watch      poll continuously and print keys as you type
blackroo> kbd watch 1    the same on port 2
```

`kbd watch` runs until you press a key on the *serial* side. It prints
`[down 1c] 'a'` / `[up 1c]` as keys are pressed and released.

This step exists because the monitor is the only place on this machine where
SIO0 has no other users: no interrupts, no `bu.c`, no scheduler. If the bytes
appear here, the wiring and the adapter are right and everything after is
software. If they do not, we have learned it without having written a driver.

What the failure modes look like:

| What you see | What it means |
|---|---|
| `no 0x96 in the reply` | Nothing is answering address `01` on that port. Wrong port, wrong wiring, or the adapter is not running. |
| `adapter present, but it reports no keyboard attached (nn=FF)` | The adapter is talking. The Bluetooth keyboard is not paired or not awake. |
| `[down 1c] 'a'` | Done. Move to step 3. |
| Phantom keys with nothing pressed | The missing 33 Ω resistors / TVS diodes. |

Note the decode anchors on finding `0x96` anywhere in the buffer rather than at
a fixed offset, because SIO0 replies lag one transfer behind the sends. That
cost a day on the memory cards.

### 3. The kernel driver — where the real work is

`arch/mipsnommu/ps/kbd-no.c` is what the tree builds today; `psx_kbd.c` exists
but its `kbd_init_hw()` is an empty stub. The driver needs to:

- poll SIO0 at address `0x01` on a timer, roughly at the controller rate
- translate Set 2 make/break into what `handle_scancode()` expects, and hand it
  to the VT — which is already wired to the GPU, so a keypress becomes a
  character on the television with no further work
- **arbitrate with `bu.c` for the bus.** This is the risk. `bu.c` owns SIO0,
  drives it from the ACK interrupt, and has its own locking (`bu_lock`,
  `bu_catch()`). Two masters on one bus, one of them interrupt-driven, on
  hardware whose timing has already produced four separate card bugs — budget
  for this, and expect it to be the part that takes the time.

Once input reaches `/dev/console`, `userland/brsh.c` moves its `infd` off
`/dev/brcon` (there is a comment there marking the spot) and the PlayStation
stops needing a host PC.

## Status

- [x] BRMON `kbd` / `kbd watch` command
- [x] Adapter flashed — v25.04 hw1 universal, 2026-08-21 (`hardware/blueretro/`)
- [x] Bluetooth keyboard paired — Apple A1314, PIN `0000` typed on the keyboard
- [x] Adapter on controller port 1; cards + multitap moved to port 2 (`/dev/bul` still 381 KB)
- [x] `out_cfg[0].dev_mode` set to `DEV_KB` — two bytes patched into the ESP32 storage partition
- [x] **Scancodes visible in `kbd watch`** — `docs/captures/2026-08-21-keyboard-scancodes.txt`
- [x] **Kernel driver + SIO0 arbitration** — `drivers/char/psxkbd.c`, 2026-08-21
- [x] **`brsh` reading `/dev/console`** — the host PC is out of the input path
