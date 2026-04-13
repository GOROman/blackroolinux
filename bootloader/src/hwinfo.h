/*
 * hwinfo.h — Hardware detection for Blackroo bootloader
 * Attribution: New Blackroo work (2026, GPL v2)
 */
#ifndef BLACKROO_HWINFO_H
#define BLACKROO_HWINFO_H

#include <stdint.h>

typedef struct {
    /* CPU */
    uint32_t cop0_prid;         /* COP0 PRId register (CPU type + revision) */
    uint8_t  cpu_type;          /* 0x03=R3000A, 0x07=IDT variant */
    uint8_t  cpu_rev;           /* Revision level */

    /* RAM */
    uint32_t ram_size_kb;       /* Detected RAM in KB */
    uint32_t ram_reg;           /* RAM_SIZE register value */

    /* Video */
    int      is_pal;            /* 1=PAL, 0=NTSC */
    uint32_t gpu_stat;          /* Raw GPU status register */
    int      screen_w;          /* Display width */
    int      screen_h;          /* Display height */

    /* Memory Cards */
    int      card1_present;
    int      card2_present;
    int      card1_blackroo;    /* 1=Blackroo formatted */
    int      card2_blackroo;
    uint32_t card1_size_kb;
    uint32_t card2_size_kb;
    uint32_t card1_serial;      /* Blackroo card serial number */
    uint32_t card2_serial;
    int      card1_seq;         /* RAID sequence number */
    int      card2_seq;

    /* BIOS */
    uint32_t bios_date;         /* BIOS date string pointer */
    char     bios_version[16];  /* BIOS version string */

    /* Bus / Peripherals */
    int      pio_present;       /* Parallel I/O expansion port */
    uint32_t pio_id;            /* PIO device ID (if present) */
    int      sio_present;       /* Serial port detected */

    /* DMA */
    uint32_t dma_dpcr;          /* DMA primary control register */

    /* Timers */
    uint32_t sysclock_hz;       /* System clock (33868800) */

} hw_info_t;

void hwinfo_detect(hw_info_t *info);
void hwinfo_display(const hw_info_t *info);

/* Wait for X button press, returns when pressed */
void wait_for_confirm(void);

#endif
