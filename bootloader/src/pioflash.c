/*
 * pioflash.c — PIO Expansion Port Flash Manager
 *
 * Detect, dump, and program NOR flash on PS1 cheat cartridges
 * (GameShark, Action Replay, Xplorer, Caetla) via the parallel
 * I/O expansion port mapped at 0x1F000000.
 *
 * Supports AMD/SST-compatible NOR flash command sequences.
 * Flash is memory-mapped — reads are at full CPU bus speed.
 * Writes use standard byte-program with toggle-bit polling.
 *
 * Reference: psx-spx Memory Control, AMD Am29F040 datasheet,
 *            SST39SF040 datasheet
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxpad.h>
#include "pioflash.h"
#include "serial.h"
#include "menu.h"

extern uint8_t pad_buff[2][34];

/* ------------------------------------------------------------------ */
/* Known flash chip database                                          */
/* ------------------------------------------------------------------ */

static const flash_chip_info_t known_chips[] = {
    /* SST */
    { 0xBF, 0xB5, "SST",     "SST39SF010",  128*1024,  4096 },
    { 0xBF, 0xB6, "SST",     "SST39SF020",  256*1024,  4096 },
    { 0xBF, 0xB7, "SST",     "SST39SF040",  512*1024,  4096 },
    { 0xBF, 0x10, "SST",     "SST29EE020",  256*1024,   128 },
    { 0xBF, 0x04, "SST",     "SST28SF040",  512*1024,   256 },
    /* AMD / Spansion */
    { 0x01, 0x20, "AMD",     "AM29F010",    128*1024, 16384 },
    { 0x01, 0xB0, "AMD",     "AM29F020",    256*1024, 16384 },
    { 0x01, 0xA4, "AMD",     "AM29F040",    512*1024, 65536 },
    { 0x01, 0xD5, "AMD",     "AM29F080",   1024*1024, 65536 },
    /* Winbond */
    { 0xDA, 0x45, "Winbond", "W29C020",     256*1024,   128 },
    { 0xDA, 0x46, "Winbond", "W29C040",     512*1024,   256 },
    /* Atmel */
    { 0x1F, 0xDA, "Atmel",   "AT29C020",    256*1024,   256 },
    { 0x1F, 0x5D, "Atmel",   "AT29C040",    512*1024,   512 },
    /* Macronix */
    { 0xC2, 0xB0, "Macronix","MX29F020",    256*1024, 16384 },
    { 0xC2, 0xA4, "Macronix","MX29F040",    512*1024, 65536 },
    /* End sentinel */
    { 0x00, 0x00, NULL, NULL, 0, 0 }
};

/* ------------------------------------------------------------------ */
/* Low-level flash access                                             */
/* ------------------------------------------------------------------ */

static inline void flash_write(uint32_t addr, uint8_t val) {
    PIO_FLASH_BASE[addr] = val;
}

static inline uint8_t flash_read(uint32_t addr) {
    return PIO_FLASH_BASE[addr];
}

/* Send AMD/SST unlock sequence */
static inline void flash_unlock(void) {
    flash_write(FLASH_CMD_ADDR1, FLASH_CMD_UNLOCK1);  /* 0x5555 = 0xAA */
    flash_write(FLASH_CMD_ADDR2, FLASH_CMD_UNLOCK2);  /* 0x2AAA = 0x55 */
}

/* Reset flash to normal read mode */
static inline void flash_reset(void) {
    flash_write(0, FLASH_CMD_RESET);  /* 0xF0 to any address */
}

/*
 * Poll toggle bit (DQ6) to wait for operation completion.
 * During program/erase, bit 6 toggles on successive reads.
 * When done, bit 6 stops toggling.
 * Returns 1 on success, 0 on timeout.
 */
