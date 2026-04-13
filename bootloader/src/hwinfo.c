/*
 * hwinfo.c — Full hardware detection for Blackroo bootloader
 *
 * Detects: CPU type/revision, RAM size, video mode, memory cards,
 * BIOS version, expansion port, serial port, DMA status.
 *
 * Reference: psx-spx hardware docs, Runix documentation
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxapi.h>
#include <psxpad.h>
#include "hwinfo.h"
#include "memcard.h"
#include "menu.h"

/* PS1 hardware registers */
#define RAM_SIZE_REG    (*(volatile uint32_t *)0x1F801060)
#define GPU_STAT_REG    (*(volatile uint32_t *)0x1F801814)
#define DMA_DPCR_REG    (*(volatile uint32_t *)0x1F8010F0)
#define PIO_BASE        (*(volatile uint32_t *)0x1F000000)
#define SIO1_STAT       (*(volatile uint16_t *)0x1F801054)

/* BIOS ROM locations */
#define BIOS_DATE_ADDR  ((const char *)0xBFC0012C)
#define BIOS_BASE       ((const char *)0xBFC00000)

extern uint8_t pad_buff[2][34];

/*
 * Detect CPU type and revision from COP0 PRId register
 * PRId format: bits 15:8 = Imp (type), bits 7:0 = Rev
 */
static uint32_t read_prid(void) {
    uint32_t prid;
    __asm__ volatile("mfc0 %0, $15" : "=r"(prid));
    return prid;
}

/*
 * Detect RAM size by writing unique patterns at power-of-2
 * boundaries via uncached KSEG1 addresses.
 */
static void detect_ram(hw_info_t *info) {
    volatile uint32_t *base   = (volatile uint32_t *)0xA0000000;
    volatile uint32_t *at_2mb = (volatile uint32_t *)0xA0200000;
    volatile uint32_t *at_4mb = (volatile uint32_t *)0xA0400000;

    /* Configure for max (8MB) to probe full range */
    RAM_SIZE_REG = 0x0B88;

    *base   = 0xDEADBEEF;
    *at_2mb = 0xCAFEBABE;
    *at_4mb = 0x12345678;

    if (*base == 0x12345678 || *base == 0xCAFEBABE) {
        info->ram_size_kb = 2048;
        info->ram_reg = 0x0888;
    } else if (*at_4mb == 0x12345678 && *at_2mb == 0xCAFEBABE) {
        info->ram_size_kb = 8192;
        info->ram_reg = 0x0B88;
    } else if (*at_2mb == 0xCAFEBABE) {
        info->ram_size_kb = 4096;
        info->ram_reg = 0x0988;
    } else {
        info->ram_size_kb = 2048;
        info->ram_reg = 0x0888;
    }

    RAM_SIZE_REG = info->ram_reg;
}

/*
 * Detect video mode and display parameters from GPU status
 */
static void detect_video(hw_info_t *info) {
    info->gpu_stat = GPU_STAT_REG;
    info->is_pal = (info->gpu_stat & (1 << 20)) ? 1 : 0;
    info->screen_w = 320;
    info->screen_h = info->is_pal ? 256 : 240;
}

/*
 * Read BIOS version string from ROM
 */
static void detect_bios(hw_info_t *info) {
    /* The BIOS date is typically at 0xBFC0012C as ASCII */
    strncpy(info->bios_version, BIOS_DATE_ADDR, 15);
    info->bios_version[15] = '\0';

    /* Try to find a more specific version string */
    /* Search BIOS ROM for "System ROM Version" or similar */
    info->bios_date = (uint32_t)BIOS_DATE_ADDR;
}

/*
 * Detect expansion port (PIO) at 0x1F000000
 */
static void detect_pio(hw_info_t *info) {
    /* Try reading the PIO base — if a device responds, something is there.
     * Action Replay / GameShark / Xplorer live here.
     * A read from empty PIO typically returns 0xFFFFFFFF. */
    volatile uint32_t *pio = (volatile uint32_t *)0x1F000000;
    uint32_t val = *pio;

    if (val != 0xFFFFFFFF && val != 0x00000000) {
        info->pio_present = 1;
        info->pio_id = val;
    } else {
        info->pio_present = 0;
        info->pio_id = 0;
    }
}

/*
 * Detect serial port (SIO1) status
 */
static void detect_serial(hw_info_t *info) {
    /* Check if SIO1 status register is readable and has valid bits */
    uint16_t stat = SIO1_STAT;
    info->sio_present = (stat != 0xFFFF) ? 1 : 0;
}

/*
 * Detect memory cards using BIOS card functions
 * NOTE: Commented out — must not run before InitPAD/StartPAD or
 * real hardware freezes. Card detection moved to main() after pad init.
 */
