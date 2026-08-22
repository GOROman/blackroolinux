# Blackroo Linux - PicoMemcard Dual-Mode Design

> **Historical.** Kept as a record of what was known at the time.
> Superseded in part: UniROM as the upload path.
> See [00-PROJECT-STATUS.md](00-PROJECT-STATUS.md) for what is true now.


> Using a single RP2040 device as both memory card storage AND serial console,
> all through one memory card slot. No SIO1, no separate serial cable, no FTDI.

---

## Concept

The PicoMemcard sits in a PS1 memory card slot and already talks to both sides:
- **PS1 side:** SIO0 bus (memory card protocol, ~250kHz)
- **PC side:** USB (for card image management)

We extend this so the same device also carries serial console traffic. The kernel
talks to the PicoMemcard over SIO0 using custom commands, and the PicoMemcard
forwards console I/O to the host PC over USB CDC (virtual serial port).

```
┌──────────────┐    SIO0 (card slot)    ┌───────────────┐     USB      ┌─────────┐
│ PlayStation 1 │◄──────────────────────►│  PicoMemcard  │◄────────────►│ Host PC │
│               │  memory card protocol  │  (RP2040)     │  CDC serial  │         │
│ Linux kernel  │  + custom serial cmds  │               │  + MSC disk  │ screen  │
└──────────────┘                         └───────────────┘              └─────────┘

No SIO1 port used. No serial cable. No FTDI. One device does everything.
```

---

## How It Works

### Standard Memory Card Commands (Existing)

These already work in the PicoMemcard firmware:

| Command | Byte | Purpose |
|---------|------|---------|
| Read | 0x52 | Read 128-byte block from virtual card |
| Write | 0x57 | Write 128-byte block to virtual card |
| ID | 0x53 | Get card info (size, block count) |

### New Serial Console Commands (Custom)

We add new commands that the PicoMemcard recognises for console I/O:

| Command | Byte | Purpose |
|---------|------|---------|
| Serial Write | 0x58 ('X') | PS1 sends bytes → PicoMemcard → USB CDC → PC |
| Serial Read | 0x59 ('Y') | PC → USB CDC → PicoMemcard → PS1 receives bytes |
| Serial Status | 0x5A ('Z') | Check if bytes are waiting from PC side |

### Protocol: Serial Write (PS1 → PC)

```
TX (PS1 sends):     81  58  00  00  [LEN]  [DATA x LEN]  [CHK]  00
RX (card returns):  FF  FF  5A  5D   --    [00 x LEN]     --    47

Where:
  81   = Memory card address (standard)
  58   = Serial write command (new)
  LEN  = Number of bytes to send (1-64)
  DATA = Console output bytes
  CHK  = XOR checksum
  47   = Good acknowledgement
```

The PicoMemcard receives the DATA bytes and writes them to USB CDC. The host PC's terminal (screen, minicom) displays them.

### Protocol: Serial Read (PC → PS1)

```
TX (PS1 sends):     81  59  00  00  [MAX]  [00 x MAX]  00  00
RX (card returns):  FF  FF  5A  5D  [LEN]  [DATA x LEN] [CHK]  47

Where:
  MAX  = Maximum bytes PS1 is willing to receive
  LEN  = Actual bytes available from USB CDC buffer
  DATA = Keyboard/terminal input from PC
```

The PicoMemcard pulls bytes from its USB CDC receive buffer (typed by user on PC) and sends them to the PS1 kernel.

### Protocol: Serial Status (Check for Input)

```
TX (PS1 sends):     81  5A  00  00  00
RX (card returns):  FF  FF  5A  5D  [COUNT]

Where:
  COUNT = Number of bytes waiting in USB CDC buffer (0 = nothing)
```

This lets the kernel poll without blocking — check if the user typed anything, and only do a Serial Read if data is waiting.

---

## Kernel Side: SIO0 Console Driver

A new kernel driver replaces `siocon.c` (which uses SIO1). This one talks to the
PicoMemcard via SIO0 using the custom serial commands.

