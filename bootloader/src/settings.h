/*
 * settings.h — Persistent boot settings for Blackroo bootloader
 *
 * Settings are stored on memory card block 1 (after Blackroo header).
 * They persist across power cycles.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */
#ifndef BLACKROO_SETTINGS_H
#define BLACKROO_SETTINGS_H

#include <stdint.h>

#define SETTINGS_MAGIC  0x424C4B53  /* "BLKS" */
#define SETTINGS_VERSION 1

/* Boot source options */
#define BOOT_SRC_SERIAL_SLOW  0
#define BOOT_SRC_SERIAL_FAST  1
#define BOOT_SRC_CARD         2
#define BOOT_SRC_CDROM        3

/* Root device options */
#define ROOT_DEV_CARD_SINGLE  0  /* /dev/bu0 */
#define ROOT_DEV_CARD_RAID    1  /* /dev/bul */
#define ROOT_DEV_RAM          2  /* /dev/ram0 */
#define ROOT_DEV_CDROM        3  /* /dev/psxcd - ROOT.IMG on this disc */
#define ROOT_DEV_COUNT        4  /* keep in step with root_dev_names[] */

/* Init program options */
#define INIT_BIN_SH     0  /* /bin/sh */
#define INIT_LINUXRC    1  /* /linuxrc */
#define INIT_SBIN_INIT  2  /* /sbin/init */

/* Console options */
#define CONSOLE_GPU_SERIAL  0  /* Both GPU + serial */
#define CONSOLE_GPU_ONLY    1  /* GPU only */
#define CONSOLE_SERIAL_ONLY 2  /* Serial only */

typedef struct {
    uint32_t magic;             /* SETTINGS_MAGIC */
    uint32_t version;           /* SETTINGS_VERSION */

    /* Boot configuration */
    uint8_t  boot_source;       /* BOOT_SRC_* */
    uint8_t  root_device;       /* ROOT_DEV_* */
    uint8_t  init_program;      /* INIT_* */
    uint8_t  console_mode;      /* CONSOLE_* */

    /* Hardware configuration */
    uint8_t  ram_size;          /* 0=auto, 2=2MB, 4=4MB, 8=8MB */
    uint8_t  video_mode;        /* 0=auto, 1=PAL, 2=NTSC */
    uint16_t serial_baud;       /* 0=115200, 1=518400 */

    /* Boot behavior */
    uint8_t  auto_boot;         /* 1=auto-boot after timeout */
    uint8_t  auto_boot_timeout; /* Seconds (1-30) */
    uint8_t  boot_count;        /* Incremented each boot */
    uint8_t  last_boot_ok;      /* 1=last boot succeeded */

    /* Kernel command line override (if non-empty, replaces default) */
    char     cmdline[64];

    /*
     * Selected display mode, as video_modes[] index PLUS ONE - so that 0,
     * which is what every previously-saved settings block already holds in
     * this byte, keeps meaning "use the console's own region". Taken from the
     * padding rather than appended, so no offset before it moves and
     * SETTINGS_VERSION need not change.
     */
    uint8_t  video_index;

    /* Padding to 128 bytes (one memory card block) */
    uint8_t  reserved[43]; /* pad to 128 bytes total */

} __attribute__((packed)) blackroo_settings_t;

/* Load settings from card (slot 0, block 1). Returns defaults if not found. */
void settings_load(blackroo_settings_t *settings);

/* Save settings to card (slot 0, block 1) */
int  settings_save(const blackroo_settings_t *settings);

/* Reset to defaults */
void settings_default(blackroo_settings_t *settings);

/* Build kernel command line from settings */
void settings_build_cmdline(const blackroo_settings_t *settings, char *buf, int bufsize);

/* Settings menu — interactive editor */
void settings_menu(blackroo_settings_t *settings);

#endif
