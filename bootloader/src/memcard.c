/*
 * memcard.c — Memory card operations for Blackroo bootloader
 *
 * Uses BIOS InitCARD/StartCARD for card detection, with
 * direct SIO0 register access for block-level read/write.
 *
 * Reference: psx-spx Controllers/Memory Cards, bu.c driver
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <stdint.h>
#include <string.h>
#include <psxapi.h>
#include <psxgpu.h>
#include <psxpad.h>
#include "memcard.h"
#include "menu.h"

extern uint8_t pad_buff[2][34];

/* Track whether PAD system is initialized */
static int pad_is_active = 0;

void memcard_set_pad_active(int active) {
    pad_is_active = active;
}

/* SIO0 registers (controller/memory card bus) */
#define SIO0_DATA   (*(volatile uint8_t  *)0x1F801040)
#define SIO0_STAT   (*(volatile uint32_t *)0x1F801044)
#define SIO0_MODE   (*(volatile uint16_t *)0x1F801048)
#define SIO0_CTRL   (*(volatile uint16_t *)0x1F80104A)
#define SIO0_BAUD   (*(volatile uint16_t *)0x1F80104E)

#define SIO0_STAT_TXRDY   (1 << 0)
#define SIO0_STAT_RXRDY   (1 << 1)

/* Memory card commands */
#define MC_ADDR   0x81    /* Memory card device address */
#define MC_CMD_READ  0x52 /* 'R' */
#define MC_CMD_WRITE 0x57 /* 'W' */
#define MC_CMD_ID    0x53 /* 'S' */

/* -------------------------------------------------------------- */
/* PAD pause/resume helpers                                       */
/* -------------------------------------------------------------- */

static void pause_pad(void) {
    if (pad_is_active) {
        StopPAD();
        /* Give SIO0 time to go idle */
        volatile int i;
        for (i = 0; i < 5000; i++);
    }
}

static void resume_pad(void) {
    if (pad_is_active) {
        StartPAD();
        ChangeClearPAD(1);
    }
}

/* -------------------------------------------------------------- */
/* Low-level SIO0 byte transfer                                   */
/* -------------------------------------------------------------- */

/*
 * Settling loops after asserting /JOYn.
 *
 * This was 2000 iterations, which is a few hundred microseconds. The kernel
 * driver needed a full 2 ms here before a multitap would answer reliably - a
 * fault found only because enabling its debug printks made detection start
 * working, the print latency supplying the delay that was missing. A tap needs
 * real time after select; a card straight in the port does not care.
 */
#define SIO0_SETTLE_LOOPS   80000

/* A gap between transactions. Probing a tap back to back succeeded on floors
 * 0 and 2 and failed on 1 and 3 - a perfect alternation, with all four cards
 * known good. The tap is still busy when the next select arrives. */
#define SIO0_GAP_LOOPS      160000

static void sio0_gap(void) {
    volatile int i;
    for (i = 0; i < SIO0_GAP_LOOPS; i++);
}

static void sio0_init_card(int port) {
    volatile int i;

    /* Reset SIO0 */
    SIO0_CTRL = 0x0040;
    for (i = 0; i < 500; i++);

    /* Drain any pending RX data */
    for (i = 0; i < 10; i++) {
        if (SIO0_STAT & SIO0_STAT_RXRDY)
            (void)SIO0_DATA;
    }

    /* Mode: 8-bit data, x1 baud multiplier */
    SIO0_MODE = 0x000D;

    /* Baud: ~250kHz for memory card */
    SIO0_BAUD = 0x0088;

    /*
     * Control register:
     *   bit 0:  TX enable
     *   bit 1:  DTR output (active = select device)
     *   bit 13: port select (0 = port 1, 1 = port 2)
     */
    SIO0_CTRL = ((port & 1) << 13) | 0x0003;

    /* Wait for the bus - and for a multitap - to settle */
    for (i = 0; i < SIO0_SETTLE_LOOPS; i++);
}

/* Address byte for a slot: 0x81 is floor A, 0x82 floor B, and so on. */
static uint8_t mc_addr(int slot) {
    return (uint8_t)(MC_ADDR + MC_FLOOR(slot));
}

const char *memcard_slot_name(int slot) {
    static const char *names[MC_SLOTS] = {
        "port 1 A", "port 1 B", "port 1 C", "port 1 D",
        "port 2 A", "port 2 B", "port 2 C", "port 2 D"
    };

    if (slot < 0 || slot >= MC_SLOTS)
        return "?";
    return names[slot];
}

/*
 * Transfer one byte: send tx, receive response.
 * Returns received byte or -1 on timeout.
 * Short timeout to avoid freezing if no card present.
 */