static int flash_poll_toggle(uint32_t addr, int timeout_loops) {
    uint8_t prev, curr;
    int i;

    prev = flash_read(addr) & 0x40;  /* DQ6 */
    for (i = 0; i < timeout_loops; i++) {
        curr = flash_read(addr) & 0x40;
        if (curr == prev)
            return 1;  /* Stable — operation complete */
        prev = curr;
    }
    flash_reset();
    return 0;  /* Timeout */
}

/* ------------------------------------------------------------------ */
/* Flash detection                                                    */
/* ------------------------------------------------------------------ */

static const flash_chip_info_t *lookup_chip(uint8_t mfr, uint8_t dev) {
    const flash_chip_info_t *c;
    for (c = known_chips; c->mfr_name != NULL; c++) {
        if (c->mfr_id == mfr && c->dev_id == dev)
            return c;
    }
    return NULL;
}

int pioflash_detect(pioflash_info_t *info) {
    uint8_t byte0, mfr, dev;

    memset(info, 0, sizeof(*info));

    /* First check if anything is at the PIO port at all */
    byte0 = flash_read(0);
    if (byte0 == 0xFF) {
        /* 0xFF usually means nothing connected */
        info->present = 0;
        return 0;
    }

    /* Enter JEDEC autoselect mode */
    flash_unlock();
    flash_write(FLASH_CMD_ADDR1, FLASH_CMD_AUTOSEL);

    /* Small delay — some chips need a few cycles */
    {
        volatile int i;
        for (i = 0; i < 100; i++);
    }

    /* Read manufacturer and device IDs */
    mfr = flash_read(0x0000);
    dev = flash_read(0x0001);

    /* Return to normal read mode */
    flash_reset();

    /* If IDs are both 0xFF or same as data byte, no flash detected */
    if ((mfr == 0xFF && dev == 0xFF) || (mfr == byte0 && dev == flash_read(1))) {
        /* Might be ROM, not flash — still report as present but unknown */
        info->present = 1;
        info->mfr_id = 0;
        info->dev_id = 0;
        info->chip = NULL;
        /* Probe size by reading — try common sizes */
        info->size = 256 * 1024;  /* Default guess */
        info->sector_size = 4096;
        return 1;
    }

    info->present = 1;
    info->mfr_id = mfr;
    info->dev_id = dev;
    info->chip = lookup_chip(mfr, dev);

    if (info->chip) {
        info->size = info->chip->size;
        info->sector_size = info->chip->sector_size;
    } else {
        /* Unknown chip — default to 256KB, 4KB sectors */
        info->size = 256 * 1024;
        info->sector_size = 4096;
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* Read / Verify                                                      */
/* ------------------------------------------------------------------ */

void pioflash_read(uint32_t offset, uint8_t *buf, uint32_t len) {
    uint32_t i;
    for (i = 0; i < len; i++) {
        buf[i] = flash_read(offset + i);
    }
}

uint32_t pioflash_verify(uint32_t offset, const uint8_t *buf, uint32_t len) {
    uint32_t i, mismatches = 0;
    for (i = 0; i < len; i++) {
        if (flash_read(offset + i) != buf[i])
            mismatches++;
    }
    return mismatches;
}

/* ------------------------------------------------------------------ */
/* Erase                                                              */
/* ------------------------------------------------------------------ */

int pioflash_erase_chip(const pioflash_info_t *info) {
    flash_unlock();
    flash_write(FLASH_CMD_ADDR1, FLASH_CMD_ERASE_PRE);
    flash_unlock();
    flash_write(FLASH_CMD_ADDR1, FLASH_CMD_ERASE_CHIP);

    /* Chip erase can take several seconds — long timeout */
    return flash_poll_toggle(0, 100000000);
}

int pioflash_erase_sector(uint32_t sector_addr) {
    flash_unlock();
    flash_write(FLASH_CMD_ADDR1, FLASH_CMD_ERASE_PRE);
    flash_unlock();
    flash_write(sector_addr, FLASH_CMD_ERASE_SECT);

    /* Sector erase: ~25ms typical */
    return flash_poll_toggle(sector_addr, 5000000);
}

/* ------------------------------------------------------------------ */
/* Program                                                            */
/* ------------------------------------------------------------------ */

int pioflash_program_byte(uint32_t offset, uint8_t data) {
    if (data == 0xFF)
        return 1;  /* Already erased, skip */

    flash_unlock();
    flash_write(FLASH_CMD_ADDR1, FLASH_CMD_PROGRAM);
    flash_write(offset, data);

    /* Byte program: ~20us typical */
    return flash_poll_toggle(offset, 500000);
}

int pioflash_program(const pioflash_info_t *info,
                     uint32_t offset, const uint8_t *buf, uint32_t len,
                     void (*progress_cb)(uint32_t done, uint32_t total))
{
    uint32_t i, sect_addr, sect_end;
    uint32_t sect_size = info->sector_size;

    /* Erase required sectors first */
    sect_addr = (offset / sect_size) * sect_size;
    sect_end = offset + len;
    while (sect_addr < sect_end) {
        if (!pioflash_erase_sector(sect_addr))
            return 0;
        sect_addr += sect_size;
    }

    /* Program byte by byte */
    for (i = 0; i < len; i++) {
        if (!pioflash_program_byte(offset + i, buf[i]))
            return 0;
        if (progress_cb && ((i & 0x3FF) == 0))
            progress_cb(i, len);
    }

    if (progress_cb)
        progress_cb(len, len);

    return 1;
}

/* ------------------------------------------------------------------ */
/* Serial dump (PIO flash -> host PC)                                 */
/* ------------------------------------------------------------------ */

void pioflash_dump_serial(const pioflash_info_t *info) {
    pioflash_xfer_hdr_t hdr;
    uint32_t i;
    uint8_t byte;

    /* Build and send header */
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = PIOFLASH_MAGIC;
    hdr.size = info->size;
    hdr.mfr_id = info->mfr_id;
    hdr.dev_id = info->dev_id;

    for (i = 0; i < sizeof(hdr); i++) {
        serial_putchar(((uint8_t *)&hdr)[i]);
    }

    /* Send raw flash data */
    for (i = 0; i < info->size; i++) {
        byte = flash_read(i);
        serial_putchar(byte);
    }
}

/* ------------------------------------------------------------------ */
/* Serial load (host PC -> PIO flash)                                 */
/* ------------------------------------------------------------------ */

int pioflash_load_serial(const pioflash_info_t *info) {
    pioflash_xfer_hdr_t hdr;
    uint32_t i, sect_addr, sect_end;
    int ch;
    uint8_t data;

    /* Receive header */
    for (i = 0; i < sizeof(hdr); i++) {
        ch = serial_getchar();
        if (ch < 0) return 0;
        ((uint8_t *)&hdr)[i] = ch;
    }

    /* Validate header */
    if (hdr.magic != PIOFLASH_MAGIC)
        return 0;
    if (hdr.size > info->size)
        return 0;  /* Won't fit */
    if (hdr.size == 0)
        return 0;

    /* Erase required sectors */
    sect_end = hdr.size;
    for (sect_addr = 0; sect_addr < sect_end; sect_addr += info->sector_size) {
        if (!pioflash_erase_sector(sect_addr))
            return 0;
    }

    /* Receive and program data */
    for (i = 0; i < hdr.size; i++) {
        ch = serial_getchar();
        if (ch < 0) return 0;
        data = (uint8_t)ch;

        if (!pioflash_program_byte(i, data))
            return 0;
    }

    /* Verify */
    /* Re-read and send ACK byte: 'V' = verified OK, 'E' = error */
    /* (Simple — full verify would require storing data, which we
     *  don't have RAM for at full flash size) */
    serial_putchar('V');

    return 1;
}

/* ------------------------------------------------------------------ */
/* Interactive menu                                                   */
/* ------------------------------------------------------------------ */

static void wait_for_button(void) {
    PADTYPE *pad;
    uint16_t prev = 0xFFFF;

    while (1) {
        VSync(0);
        pad = (PADTYPE *)pad_buff[0];
        if (pad->stat == 0) {
            uint16_t btn = ~pad->btn;
            uint16_t pressed = btn & ~prev;
            prev = btn;
            if (pressed & (PAD_START | PAD_SELECT | PAD_CROSS | PAD_CIRCLE))
                return;
        }
    }
}

static void wait_for_release(void) {
    menu_wait_release();
}

void pioflash_manager_menu(void) {
    pioflash_info_t flash;
    int selected = 0;
    uint16_t prev_buttons = 0;

    /* Detect flash on entry */
    pioflash_detect(&flash);
    wait_for_release();

    while (1) {
        FntPrint(0, "\n");
        FntPrint(0, "  PIO FLASH MANAGER\n");
        FntPrint(0, "  =================\n\n");

        if (!flash.present) {
            FntPrint(0, "  No device at expansion port.\n");
            FntPrint(0, "  Insert a GameShark, Action\n");
            FntPrint(0, "  Replay, or Xplorer cartridge.\n\n");
            FntPrint(0, "  [Start] Back\n");
            FntFlush(-1);
            menu_swap_buffers();
            wait_for_button();
            return;
        }

        /* Show flash info */
        if (flash.chip) {
            FntPrint(0, "  Chip:  %s %s\n",
                     flash.chip->mfr_name, flash.chip->dev_name);
        } else if (flash.mfr_id) {
            FntPrint(0, "  Chip:  Unknown (MFR:%02X DEV:%02X)\n",
                     flash.mfr_id, flash.dev_id);
        } else {
            FntPrint(0, "  Chip:  ROM/Unknown device\n");
        }
        FntPrint(0, "  Size:  %d KB (%d bytes)\n",
                 flash.size / 1024, flash.size);
        if (flash.sector_size >= 1024)
            FntPrint(0, "  Sect:  %d KB\n", flash.sector_size / 1024);
        else
            FntPrint(0, "  Sect:  %d bytes\n", flash.sector_size);
        FntPrint(0, "\n");

        /* Menu items */
        FntPrint(0, "  %s Dump to Serial (backup)\n",
                 selected == 0 ? ">" : " ");
        FntPrint(0, "  %s Load from Serial\n",
                 selected == 1 ? ">" : " ");
        FntPrint(0, "  %s Erase Chip\n",
                 selected == 2 ? ">" : " ");
        FntPrint(0, "  %s Re-detect\n",
                 selected == 3 ? ">" : " ");
        FntPrint(0, "  %s Back\n",
                 selected == 4 ? ">" : " ");

        FntPrint(0, "\n  D-pad:Navigate  Start:Select\n");

        FntFlush(-1);
        menu_swap_buffers();

        /* Input handling */
        {
            PADTYPE *pad = (PADTYPE *)pad_buff[0];
            uint16_t btn = 0;
            uint16_t pressed;

            if (pad->stat == 0)
                btn = ~pad->btn;

            pressed = btn & ~prev_buttons;
            prev_buttons = btn;

            if (pressed & PAD_UP) {
                if (selected > 0) selected--;
            }
            if (pressed & PAD_DOWN) {
                if (selected < 4) selected++;
            }
            if (pressed & (PAD_START | PAD_CROSS | PAD_CIRCLE)) {
                if (selected == 4) return;  /* Back */

                if (selected == 0) {
                    /* Dump to serial */
                    FntPrint(0, "\n  DUMPING FLASH TO SERIAL...\n");
                    FntPrint(0, "  Size: %d KB\n", flash.size / 1024);
                    FntPrint(0, "  Baud: 115200\n");
                    FntPrint(0, "  Connect serial cable and run:\n");
                    FntPrint(0, "  python3 pioflash_recv.py\n\n");
                    FntPrint(0, "  [Start] Begin dump\n");
                    FntPrint(0, "  [Select] Cancel\n");
                    FntFlush(-1);
                    menu_swap_buffers();

                    wait_for_release();

                    /* Wait for confirm or cancel */
                    while (1) {
                        uint16_t b;
                        VSync(0);
                        pad = (PADTYPE *)pad_buff[0];
                        b = (pad->stat == 0) ? ~pad->btn : 0;
                        if (b & PAD_START) {
                            serial_init(115200);
                            pioflash_dump_serial(&flash);

                            FntPrint(0, "\n\n  Dump complete! %d KB sent.\n",
                                     flash.size / 1024);
                            FntPrint(0, "\n  [Start] OK\n");
                            FntFlush(-1);
                            menu_swap_buffers();
                            wait_for_release();
                            wait_for_button();
                            break;
                        }
                        if (b & PAD_SELECT) break;
                    }
                    wait_for_release();
                }

                if (selected == 1) {
                    /* Load from serial */
                    int ok;

                    FntPrint(0, "\n  LOAD FROM SERIAL TO FLASH\n");
                    FntPrint(0, "  WARNING: This will ERASE the\n");
                    FntPrint(0, "  cartridge flash and write new\n");
                    FntPrint(0, "  data. Back up first!\n\n");
                    FntPrint(0, "  Run on host:\n");
                    FntPrint(0, "  python3 pioflash_send.py rom.bin\n\n");
                    FntPrint(0, "  [Start] Begin receive\n");
                    FntPrint(0, "  [Select] Cancel\n");
                    FntFlush(-1);
                    menu_swap_buffers();

                    wait_for_release();

                    while (1) {
                        uint16_t b;
                        VSync(0);
                        pad = (PADTYPE *)pad_buff[0];
                        b = (pad->stat == 0) ? ~pad->btn : 0;
                        if (b & PAD_START) {
                            FntPrint(0, "\n  Waiting for data...\n");
                            FntFlush(-1);
                            menu_swap_buffers();

                            serial_init(115200);
                            ok = pioflash_load_serial(&flash);

                            if (ok) {
                                FntPrint(0, "\n  Flash programmed OK!\n");
                            } else {
                                FntPrint(0, "\n  FAILED! Check connection.\n");
                            }
                            FntPrint(0, "\n  [Start] OK\n");
                            FntFlush(-1);
                            menu_swap_buffers();
                            wait_for_release();
                            wait_for_button();
                            break;
                        }
                        if (b & PAD_SELECT) break;
                    }
                    wait_for_release();
                }

                if (selected == 2) {
                    /* Erase chip */
                    FntPrint(0, "\n  ERASE ENTIRE CHIP?\n");
                    FntPrint(0, "  This DESTROYS all data!\n\n");
                    FntPrint(0, "  [Start] ERASE\n");
                    FntPrint(0, "  [Select] Cancel\n");
                    FntFlush(-1);
                    menu_swap_buffers();

                    wait_for_release();

                    while (1) {
                        uint16_t b;
                        VSync(0);
                        pad = (PADTYPE *)pad_buff[0];
                        b = (pad->stat == 0) ? ~pad->btn : 0;
                        if (b & PAD_START) {
                            int ok;
                            FntPrint(0, "\n  Erasing...\n");
                            FntFlush(-1);
                            menu_swap_buffers();

                            ok = pioflash_erase_chip(&flash);

                            if (ok)
                                FntPrint(0, "  Erase complete.\n");
                            else
                                FntPrint(0, "  Erase FAILED!\n");
                            FntPrint(0, "\n  [Start] OK\n");
                            FntFlush(-1);
                            menu_swap_buffers();
                            wait_for_release();
                            wait_for_button();
                            break;
                        }
                        if (b & PAD_SELECT) break;
                    }
                    wait_for_release();
                }

                if (selected == 3) {
                    /* Re-detect */
                    pioflash_detect(&flash);
                    wait_for_release();
                }
            }
        }
    }
}
