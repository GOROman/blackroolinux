/*
 * settings.c — Persistent boot settings for Blackroo bootloader
 *
 * Settings stored on memory card block 1 (128 bytes).
 * Survives power cycles. Editable from bootloader menu.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxpad.h>
#include "settings.h"
#include "video.h"
#include "memcard.h"
#include "menu.h"
#include <stdio.h>

extern uint8_t pad_buff[2][34];

void settings_default(blackroo_settings_t *settings) {
    memset(settings, 0, sizeof(*settings));
    settings->magic = SETTINGS_MAGIC;
    settings->version = SETTINGS_VERSION;
    settings->boot_source = BOOT_SRC_SERIAL_SLOW;
    /*
     * BLACKROO 2026-08-22: /dev/ram0, not the memory cards.
     *
     * This said ROOT_DEV_CARD_RAID, which was harmless only because nothing
     * read it: cdrom_boot() used a hard-coded "root=/dev/ram0 ..." string.
     * The moment that was changed to build the command line from settings -
     * so a chosen video mode could reach the kernel - this default became the
     * boot device, and the kernel panicked trying to mount memory cards that
     * have no filesystem on them.
     *
     * Every disc we ship carries an embedded initrd, and card writes through
     * the block driver are still unproven, so the RAM disk is the only
     * defensible default. A default that is never exercised is not a default,
     * it is a landmine.
     */
    settings->root_device = ROOT_DEV_RAM;
    settings->init_program = INIT_BIN_SH;
    settings->console_mode = CONSOLE_GPU_SERIAL;
    settings->ram_size = 0;  /* auto */
    settings->video_mode = 0;  /* auto */
    settings->video_index = 0; /* 0 = follow the console's region */
    settings->serial_baud = 0;  /* 115200 */
    settings->auto_boot = 0;
    settings->auto_boot_timeout = 5;
    settings->cmdline[0] = '\0';
}

void settings_load(blackroo_settings_t *settings) {
    uint8_t block[128];

    settings_default(settings);

    /* Try to read block 1 from card slot 0 */
    if (memcard_read_block(0, 1, block) != 0)
        return;  /* Read failed — use defaults */

    /* Check magic */
    blackroo_settings_t *stored = (blackroo_settings_t *)block;
    if (stored->magic != SETTINGS_MAGIC || stored->version != SETTINGS_VERSION)
        return;  /* Invalid or different version — use defaults */

    memcpy(settings, block, sizeof(*settings));
}

int settings_save(const blackroo_settings_t *settings) {
    return memcard_write_block(0, 1, (const uint8_t *)settings);
}

void settings_build_cmdline(const blackroo_settings_t *settings,
                            char *buf, int bufsize) {
    /* If custom cmdline set, use it */
    if (settings->cmdline[0] != '\0') {
        strncpy(buf, settings->cmdline, bufsize - 1);
        buf[bufsize - 1] = '\0';
        return;
    }

    /* Build from settings */
    const char *root;
    const char *init;
    const char *console;

    switch (settings->root_device) {
        case ROOT_DEV_CARD_SINGLE: root = "root=/dev/bu0"; break;
        case ROOT_DEV_CARD_RAID:   root = "root=/dev/bul"; break;
        case ROOT_DEV_RAM:         root = "root=/dev/ram0"; break;
        /* The disc's own ROOT.IMG. psxcd finds it by walking the ISO9660
         * root directory, so no LBA is passed here - see docs/24 §5.5 B. */
        case ROOT_DEV_CDROM:       root = "root=/dev/psxcd"; break;
        default:                   root = "root=/dev/bu0"; break;
    }

    switch (settings->init_program) {
        case INIT_BIN_SH:     init = "init=/bin/sh"; break;
        case INIT_LINUXRC:    init = "init=/linuxrc"; break;
        case INIT_SBIN_INIT:  init = "init=/sbin/init"; break;
        default:              init = "init=/bin/sh"; break;
    }

    switch (settings->console_mode) {
        case CONSOLE_GPU_SERIAL:
            console = "console=ttyS0,115200 console=tty0";
            break;
        case CONSOLE_GPU_ONLY:
            console = "console=tty0";
            break;
        case CONSOLE_SERIAL_ONLY:
            console = "console=ttyS0,115200";
            break;
        default:
            console = "console=ttyS0,115200 console=tty0";
            break;
    }

    /*
     * The display mode travels with the command line, so the kernel comes up
     * in whatever the user left the menu on. psxcon.c currently picks its
     * resolution at compile time and ignores this - wiring that up is the
     * other half of the job.
     */
    if (settings->video_index)
        snprintf(buf, bufsize, "%s %s %s %s", root, init, console,
                 video_cmdline_arg(settings->video_index - 1));
    else
        snprintf(buf, bufsize, "%s %s %s", root, init, console);
}