### Driver Outline

```c
/*
 * picocon.c — Console over PicoMemcard (SIO0 memory card bus)
 *
 * Instead of using the PS1's SIO1 UART, this driver sends console
 * output through the memory card slot to a PicoMemcard RP2040,
 * which bridges it to USB CDC on the host PC.
 *
 * Attribution: New Blackroo work. Protocol designed for this project.
 * Uses SIO0 register access patterns from bu.c (Runix memory card driver).
 */

#include <linux/console.h>
#include <linux/tty.h>
#include <linux/init.h>
#include <asm/io.h>

/* SIO0 registers (same as memory card driver) */
#define SIO0_DATA    0x1F801040
#define SIO0_STAT    0x1F801044
#define SIO0_MODE    0x1F801048
#define SIO0_CTRL    0x1F80104A
#define SIO0_BAUD    0x1F80104E

/* Custom commands */
#define PICOMC_CMD_SERIAL_TX   0x58  /* Send bytes to PC */
#define PICOMC_CMD_SERIAL_RX   0x59  /* Receive bytes from PC */
#define PICOMC_CMD_SERIAL_STAT 0x5A  /* Check for pending input */

/* Maximum bytes per SIO0 transaction */
#define PICOMC_MAX_SERIAL_BYTES 64

/*
 * Send a buffer of console output through SIO0 to PicoMemcard.
 * The PicoMemcard forwards it over USB CDC to the host PC.
 */
static void picomc_console_write(struct console *co,
                                  const char *buf, unsigned int len)
{
    int i, chunk;

    while (len > 0) {
        chunk = (len > PICOMC_MAX_SERIAL_BYTES)
                ? PICOMC_MAX_SERIAL_BYTES : len;

        /* Select memory card slot (same as bu.c bu_hw_init) */
        sio0_hw_init();

        /* Send address byte (0x81 = memory card) */
        sio0_send_byte(0x81);

        /* Send custom serial write command */
        sio0_send_byte(PICOMC_CMD_SERIAL_TX);

        /* Wait for card ID response (0x5A, 0x5D) */
        sio0_recv_byte();  /* 0x5A */
        sio0_recv_byte();  /* 0x5D */

        /* Send length */
        sio0_send_byte(chunk);

        /* Send data bytes */
        for (i = 0; i < chunk; i++) {
            sio0_send_byte(buf[i]);
        }

        /* Send checksum, receive ACK */
        sio0_send_byte(0x00);  /* checksum placeholder */
        sio0_recv_byte();      /* 0x47 = Good */

        buf += chunk;
        len -= chunk;
    }
}

/*
 * Check if input is waiting, and read it.
 */
static int picomc_console_getchar(void)
{
    uint8_t count, ch;

    /* Status check */
    sio0_hw_init();
    sio0_send_byte(0x81);
    sio0_send_byte(PICOMC_CMD_SERIAL_STAT);
    sio0_recv_byte();  /* 0x5A */
    sio0_recv_byte();  /* 0x5D */
    count = sio0_recv_byte();

    if (count == 0)
        return -1;  /* Nothing waiting */

    /* Read one byte */
    sio0_hw_init();
    sio0_send_byte(0x81);
    sio0_send_byte(PICOMC_CMD_SERIAL_RX);
    sio0_recv_byte();  /* 0x5A */
    sio0_recv_byte();  /* 0x5D */
    sio0_send_byte(1); /* request 1 byte */
    ch = sio0_recv_byte();
    sio0_recv_byte();  /* checksum */
    sio0_recv_byte();  /* 0x47 */

    return ch;
}

static struct console picocons = {
    name:    "pico",
    write:   picomc_console_write,
    device:  picomc_console_device,
    setup:   picomc_console_setup,
    flags:   CON_PRINTBUFFER,
    index:   -1,
};

void __init picomc_console_init(void)
{
    printk("Blackroo: PicoMemcard console enabled (SIO0)\n");
    register_console(&picocons);
}
```

### Kernel Config Option

