/*
 * kernel.c — Kernel loading and launching for Blackroo bootloader
 *
 * Handles the final steps before jumping to the Linux kernel:
 * configures RAM, disables interrupts, flushes cache, jumps.
 *
 * Reference: psx-spx Memory Control, R3000A architecture
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxpad.h>
#include <psxcd.h>
#include <psxapi.h>
#include "kernel.h"
#include "settings.h"
#include "hwinfo.h"
#include "menu.h"

extern uint8_t pad_buff[2][34];
extern hw_info_t hw;
extern blackroo_settings_t cfg;

/* PS1 hardware registers */
#define RAM_SIZE_REG (*(volatile uint32_t *)0x1F801060)
#define INT_STAT     (*(volatile uint16_t *)0x1F801070)
#define INT_MASK     (*(volatile uint16_t *)0x1F801074)
#define DMA_DPCR     (*(volatile uint32_t *)0x1F8010F0)
#define DMA_DICR     (*(volatile uint32_t *)0x1F8010F4)

/* RAM register values */
#define RAM_REG_2MB  0x0888
#define RAM_REG_4MB  0x0988
#define RAM_REG_8MB  0x0B88

uint32_t kernel_load_psexe(const void *data, uint32_t size) {
    const psexe_header_t *hdr = (const psexe_header_t *)data;

    /* Verify magic */
    if (memcmp(hdr->magic, "PS-X EXE", 8) != 0) return 0;

    /* Copy kernel data to load address */
    memcpy((void *)hdr->t_addr,
           (const uint8_t *)data + 2048, /* skip 2KB header */
           hdr->t_size);

    return hdr->pc0;
}

void kernel_launch(uint32_t entry_point, uint32_t ram_kb,
                   const char *cmdline) {
    typedef void (*kernel_entry_t)(void);

    /* 1. Configure RAM size register */
    switch (ram_kb) {
        case 8192: RAM_SIZE_REG = RAM_REG_8MB; break;
        case 4096: RAM_SIZE_REG = RAM_REG_4MB; break;
        default:   RAM_SIZE_REG = RAM_REG_2MB; break;
    }

    /* 2. Disable all interrupts */
    INT_MASK = 0;
    INT_STAT = 0;

    /* 3. Stop all DMA. 0x07654321 is the *enable-everything* default, which
     * is what this used to write despite the comment. Zero really does stop
     * the channels; the kernel re-enables what it needs. */
    DMA_DPCR = 0;
    DMA_DICR = 0;

    /* 4. Place kernel command line at the BIOS argument area (0x80000180).
     *
     * The kernel picks this up in arch/mipsnommu/ps/prom/cmdline.c, but
     * only when it is tagged with the magic "BRCL" — otherwise leftover
     * RAM contents after a warm boot could be mistaken for a command
     * line. Layout: "BRCL" followed by a NUL-terminated string.
     *
     * NOTE: not yet rebuilt — this machine has no PSn00bSDK/Docker any
     * more (see docs/19). The kernel side is already live. */
    if (cmdline) {
        char *arg = (char *)0x80000180;

        arg[0] = 'B'; arg[1] = 'R'; arg[2] = 'C'; arg[3] = 'L';
        strncpy(arg + 4, cmdline, 251);
        arg[255] = '\0';
    }

    /*
     * 5. Flush the instruction cache — via the BIOS, not by hand.
     *
     * The previous version isolated the cache with inline asm and zeroed
     * 4 KB from a loop that was itself executing out of cached KSEG0. On
     * real hardware that is not safe: with IsC set, instruction fetches
     * come from the isolated cache, so the loop can fetch garbage and die.
     * It also did:
     *
     *     mfc0 $t0, $12      // read SR
     *     lui  $t0, 1        // ...and immediately discard it
     *     mtc0 $t0, $12      // SR := IsC only, clearing IEc/BEV/everything
     *
     * which hands the kernel a mangled status register even when it works.
     * Symptom on hardware: kloader vanished (no beacons) with the screen
     * frozen on its last frame and the kernel never running — it crashed
     * here, before the jump.
     *
     * FlushCache() is a BIOS call (A0h:44h) written for this CPU, and the
     * BIOS vectors are still installed while we run. The kernel's own
     * head.S sets SR, I_MASK, I_STAT, DPCR and DICR immediately on entry
     * anyway, so we do not need to pre-arrange any of that for it.
     */
    FlushCache();

    /* 6. Jump to kernel entry point — never returns */
    ((kernel_entry_t)entry_point)();

    /* Should never reach here */
    while(1);
}