static int sio0_xfer(uint8_t tx) {
    int timeout;

    /* Wait for TX ready — short timeout */
    timeout = 50000;
    while (!(SIO0_STAT & SIO0_STAT_TXRDY)) {
        if (--timeout <= 0) return -1;
    }

    /* Send byte */
    SIO0_DATA = tx;

    /* Wait for RX response — short timeout */
    timeout = 50000;
    while (!(SIO0_STAT & SIO0_STAT_RXRDY)) {
        if (--timeout <= 0) return -1;
    }

    return SIO0_DATA;
}

static void sio0_shutdown(void) {
    /* Deassert DTR, reset SIO0 */
    SIO0_CTRL = 0x0040;
    volatile int i;
    for (i = 0; i < 500; i++);
}

/* -------------------------------------------------------------- */
/* Card detection using Get ID command (0x53)                     */
/* -------------------------------------------------------------- */

int memcard_detect(int slot) {
    int rx;

    pause_pad();
    sio0_init_card(MC_PORT(slot));

    /* Send address byte: memory card, on this multitap floor */
    rx = sio0_xfer(mc_addr(slot));
    if (rx < 0) goto fail;

    /* Send Get ID command */
    rx = sio0_xfer(MC_CMD_ID);
    if (rx < 0) goto fail;

    /* Card responds: 0x5A (ID byte 1) */
    rx = sio0_xfer(0x00);
    if (rx != 0x5A) goto fail;

    /* 0x5D (ID byte 2) */
    rx = sio0_xfer(0x00);
    if (rx != 0x5D) goto fail;

    /* Read remaining ID response and discard */
    sio0_xfer(0x00);  /* 0x5C */
    sio0_xfer(0x00);  /* 0x5D */
    sio0_xfer(0x00);  /* block count MSB */
    sio0_xfer(0x00);  /* block count LSB */
    sio0_xfer(0x00);  /* block size MSB */
    sio0_xfer(0x00);  /* block size LSB = 0x80 (128) */
    sio0_xfer(0x00);  /* sector count MSB */
    sio0_xfer(0x00);  /* sector count LSB */

    sio0_shutdown();
    resume_pad();
    return 1;

fail:
    sio0_shutdown();
    resume_pad();
    return 0;
}

/* -------------------------------------------------------------- */
/* Read a 128-byte block using Read command (0x52)                */
/* -------------------------------------------------------------- */

int memcard_read_block(int slot, int block, uint8_t *buf) {
    int rx, i;
    uint8_t msb, lsb;
    uint8_t checksum;

    msb = (block >> 8) & 0xFF;
    lsb = block & 0xFF;

    pause_pad();
    sio0_init_card(MC_PORT(slot));

    /* Address + command */
    rx = sio0_xfer(mc_addr(slot));
    if (rx < 0) goto fail;
    sio0_xfer(MC_CMD_READ);

    /* Card ID response */
    rx = sio0_xfer(0x00);
    if (rx != 0x5A) goto fail;
    rx = sio0_xfer(0x00);
    if (rx != 0x5D) goto fail;

    /* Send block address */
    sio0_xfer(msb);
    sio0_xfer(lsb);

    /* Command ACK */
    rx = sio0_xfer(0x00);
    if (rx != 0x5C) goto fail;
    rx = sio0_xfer(0x00);
    if (rx != 0x5D) goto fail;

    /* Address echo */
    rx = sio0_xfer(0x00);  /* MSB echo */
    if (rx != msb) goto fail;
    rx = sio0_xfer(0x00);  /* LSB echo */
    if (rx != lsb) goto fail;

    /* Read 128 data bytes */
    checksum = msb ^ lsb;
    for (i = 0; i < BU_BLOCK_SIZE; i++) {
        rx = sio0_xfer(0x00);
        if (rx < 0) goto fail;
        buf[i] = rx;
        checksum ^= rx;
    }

    /* Verify checksum */
    rx = sio0_xfer(0x00);
    if (rx != checksum) goto fail;

    /* End byte: 0x47 = 'G' (Good) */
    rx = sio0_xfer(0x00);
    if (rx != 0x47) goto fail;

    sio0_shutdown();
    resume_pad();
    return 0;

fail:
    sio0_shutdown();
    resume_pad();
    return -1;
}

/* -------------------------------------------------------------- */
/* Write a 128-byte block using Write command (0x57)              */
/* -------------------------------------------------------------- */