#if 0
static void detect_cards(hw_info_t *info) {
    blackroo_card_header_t hdr;

    info->card1_present = memcard_detect(0);
    info->card2_present = memcard_detect(1);

    if (info->card1_present) {
        info->card1_size_kb = 128;
        if (memcard_read_header(0, &hdr) == 0 && hdr.id == BU_ID) {
            info->card1_blackroo = 1;
            info->card1_serial = hdr.serial;
            info->card1_seq = hdr.number;
        }
    }

    if (info->card2_present) {
        info->card2_size_kb = 128;
        if (memcard_read_header(1, &hdr) == 0 && hdr.id == BU_ID) {
            info->card2_blackroo = 1;
            info->card2_serial = hdr.serial;
            info->card2_seq = hdr.number;
        }
    }
}
#endif

void hwinfo_detect(hw_info_t *info) {
    memset(info, 0, sizeof(*info));

    info->sysclock_hz = 33868800;

    /* CPU detection */
    info->cop0_prid = read_prid();
    info->cpu_type = (info->cop0_prid >> 8) & 0xFF;
    info->cpu_rev = info->cop0_prid & 0xFF;

    detect_ram(info);
    detect_video(info);
    detect_bios(info);
    detect_pio(info);
    detect_serial(info);
    /* detect_cards(info); — deferred to main() after PAD init */

    info->dma_dpcr = DMA_DPCR_REG;
}

/* Wait for any confirm button (Start, Select, X, O) */
void wait_for_confirm(void) {
    PADTYPE *pad;
    uint16_t prev = 0;
    uint16_t confirm_mask = PAD_START | PAD_SELECT | PAD_CROSS | PAD_CIRCLE;

    while (1) {
        VSync(0);
        pad = (PADTYPE *)pad_buff[0];
        if (pad->stat == 0) {
            uint16_t btn = ~pad->btn;
            uint16_t pressed = btn & ~prev;
            prev = btn;
            if (pressed & confirm_mask)
                return;
        }
    }
}

void hwinfo_display(const hw_info_t *info) {
    const char *cpu_name;
    uint16_t prev_btn = 0;

    menu_wait_release();

    switch (info->cpu_type) {
        case 0x03: cpu_name = "R3000A (Sony CXD8530)"; break;
        case 0x07: cpu_name = "R3000A (IDT 3041)"; break;
        default:   cpu_name = "Unknown MIPS"; break;
    }

    while (1) {
        FntPrint(0, "\n");
        FntPrint(0, "  HARDWARE INFORMATION\n");
        FntPrint(0, "  ====================\n\n");

        /* CPU */
        FntPrint(0, "  CPU:     %s\n", cpu_name);
        FntPrint(0, "  PRId:    0x%04X (type=%02X rev=%02X)\n",
                 info->cop0_prid, info->cpu_type, info->cpu_rev);
        FntPrint(0, "  Clock:   33.8688 MHz\n");
        FntPrint(0, "  Cache:   4KB I-cache, 1KB scratch\n");
        FntPrint(0, "  TLB:     64 entries (4KB pages)\n\n");

        /* Memory */
        FntPrint(0, "  RAM:     %d KB (reg=0x%04X)\n",
                 info->ram_size_kb, info->ram_reg);

        /* Video */
        FntPrint(0, "  Video:   %s %dx%d\n",
                 info->is_pal ? "PAL" : "NTSC",
                 info->screen_w, info->screen_h);
        FntPrint(0, "  GPU:     0x%08X\n", info->gpu_stat);

        /* BIOS */
        FntPrint(0, "  BIOS:    %s\n", info->bios_version);

        /* Cards */
        FntPrint(0, "  Card 1:  %s",
                 info->card1_present ? "Present" : "Empty");
        if (info->card1_blackroo)
            FntPrint(0, " [Blackroo #%d]", info->card1_seq);
        FntPrint(0, "\n");
        FntPrint(0, "  Card 2:  %s",
                 info->card2_present ? "Present" : "Empty");
        if (info->card2_blackroo)
            FntPrint(0, " [Blackroo #%d]", info->card2_seq);
        FntPrint(0, "\n");

        /* Peripherals */
        FntPrint(0, "  Serial:  %s\n",
                 info->sio_present ? "Ready" : "N/A");
        FntPrint(0, "  PIO:     %s",
                 info->pio_present ? "Device" : "Empty");
        if (info->pio_present)
            FntPrint(0, " (ID:0x%08X)", info->pio_id);
        FntPrint(0, "\n");
        FntPrint(0, "  DMA:     0x%08X\n", info->dma_dpcr);

        FntPrint(0, "\n  [Start] Back\n");
        FntFlush(-1);
        menu_swap_buffers();

        /* Check for fresh button press (edge-triggered) */
        {
            PADTYPE *pad = (PADTYPE *)pad_buff[0];
            uint16_t btn = (pad->stat == 0) ? ~pad->btn : 0;
            uint16_t pressed = btn & ~prev_btn;
            uint16_t confirm = PAD_START | PAD_SELECT | PAD_CROSS | PAD_CIRCLE;
            prev_btn = btn;
            if (pressed & confirm) {
                menu_wait_release();
                return;
            }
        }
    }
}