/* ------------------------------------------------------------------ */
/* CD-ROM boot: read PS-EXE from ISO9660 and launch                  */
/* ------------------------------------------------------------------ */

static void cdrom_wait_button(void) {
    while (1) {
        VSync(0);
        PADTYPE *pad = (PADTYPE *)pad_buff[0];
        uint16_t btn = (pad->stat == 0) ? ~pad->btn : 0;
        if (btn & (PAD_START | PAD_SELECT | PAD_CROSS | PAD_CIRCLE)) {
            while (btn & (PAD_START | PAD_SELECT | PAD_CROSS | PAD_CIRCLE)) {
                VSync(0);
                pad = (PADTYPE *)pad_buff[0];
                btn = (pad->stat == 0) ? ~pad->btn : 0;
            }
            return;
        }
    }
}

void cdrom_boot(const char *filename, int profile) {
    CdlFILE file;
    psexe_header_t hdr;
    uint8_t sector[2048];
    uint8_t *dst;
    uint32_t remaining, offset;
    int i;

    /* Show status */
    FntPrint(0, "\n  Initializing CD-ROM...\n");
    FntFlush(-1);
    menu_swap_buffers();

    /* Initialize CD subsystem */
    CdInit();

    /* Set read speed: 2x (double speed) */
    {
        uint8_t mode = CdlModeSpeed;
        CdControl(CdlSetmode, &mode, 0);
    }

    /* Search for file on disc */
    FntPrint(0, "  Searching: %s\n", filename);
    FntFlush(-1);
    menu_swap_buffers();

    if (!CdSearchFile(&file, (char *)filename)) {
        FntPrint(0, "\n  FILE NOT FOUND: %s\n", filename);
        FntPrint(0, "\n  Make sure the CD-R contains\n");
        FntPrint(0, "  an ISO9660 filesystem with\n");
        FntPrint(0, "  the kernel PS-EXE file.\n\n");
        FntPrint(0, "  [Start] Back\n");
        FntFlush(-1);
        menu_swap_buffers();
        cdrom_wait_button();
        return;
    }

    FntPrint(0, "  Found: %d bytes\n", file.size);
    FntFlush(-1);
    menu_swap_buffers();

    if (file.size < 2048) {
        FntPrint(0, "\n  File too small for PS-EXE!\n");
        FntPrint(0, "\n  [Start] Back\n");
        FntFlush(-1);
        menu_swap_buffers();
        cdrom_wait_button();
        return;
    }

    /* Read first sector (PS-EXE header) */
    CdControl(CdlSetloc, (uint8_t *)&file.pos, 0);
    CdRead(1, (uint32_t *)sector, CdlModeSpeed);
    CdReadSync(0, 0);

    /* Parse PS-EXE header */
    memcpy(&hdr, sector, sizeof(hdr));

    if (memcmp(hdr.magic, "PS-X EXE", 8) != 0) {
        FntPrint(0, "\n  Not a valid PS-EXE file!\n");
        FntPrint(0, "  Missing PS-X EXE magic.\n\n");
        FntPrint(0, "  [Start] Back\n");
        FntFlush(-1);
        menu_swap_buffers();
        cdrom_wait_button();
        return;
    }

    FntPrint(0, "  Entry:  0x%08X\n", hdr.pc0);
    FntPrint(0, "  Load:   0x%08X\n", hdr.t_addr);
    FntPrint(0, "  Size:   %d bytes\n", hdr.t_size);
    FntPrint(0, "  Loading...\n");
    FntFlush(-1);
    menu_swap_buffers();

    /*
     * PS-EXE layout on disc:
     *   Sector 0: 2048 bytes of header (we only need first ~64 bytes)
     *   Sector 1+: kernel data (t_size bytes)
     *
     * The 2048-byte header is padded, data starts at file offset 2048.
     */

    dst = (uint8_t *)hdr.t_addr;
    remaining = hdr.t_size;
    offset = 0;

    /* Skip header — advance position by 1 sector */
    {
        CdlLOC loc;
        int sector_num = CdPosToInt(&file.pos) + 1;
        CdIntToPos(sector_num, &loc);
        CdControl(CdlSetloc, (uint8_t *)&loc, 0);
    }

    /* Read data in chunks of sectors */
    while (remaining > 0) {
        uint32_t chunk = (remaining > 2048) ? 2048 : remaining;

        CdRead(1, (uint32_t *)sector, CdlModeSpeed);
        CdReadSync(0, 0);

        memcpy(dst + offset, sector, chunk);
        offset += chunk;
        remaining -= chunk;

        /* Advance to next sector */
        {
            CdlLOC loc;
            int sector_num = CdPosToInt(&file.pos) + 1 + (offset / 2048);
            CdIntToPos(sector_num, &loc);
            CdControl(CdlSetloc, (uint8_t *)&loc, 0);
        }
    }

    FntPrint(0, "  Loaded %d bytes OK\n", hdr.t_size);
    FntPrint(0, "  Launching kernel...\n");
    FntFlush(-1);
    menu_swap_buffers();

    /*
     * Launch with the command line built from the saved settings, rather than
     * a string frozen into this function.
     *
     * That is what carries the display mode across the handoff, so the kernel
     * comes up in whatever the menu was left on - the whole point of being
     * able to walk the modes with L2/R2.
     *
     * Note this drops the `brmon` that used to be hard-coded here, so a CD
     * boot now runs straight through to userspace instead of stopping in the
     * monitor. Put it back for a debugging session via the custom command
     * line in System Settings.
     */
    {
        char cmdline[192];
        blackroo_settings_t boot_cfg = cfg;

        /*
         * Override the saved settings with the profile for this entry.
         *
         * Not a suggestion and not a default: the disc knows what each of its
         * kernels needs, and a user should not have to visit System Settings
         * and get three fields right before a disc will boot the way it says
         * on the menu. Only a custom command line beats this - see
         * settings_build_cmdline().
         */
        switch (profile) {
            case CDBOOT_PROFILE_CDROOT:
                boot_cfg.root_device  = ROOT_DEV_CDROM;
                boot_cfg.init_program = INIT_BIN_SH;
                boot_cfg.console_mode = CONSOLE_GPU_SERIAL;
                break;
            case CDBOOT_PROFILE_RAMDISK:
                boot_cfg.root_device  = ROOT_DEV_RAM;
                boot_cfg.init_program = INIT_BIN_SH;
                boot_cfg.console_mode = CONSOLE_GPU_SERIAL;
                break;
            default:
                break;      /* CDBOOT_PROFILE_SETTINGS: leave cfg alone */
        }

        settings_build_cmdline(&boot_cfg, cmdline, sizeof(cmdline));

        /* Show it. A boot that goes wrong from here is nearly impossible to
         * diagnose from the television, and this is the one line that says
         * what was actually asked for. */
        FntPrint(0, "\n  %s\n", cmdline);
        if (cfg.cmdline[0] != '\0')
            FntPrint(0, "  (custom cmdline from Settings)\n");
        FntFlush(-1);
        menu_swap_buffers();

        kernel_launch(hdr.pc0, hw.ram_size_kb, cmdline);
    }
}