int memcard_write_block(int slot, int block, const uint8_t *buf) {
    int rx, i;
    uint8_t msb, lsb;
    uint8_t checksum;

    msb = (block >> 8) & 0xFF;
    lsb = block & 0xFF;

    pause_pad();
    sio0_init_card(MC_PORT(slot));

    /* Address + command */
    rx = sio0_xfer(mc_addr(slot));
    if (rx < 0) goto fail;
    sio0_xfer(MC_CMD_WRITE);

    /* Card ID response */
    rx = sio0_xfer(0x00);
    if (rx != 0x5A) goto fail;
    rx = sio0_xfer(0x00);
    if (rx != 0x5D) goto fail;

    /* Send block address */
    sio0_xfer(msb);
    sio0_xfer(lsb);

    /* Write 128 data bytes */
    checksum = msb ^ lsb;
    for (i = 0; i < BU_BLOCK_SIZE; i++) {
        sio0_xfer(buf[i]);
        checksum ^= buf[i];
    }

    /* Send checksum */
    sio0_xfer(checksum);

    /* ACK bytes */
    rx = sio0_xfer(0x00);
    if (rx != 0x5C) goto fail;
    rx = sio0_xfer(0x00);
    if (rx != 0x5D) goto fail;

    /* Status: 0x47='G' (Good), 0x4E='N' (Bad CRC), 0xFF=Bad sector */
    rx = sio0_xfer(0x00);
    if (rx != 0x47) goto fail;

    sio0_shutdown();
    resume_pad();
    return 0;

fail:
    sio0_shutdown();
    resume_pad();
    return -1;
}

/* -------------------------------------------------------------- */
/* Higher-level functions                                         */
/* -------------------------------------------------------------- */

int memcard_format(int slot, int sequence_number) {
    blackroo_card_header_t hdr;
    uint8_t block[BU_BLOCK_SIZE];

    memset(&hdr, 0, sizeof(hdr));
    hdr.id = BU_ID;
    hdr.size = BU_CARD_BLOCKS;
    hdr.serial = 0xABCD0000 + sequence_number;
    /* bu.c judges each card on its own `number`, so these must be distinct
     * across the set - the slot index guarantees that. */
    hdr.number = sequence_number;

    memset(block, 0, BU_BLOCK_SIZE);
    memcpy(block, &hdr, sizeof(hdr));

    return memcard_write_block(slot, 0, block);
}

int memcard_is_blackroo(int slot) {
    blackroo_card_header_t hdr;
    if (memcard_read_header(slot, &hdr) < 0) return 0;
    return (hdr.id == BU_ID);
}

int memcard_read_header(int slot, blackroo_card_header_t *hdr) {
    uint8_t block[BU_BLOCK_SIZE];
    if (memcard_read_block(slot, 0, block) < 0) return -1;
    memcpy(hdr, block, sizeof(*hdr));
    return 0;
}

/* -------------------------------------------------------------- */
/* Manager menu                                                    */
/* -------------------------------------------------------------- */

/*
 * Eight slots, not two.
 *
 * The old version knew about "Card 1" and "Card 2" - the two console ports -
 * and could not see anything behind a multitap, which is where this project's
 * cards actually live. It now walks all four floors of both ports, using the
 * same numbering as the kernel so a slot means the same thing everywhere:
 * BRMON's `card format 5`, /dev/bul's fifth card and "port 2 A" here are one
 * and the same.
 *
 * Formatting writes the Blackroo header (id 0x1234) that bu.c requires before
 * it will claim a card at all - a stock Sony card reads as "not found" by
 * design. Doing it here means cards can be prepared without booting Linux.
 */

static int mc_present[MC_SLOTS];
static int mc_blackroo[MC_SLOTS];
static int mc_number[MC_SLOTS];

static void mc_scan(void) {
    int i;

    for (i = 0; i < MC_SLOTS; i++) {
        mc_present[i] = memcard_detect(i);
        mc_blackroo[i] = 0;
        mc_number[i] = -1;

        if (mc_present[i]) {
            blackroo_card_header_t hdr;

            if (memcard_read_header(i, &hdr) == 0 && hdr.id == BU_ID) {
                mc_blackroo[i] = 1;
                mc_number[i] = (int)hdr.number;
            }
        }

        sio0_gap();   /* a tap is still busy when the next select arrives */
    }
}

/* Wait for every button to come up, so one press is not read twice. */
static void mc_wait_release(void) {
    PADTYPE *pad;
    uint16_t b;

    do {
        VSync(0);
        pad = (PADTYPE *)pad_buff[0];
        b = (pad->stat == 0) ? (uint16_t)~pad->btn : 0;
    } while (b);
}