```
# PlayStation Console Device
CONFIG_SERIAL_PSX=y              # Standard SIO1 serial (keep as fallback)
CONFIG_SERIAL_PSX_CONSOLE=y
CONFIG_PICOMC_CONSOLE=y          # NEW: PicoMemcard console over SIO0
```

### Coexistence with Memory Card Driver

The serial console and memory card driver both use SIO0. They must time-share:

```
SIO0 bus arbitration:
  1. Console output is buffered in kernel
  2. When SIO0 is free (no memory card I/O in progress), flush console buffer
  3. Memory card I/O takes priority (block device requests)
  4. Console output sent in gaps between card operations
  5. Use the existing bu_lock mechanism to prevent collisions

In practice:
  - Boot messages: continuous console output, no card I/O yet → fast
  - Shell session: occasional console I/O, occasional card I/O → fine
  - Heavy card I/O: console output delayed slightly → acceptable
```

### Addressing: Which Card Slot?

The PicoMemcard must be in a known slot. Options:

| Approach | Details |
|----------|---------|
| **Dedicated slot** | PicoMemcard always in slot 1 (port 1, address 0x81). Real cards in other slots. |
| **Any slot, auto-detect** | Kernel probes all slots for PicoMemcard by sending serial status command (0x5A). Whichever responds is the console device. |
| **Configurable** | Kernel boot parameter: `picomc=0` for slot 1, `picomc=1` for slot 2, etc. |

**Recommendation:** Auto-detect. Send the custom 0x5A command to each slot during init. A real memory card will return an error (unknown command). The PicoMemcard will respond with a byte count. Whichever slot responds is the console.

---

## PicoMemcard Firmware Side

### Changes to Existing Firmware

The PicoMemcard firmware (`memcard_simulator.c`) handles SIO0 commands in a state machine. We add cases for the new commands:

```c
/* In the command handler, after existing Read/Write/ID cases */

case PICOMC_CMD_SERIAL_TX: {
    /* PS1 is sending console output bytes */
    uint8_t len = recv_byte();  /* Length */
    for (int i = 0; i < len; i++) {
        uint8_t ch = recv_byte();
        /* Push to USB CDC TX buffer */
        if (tud_cdc_connected()) {
            tud_cdc_write_char(ch);
        }
    }
    tud_cdc_write_flush();
    recv_byte();  /* checksum (ignore for now) */
    send_byte(0x47);  /* Good */
    break;
}

case PICOMC_CMD_SERIAL_RX: {
    /* PS1 wants to read input from PC */
    uint8_t max = recv_byte();  /* Max bytes requested */
    uint8_t avail = tud_cdc_available();
    uint8_t count = (avail < max) ? avail : max;
    send_byte(count);
    for (int i = 0; i < count; i++) {
        uint8_t ch;
        tud_cdc_read(&ch, 1);
        send_byte(ch);
    }
    send_byte(0x00);  /* checksum placeholder */
    send_byte(0x47);  /* Good */
    break;
}

case PICOMC_CMD_SERIAL_STAT: {
    /* PS1 checking if input is waiting */
    uint8_t avail = tud_cdc_available();
    send_byte(avail > 255 ? 255 : avail);
    break;
}
```

### USB CDC Setup

The PicoMemcard already uses TinyUSB. We add CDC alongside the existing MSC:

```c
/* USB device presents:
   - CDC ACM: /dev/ttyACM0 on host — serial console
   - MSC:     /dev/sdX on host     — SD card access for card images
*/
```

Both interfaces work simultaneously. The user opens a terminal on `/dev/ttyACM0` and sees Linux console output. They type commands and the keystrokes flow back through USB → RP2040 → SIO0 → kernel.

---

## Performance

### Console Throughput via SIO0

```
SIO0 clock: ~250 kHz
Bits per byte: ~10 (8 data + overhead)
Raw byte rate: ~25 KB/s

Per console transaction (64 bytes):
  Protocol overhead: ~8 bytes (address, command, ID, ACK)
  Data: 64 bytes
  Total: ~72 bytes
  Time: ~2.9ms per 64-byte chunk

Effective console throughput: ~22 KB/s
  = ~22,000 characters per second
  = WAY more than enough for a text console
```

