# BlueRetro adapter — the keyboard hardware

The ESP32 adapter that gives this PlayStation a keyboard, by emulating the
Lightspan PS1 keyboard (SIO0 device ID `0x96`) from any Bluetooth HID keyboard.
Plan and protocol: `docs/27-KEYBOARD-BRINGUP.md`.

## The board

| | |
|---|---|
| Chip | ESP32, 4 MB flash (mfr `5e`, dev `4016`), 3.3 V strapping |
| USB bridge | CH340 (`1a86:7523`) — appears as a `/dev/ttyUSB*` |
| Variant | HW1, external adapter (firmware logs `internal_flag_init: External adapter`) |
| Log baud | **921600**, not 115200 — the ROM banner is 115200, the app then reconfigures |

**The port name is not stable.** This box has several USB serial adapters and
they enumerate in plug order; the PS1's own SIO1 cable has been on both
`ttyUSB0` and `ttyUSB1` on different days. Identify by device: the BlueRetro is
the CH340, the PS1 is whichever port answers a `BK>>` beacon. See GR-009.

## Firmware

Flashed 2026-08-21 with **v25.04 `BlueRetro_hw1_universal.bin`**.

Universal rather than `playstation` deliberately: it auto-detects the console
instead of hardcoding one, so the adapter stays usable on other machines.
Consequence worth knowing — **it has to be connected to a powered console to
pick a system**, where the `playstation` build would simply assume one.

### What was on it before

A custom build: `35958de-dirty`, compiled **Apr 14 2023**, and hardcoded to the
wrong console:

```
# Hardcoded system : 17: PS2
```

That matters because BlueRetro fixes the system at compile time and the
Lightspan keyboard is a **PS1** device — on PS2, keyboards go over USB instead.
The adapter was working perfectly and would still never have answered our
`kbd` probe.

Because it was a `-dirty` build it is not reproducible from any release, so the
whole 4 MB was read back first:

- `blueretro-ps2-35958de-dirty-backup.bin` (+ `.md5`) — verified to contain the
  `BlueRetro` and `35958de-dirty` strings.

Restore it with the same `write-flash` used below, at offset `0x0` with the
single 4 MB image.

### Reflashing

`esptool` needs care on this box: the pipx install at `~/.local/bin/esptool.py`
is **broken** — its venv holds `cpython-312` binaries while the venv's Python is
now 3.14, orphaned by a pyenv upgrade. `pipx reinstall esptool` would fix it;
otherwise build a venv from `~/.pyenv/versions/3.11.10/bin/python`, which has a
working pip.

```bash
esptool --chip esp32 --port /dev/ttyUSB<N> -b 460800 \
  --before default_reset --after hard_reset \
  write-flash --flash-mode dio --flash-size 4MB --flash-freq 40m \
  0x1000  bootloader/bootloader.bin \
  0x8000  partition_table/partition-table.bin \
  0xd000  ota/ota_data_initial.bin \
  0x10000 BlueRetro_hw1_universal.bin
```

Releases: github.com/darthcloud/BlueRetro/releases — v25.04 is the last stable;
the project was archived 2025-12-14. Later updates can go over the air via
blueretro.io/ota.html in Chrome.

### Reading the log

```bash
python3 - <<'EOF'
import serial, time, re
s = serial.Serial('/dev/ttyUSB2', 921600, timeout=0.5)
s.dtr = False; s.rts = True; time.sleep(0.15); s.rts = False   # RTS drives EN
buf = b""; t0 = time.time()
while time.time() - t0 < 20:
    d = s.read(8192)
    if d: buf += d
print(re.sub(r'\x1b\[[0-9;]*m', '', buf.decode('latin1')))
EOF
```

## Pairing a keyboard

Paired 2026-08-21: an **Apple aluminium Wireless Keyboard** (A1314 —
`VID 0x05AC PID 0x0239`), bdaddr `44:2A:60:E9:A8:C3`, name "Chelsons Keyboard".