static void mc_show_block(int slot) {
    uint8_t blk[BU_BLOCK_SIZE];
    int ok = (memcard_read_block(slot, 0, blk) == 0);

    mc_wait_release();

    while (1) {
        PADTYPE *pad;
        uint16_t b;
        int j;

        FntPrint(0, "\n  BLOCK 0 - slot %d (%s)\n\n",
                 slot + 1, memcard_slot_name(slot));

        if (!ok) {
            FntPrint(0, "  Read failed.\n");
        } else {
            for (j = 0; j < 64; j++) {
                if ((j % 16) == 0) FntPrint(0, "  ");
                FntPrint(0, "%02X ", blk[j]);
                if ((j % 16) == 15) FntPrint(0, "\n");
            }
            FntPrint(0, "\n  ... (%d more bytes)\n", BU_BLOCK_SIZE - 64);
        }

        FntPrint(0, "\n  [Start] Back\n");
        FntFlush(-1);
        menu_swap_buffers();

        pad = (PADTYPE *)pad_buff[0];
        b = (pad->stat == 0) ? (uint16_t)~pad->btn : 0;
        if (b & (PAD_START | PAD_SELECT | PAD_CROSS | PAD_CIRCLE)) {
            mc_wait_release();
            return;
        }
    }
}

/* Destructive, so make it say so and require a deliberate second press. */
static int mc_confirm_format(int slot) {
    mc_wait_release();

    while (1) {
        PADTYPE *pad;
        uint16_t b;

        FntPrint(0, "\n  FORMAT SLOT %d (%s)\n", slot + 1,
                 memcard_slot_name(slot));
        FntPrint(0, "  =========================\n\n");
        FntPrint(0, "  This ERASES block 0, the card's\n");
        FntPrint(0, "  directory. Any saves on it are lost.\n\n");
        FntPrint(0, "  It will be given Blackroo id 0x%04X\n", BU_ID);
        FntPrint(0, "  and RAID number %d.\n\n", slot);
        FntPrint(0, "  [Start] Format    [Select] Cancel\n");
        FntFlush(-1);
        menu_swap_buffers();

        pad = (PADTYPE *)pad_buff[0];
        b = (pad->stat == 0) ? (uint16_t)~pad->btn : 0;

        if (b & PAD_START)  { mc_wait_release(); return 1; }
        if (b & (PAD_SELECT | PAD_CIRCLE)) { mc_wait_release(); return 0; }
    }
}

void memcard_manager_menu(void) {
    int selected = 0;
    uint16_t prev_buttons = 0;
    int i, total;

    mc_scan();
    menu_wait_release();

    while (1) {
        FntPrint(0, "\n  MEMORY CARD MANAGER\n");
        FntPrint(0, "  ===================\n\n");

        total = 0;
        for (i = 0; i < MC_SLOTS; i++) {
            FntPrint(0, "  %s %d %-9s ",
                     selected == i ? ">" : " ", i + 1,
                     memcard_slot_name(i));

            if (!mc_present[i]) {
                FntPrint(0, "-\n");
            } else if (mc_blackroo[i]) {
                FntPrint(0, "128K  Blackroo #%d\n", mc_number[i]);
                total += 127;
            } else {
                FntPrint(0, "128K  not formatted\n");
            }
        }

        FntPrint(0, "\n  Linux would join %d KB\n\n", total);
        FntPrint(0, "  [Start] Format   [O] Read block 0\n");
        FntPrint(0, "  [[]] Re-scan     [Select] Back\n");

        FntFlush(-1);
        menu_swap_buffers();

        {
            PADTYPE *pad = (PADTYPE *)pad_buff[0];
            uint16_t btn = 0;
            uint16_t pressed;

            if (pad->stat == 0)
                btn = (uint16_t)~pad->btn;

            pressed = btn & ~prev_buttons;
            prev_buttons = btn;

            if ((pressed & PAD_UP) && selected > 0)
                selected--;
            if ((pressed & PAD_DOWN) && selected < MC_SLOTS - 1)
                selected++;

            if (pressed & PAD_SELECT)
                return;

            if (pressed & PAD_SQUARE) {
                mc_scan();
                prev_buttons = 0;
            }

            if ((pressed & PAD_CIRCLE) && mc_present[selected]) {
                mc_show_block(selected);
                prev_buttons = 0;
            }

            if ((pressed & (PAD_START | PAD_CROSS)) && mc_present[selected]) {
                if (mc_confirm_format(selected)) {
                    /* The slot index is the RAID sequence number: distinct
                     * per card, which is what bu.c needs. */
                    if (memcard_format(selected, selected) == 0) {
                        mc_blackroo[selected] = 1;
                        mc_number[selected] = selected;
                    } else {
                        mc_blackroo[selected] = 0;
                    }
                }
                prev_buttons = 0;
            }
        }
    }
}