For comparison, a standard serial console at 115200 baud is ~11.5 KB/s. **The SIO0 console is actually faster than a standard serial connection.**

### Latency

```
PS1 → PicoMemcard (SIO0): ~0.1ms per byte
PicoMemcard → USB CDC:     ~1ms (USB frame)
USB → Host terminal:        ~1ms
Total one-way:              ~2-3ms

For interactive shell: imperceptible.
```

---

## Kernel Upload via PicoMemcard

With this setup, kernel upload also goes through the PicoMemcard:

### Method 1: Write Kernel to Virtual Card from PC

```bash
# On host PC:
# 1. Mount PicoMemcard SD card via USB MSC
# 2. Copy kernel image to SD card
cp kernel.exe /media/picomemcard/kernel.exe

# 3. PicoMemcard firmware loads kernel image as card data
# 4. PS1 bootloader reads kernel from memory card (standard read commands)
# 5. Kernel boots, console output appears on /dev/ttyACM0
```

### Method 2: Stream Kernel via Serial Protocol

```bash
# On host PC:
# 1. Send kernel binary over USB CDC using custom upload protocol
./blackroo_upload.py /dev/ttyACM0 kernel.exe

# 2. PicoMemcard receives kernel data via USB CDC
# 3. PicoMemcard presents it to PS1 via memory card read commands
# 4. PS1 bootloader reads and executes
```

### Method 3: UniROM Compatibility

If UniROM is running (loaded via FreePSXBoot from another card slot), it still uses SIO1 for serial upload. The PicoMemcard approach is **separate** from UniROM — it's for when our own bootloader is running.

---

## Development Phases

| Phase | What | Effort | Depends On |
|-------|------|--------|------------|
| **1. Use stock PicoMemcard** | Standard 128KB card emulation, test with bu.c driver | None | PicoMemcard hardware ready |
| **2. Add USB CDC** | Add virtual serial port to PicoMemcard firmware | Small | Phase 1 |
| **3. Custom serial commands** | Add 0x58/0x59/0x5A commands to firmware | Medium | Phase 2 |
| **4. Kernel console driver** | Write picocon.c, register as console device | Medium | Phase 3 |
| **5. SIO0 bus arbitration** | Console + memory card coexistence on same bus | Medium | Phase 4 |
| **6. Kernel upload via card** | Bootloader reads kernel from PicoMemcard SD | Medium | Phase 4 |
| **7. Large virtual card** | Extend beyond 128KB using SD backing | Medium | Phase 1 |

**Phase 1 is zero effort** — use what exists today to test memory card I/O. The serial console features build on top incrementally.

---

## Bill of Materials

| Item | Cost (est.) | Notes |
|------|-------------|-------|
| Raspberry Pi Pico (RP2040) | $4-6 | Or RP2040-Zero ($3) |
| PicoMemcard adapter PCB | $5-10 | Open source design from dangiu |
| MicroSD card + slot | $4-7 | Any size works |
| USB cable (micro or C) | $2-3 | For PC connection |
| **Total** | **~$15-25** | **One device replaces serial cable + memory card** |

No FTDI needed. No SIO1 cable. No soldering to the PS1 motherboard. Just plug the PicoMemcard into a card slot and USB into your PC.

---

## Attribution

- PicoMemcard hardware and base firmware: [dangiu/PicoMemcard](https://github.com/dangiu/PicoMemcard) (MIT license)
- Custom serial protocol and console driver: New Blackroo work (GPL v2)
- SIO0 register access patterns: Derived from bu.c (Runix, GPL v2)
- USB CDC implementation: TinyUSB library (MIT license)

All Blackroo modifications to PicoMemcard firmware will be maintained in a separate
repository/branch with clear attribution to the original project.

---

*Blackroo Linux — PicoMemcard Dual-Mode Design*
*Everything through one card slot.*