**The PIN is `0000`, and you type it on the keyboard.** BlueRetro answers
`BT_HCI_EVT_PIN_CODE_REQ` with `bt_default_pin[0]` = `"0000"`
(`main/bluetooth/hci.c:37`), for every device except Wiimotes. Apple's
aluminium keyboards use legacy pairing and have no display, so the PIN has to
be typed **on the keyboard** followed by Return.

Until that happens the log loops every 30 seconds and looks like a failure:

```
BT_HCI_EVT_CONN_COMPLETE
dev: 0 type: 0:0 Chelsons Keyboard
BT_HCI_EVT_LINK_KEY_REQ
BT_HCI_EVT_PIN_CODE_REQ
   ... 30 s ...
BT_HCI_EVT_AUTH_COMPLETE
# dev: 0 error: 0x22        <- LMP response timeout: nobody typed the PIN
BT_HCI_EVT_DISCONN_COMPLETE
```

Success looks like this:

```
BT_HCI_EVT_LINK_KEY_NOTIFY          <- key stored, pairing done
BT_HCI_EVT_ENCRYPT_CHANGE
bt_l2cap_cmd_hid_ctrl_conn_req
bt_l2cap_cmd_hid_intr_conn_req
bt_sdp_parser: VID: 0x05AC PID: 0x0239
bt_sdp_parser HID descriptor size: 224 Usage page: 0501
1 I 07E0 0 8 0700 16 8 0700 24 8 0700 32 8 0700 40 8 0700 48 8 0700 56 8
```

That last line is the standard 8-byte HID boot-keyboard report — modifier byte
then six 8-bit keycodes on usage page `0x07` — so BlueRetro has understood the
descriptor as a keyboard.

The link key is stored, so it reconnects on its own from then on.

**Two things the log will not tell you.** BlueRetro does not print HID input
reports at this level, so typing produces no output here — the PS1 side
(BRMON `kbd watch`) is where keys become visible. And nothing announces the
console until the adapter is wired to a **powered** one, because the universal
build auto-detects rather than assuming.

## Opening the port resets the board

`/dev/ttyUSB2` cannot be opened without resetting the ESP32 — RTS drives EN and
the reset happens on open however DTR/RTS are set beforehand. Start the capture
first, let the boot finish, and only then pair; attaching mid-pairing restarts
the adapter and throws the attempt away.

## Sharing the bus with the memory cards

The adapter sits on **controller port 1**; the multitap and real memory cards
moved to **port 2** (2026-08-21). Two consequences:

- `bu.h` computes `minor = (port << 2) | floor`, so the cards are now minors
  **4-7** and show in BRMON as slots **5-8**. `/dev/bul` is unchanged — it
  aggregates whatever is found — but `card rd <blk> <slot>` and
  `card format <slot>` numbering shifts by four.
- **BlueRetro emulates a memory card as well as a keyboard**, so port 1 answers
  address `0x81` too:

  ```
  # mc_restore: No Memory Card on FS. Creating...
  # mc_store: file updated cnt: 32
  ```

  Harmless as things stand: `bu.c` only claims a card whose block 0 carries the
  Blackroo id `0x1234`, so this one is reported "not found" and skipped. Worth
  knowing so it does not read as a fault in the boot sweep — and worth
  remembering, because it is SD-backed and a `card format` would turn it into
  real storage.

This is the address-byte split from `docs/15` working in practice: `0x01` gets
the keyboard, `0x81` gets a card, on one bus.

## Wiring to the console (from docs/14)

PS1 **controller port 1**: DATA→IO19, CMD→IO32, ATT/CS→I34, CLK→IO33,
ACK→IO21, GND→GND, and pin 3 (8 V) through an LT1117IST-5 LDO for 5 V.

**33 Ω inline resistors and TVS diodes (5VWM 9.2VC DO214AA) on the signal
lines are not optional** — without them the PS1 sees random phantom button
presses. The BlueRetro wiki calls this out specifically for PS1.

If Player 2 is unused, tie IO5/IO26/IO27 to 3.3 V rather than leaving them
floating.