/* Helper: read pad with debounce */
static uint16_t read_pad_once(void) {
    static uint16_t prev = 0;
    PADTYPE *pad = (PADTYPE *)pad_buff[0];
    uint16_t btn = 0;
    uint16_t pressed;

    if (pad->stat == 0)
        btn = ~pad->btn;

    pressed = btn & ~prev;
    prev = btn;
    return pressed;
}

static const char *boot_src_names[] = {
    "Serial (115200)", "Serial (518400)", "Memory Card", "CD-ROM"
};
static const char *root_dev_names[] = {
    "/dev/bu0 (single)", "/dev/bul (RAID)", "/dev/ram0 (ramdisk)",
    "/dev/psxcd (this disc)"
};
static const char *init_names[] = {
    "/bin/sh", "/linuxrc", "/sbin/init"
};
static const char *console_names[] = {
    "GPU + Serial", "GPU only", "Serial only"
};
static const char *ram_names[] = {
    "Auto", "2 MB", "4 MB", "8 MB"
};

void settings_menu(blackroo_settings_t *settings) {
    int selected = 0;
    int num_items = 8;
    int changed = 0;

    while (1) {
        FntPrint(0, "\n");
        FntPrint(0, "  SYSTEM SETTINGS\n");
        FntPrint(0, "  ===============\n\n");

        FntPrint(0, "  %s Boot Source:  %s\n",
                 selected == 0 ? ">" : " ",
                 boot_src_names[settings->boot_source]);
        FntPrint(0, "  %s Root Device: %s\n",
                 selected == 1 ? ">" : " ",
                 root_dev_names[settings->root_device]);
        FntPrint(0, "  %s Init:        %s\n",
                 selected == 2 ? ">" : " ",
                 init_names[settings->init_program]);
        FntPrint(0, "  %s Console:     %s\n",
                 selected == 3 ? ">" : " ",
                 console_names[settings->console_mode]);
        FntPrint(0, "  %s RAM:         %s\n",
                 selected == 4 ? ">" : " ",
                 ram_names[settings->ram_size > 3 ? 0 : settings->ram_size]);
        FntPrint(0, "  %s Auto-boot:   %s (%ds)\n",
                 selected == 5 ? ">" : " ",
                 settings->auto_boot ? "ON" : "OFF",
                 settings->auto_boot_timeout);
        FntPrint(0, "\n");
        FntPrint(0, "  %s Save Settings\n",
                 selected == 6 ? ">" : " ");
        FntPrint(0, "  %s Back\n",
                 selected == 7 ? ">" : " ");

        if (changed)
            FntPrint(0, "\n  * Unsaved changes\n");

        /* Show resulting cmdline */
        {
            char cmd[256];
            settings_build_cmdline(settings, cmd, sizeof(cmd));
            FntPrint(0, "\n  Cmdline:\n  %s\n", cmd);
        }

        FntPrint(0, "\n  D-pad:Nav  L/R:Change  Start:Select\n");
        FntFlush(-1);
        menu_swap_buffers();

        uint16_t pressed = read_pad_once();

        if (pressed & PAD_UP) {
            if (selected > 0) selected--;
        }
        if (pressed & PAD_DOWN) {
            if (selected < num_items - 1) selected++;
        }
        if (pressed & PAD_RIGHT) {
            changed = 1;
            switch (selected) {
                case 0:
                    settings->boot_source = (settings->boot_source + 1) % 4;
                    break;
                case 1:
                    settings->root_device = (settings->root_device + 1) % ROOT_DEV_COUNT;
                    break;
                case 2:
                    settings->init_program = (settings->init_program + 1) % 3;
                    break;
                case 3:
                    settings->console_mode = (settings->console_mode + 1) % 3;
                    break;
                case 4:
                    settings->ram_size = (settings->ram_size + 1) % 4;
                    break;
                case 5:
                    settings->auto_boot = !settings->auto_boot;
                    break;
            }
        }
        if (pressed & PAD_LEFT) {
            changed = 1;
            switch (selected) {
                case 0:
                    settings->boot_source = (settings->boot_source + 3) % 4;
                    break;
                case 1:
                    settings->root_device = (settings->root_device + ROOT_DEV_COUNT - 1) % ROOT_DEV_COUNT;
                    break;
                case 2:
                    settings->init_program = (settings->init_program + 2) % 3;
                    break;
                case 3:
                    settings->console_mode = (settings->console_mode + 2) % 3;
                    break;
                case 4:
                    settings->ram_size = (settings->ram_size + 3) % 4;
                    break;
                case 5:
                    settings->auto_boot = !settings->auto_boot;
                    break;
            }
        }
        if (pressed & (PAD_START | PAD_SELECT | PAD_CROSS | PAD_CIRCLE)) {
            if (selected == 6) {
                /* Save */
                if (settings_save(settings) == 0) {
                    changed = 0;
                }
            } else if (selected == 7) {
                return;
            }
        }
    }
}
