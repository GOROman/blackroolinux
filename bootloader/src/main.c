/*
 * main.c — Blackroo Linux Bootloader for PlayStation 1
 *
 * Native PS-EXE application built with PSn00bSDK.
 * Provides boot menu, serial upload, memory card management,
 * and kernel loading for Blackroo Linux.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 * Built with PSn00bSDK (MIT license) by Lameguy64
 */

#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxspu.h>
#include <psxpad.h>
#include <psxapi.h>

#include "menu.h"
#include "video.h"
#include "logo.h"
#include "hwinfo.h"
#include <stdio.h>
#include "serial.h"
#include "shell.h"
#include "memcard.h"
#include "kernel.h"
#include "settings.h"
#include "pioflash.h"
#include "version.h"

/* Double buffer display */
DISPENV disp[2];
DRAWENV draw[2];
int db = 0;

/* Controller state */
uint8_t pad_buff[2][34];

/* Hardware info */
hw_info_t hw;
blackroo_settings_t cfg;

/*
 * TEMPORARY boot tracing.
 *
 * A hang before the display comes up is invisible: the television keeps
 * showing the BIOS logo and there is nothing to read. These markers go
 * straight out of SIO1 so the host can see exactly how far startup got, in
 * the emulator as well as on the console.
 *
 * Remove once the startup path is trusted again.
 */
/*
 * Set to 1 to trace startup over SIO1 and BIOS stdout. Left in deliberately:
 * a hang before the display comes up is otherwise completely silent, and this
 * is what located the DMA list bug in logo_draw() - the markers showed the
 * first frame dying inside FntFlush(), a function with nothing wrong with it.
 */
#define BOOT_TRACE 0

#if BOOT_TRACE
static void boot_mark(const char *s) {
    static int inited = 0;
    volatile uint16_t *stat = (volatile uint16_t *)0x1F801054;
    volatile uint8_t  *data = (volatile uint8_t  *)0x1F801050;

    if (!inited) {
        serial_init(SERIAL_BAUD_SLOW);
        inited = 1;
    }

    /* Also out of the BIOS stdout, which an emulator can capture even when
     * SIO1 has no CTS to transmit against. */
    printf("%s", s);

    /*
     * Bounded, unlike serial_putchar(), which spins on TXRDY for ever.
     *
     * SIO1 will not transmit unless CTS is asserted - the FTDI adapter does
     * that on the console, and nothing does it in an emulator. A tracing
     * helper that hangs when there is no cable is worse than useless: it
     * turns "where does startup die" into "startup dies here, in the thing
     * asking the question".
     */
    while (*s) {
        long spin = 200000;

        while (!(*stat & 1))
            if (--spin <= 0)
                return;
        *data = (uint8_t)*s++;
    }
}
#else
#define boot_mark(s) do { } while (0)
#endif

/* Which entry of video_modes[] is live. L2/R2 on the main menu move it. */
static int video_idx;

static void init_display(int mode_index) {
    const video_mode_t *m;
    int w, h;

    video_idx = video_wrap_index(mode_index);
    m = &video_modes[video_idx];
    w = m->width;
    h = m->height;

    boot_mark("    d1 ResetGraph\r\n");
    ResetGraph(0);
    boot_mark("    d2 SetVideoMode\r\n");
    SetVideoMode(m->pal ? MODE_PAL : MODE_NTSC);

    /*
     * Two buffers stacked vertically. 240x2 and 256x2 both fit VRAM's 512
     * lines - the latter exactly, which is why the interlaced modes are not
     * on the menu (see video.h).
     */
    SetDefDispEnv(&disp[0], 0, 0, w, h);
    SetDefDispEnv(&disp[1], 0, h, w, h);
    SetDefDrawEnv(&draw[0], 0, h, w, h);
    SetDefDrawEnv(&draw[1], 0, 0, w, h);

    draw[0].isbg = 1;
    draw[1].isbg = 1;
    setRGB0(&draw[0], 0, 0, 32);  /* Dark blue background */
    setRGB0(&draw[1], 0, 0, 32);

    PutDispEnv(&disp[0]);
    PutDrawEnv(&draw[0]);
    SetDispMask(1);

    /*
     * The font lives at VRAM x=960, clear of every buffer above (640 is the
     * widest mode). The text window has to follow the resolution or a 640-wide
     * mode would draw a 320-wide column of text down the left of the screen.
     */
    boot_mark("    d3 FntLoad\r\n");
    FntLoad(960, 0);
    FntOpen(8, 8, w - 16, h - 32, 0, 512);

    /* ResetGraph above wipes VRAM, so the logo goes back after every mode
     * change, not just once at startup. */
    boot_mark("    d4 logo_upload\r\n");
    logo_upload();
    boot_mark("    d5 display ok\r\n");
}

static void swap_buffers(void) {
    DrawSync(0);
    VSync(0);
    db = !db;
    PutDispEnv(&disp[db]);
    PutDrawEnv(&draw[db]);
}

static void do_serial_shell(int fast) {
    uint32_t baud = fast ? SERIAL_BAUD_FAST : SERIAL_BAUD_SLOW;
    shell_run(baud);
}

static void do_boot_card(void) {
    uint16_t prev_btn = 0;
    menu_wait_release();
    while (1) {
        FntPrint(0, "\n");
        FntPrint(0, "  BOOT FROM MEMORY CARD\n");
        FntPrint(0, "  =====================\n\n");
        FntPrint(0, "  Not implemented yet.\n\n");
        FntPrint(0, "  Planned:\n");
        FntPrint(0, "  - Read compressed kernel from\n");
        FntPrint(0, "    memory card RAID\n");
        FntPrint(0, "  - Decompress and launch\n");
        FntPrint(0, "  - Standalone boot (no serial)\n\n");
        FntPrint(0, "  Card 1: %s\n",
                 hw.card1_present ? "Present" : "Empty");
        FntPrint(0, "  Card 2: %s\n\n",
                 hw.card2_present ? "Present" : "Empty");
        FntPrint(0, "  [Start] Back\n");
        FntFlush(-1);
        menu_swap_buffers();

        {
            PADTYPE *pad = (PADTYPE *)pad_buff[0];
            uint16_t btn = (pad->stat == 0) ? ~pad->btn : 0;
            uint16_t pressed = btn & ~prev_btn;
            prev_btn = btn;
            if (pressed & (PAD_START | PAD_SELECT | PAD_CROSS | PAD_CIRCLE)) {
                menu_wait_release();
                return;
            }
        }
    }
}

static void do_boot_cdrom(void) {
    int selected = 0;
    uint16_t prev_btn = 0;

    menu_wait_release();

    while (1) {
        FntPrint(0, "\n");
        FntPrint(0, "  BOOT FROM CD-ROM\n");
        FntPrint(0, "  ================\n\n");
        FntPrint(0, "  Insert a burned CD-R with\n");
        FntPrint(0, "  kernel PS-EXE on data track.\n\n");
        FntPrint(0, "  Wobble groove protection is\n");
        FntPrint(0, "  bypassed — bootloader is\n");
        FntPrint(0, "  already running via exploit.\n\n");
        /* Which file you pick decides where the root filesystem comes from,
         * and picking the wrong one used to fail silently: KERNEL.EXE has an
         * initrd built in, which takes over as root no matter what root= says.
         * Each entry now carries its own known-good root=/init=/console=
         * (CDBOOT_PROFILE_*), so neither depends on System Settings and the
         * menu text is the truth rather than a hint. LINUX.EXE is first
         * because a disc that carries its own root filesystem is the point of
         * this release. */
        FntPrint(0, "  %s LINUX.EXE   - root on this disc\n",
                 selected == 0 ? ">" : " ");
        FntPrint(0, "  %s KERNEL.EXE  - root in a ramdisk\n",
                 selected == 1 ? ">" : " ");
        FntPrint(0, "\n  Each sets its own root=, init=\n");
        FntPrint(0, "  and console=. System Settings\n");
        FntPrint(0, "  is not consulted.\n");
        FntPrint(0, "  %s Back\n",
                 selected == 2 ? ">" : " ");
        FntPrint(0, "\n  D-pad:Navigate  Start:Select\n");
        FntFlush(-1);
        menu_swap_buffers();

        {
            PADTYPE *pad = (PADTYPE *)pad_buff[0];
            uint16_t btn = (pad->stat == 0) ? ~pad->btn : 0;
            uint16_t pressed = btn & ~prev_btn;
            prev_btn = btn;

            if (pressed & PAD_UP) {
                if (selected > 0) selected--;
            }
            if (pressed & PAD_DOWN) {
                if (selected < 2) selected++;
            }
            if (pressed & (PAD_START | PAD_CROSS | PAD_CIRCLE)) {
                if (selected == 2) return;

                if (selected == 0 || selected == 1) {
                    /* Each entry carries the settings its kernel needs, so
                     * neither choice depends on System Settings being right. */
                    /* Order must match the menu drawn above: entry 0 is
                     * LINUX.EXE (root on the disc), entry 1 is KERNEL.EXE. */
                    if (selected == 0)
                        cdrom_boot("LINUX.EXE;1",  CDBOOT_PROFILE_CDROOT);
                    else
                        cdrom_boot("KERNEL.EXE;1", CDBOOT_PROFILE_RAMDISK);
                }
            }
        }
    }
}

int main(void) {
    boot_mark("\r\n[kloader] main\r\n");
    int selected = 0;
    int confirmed;
    char status[64] = "Ready";

    /* Silence SPU */
    SpuInit();

    /* Detect hardware */
    boot_mark("[1] hwinfo_detect\r\n");
    hwinfo_detect(&hw);
    boot_mark("[2] settings_load\r\n");
    settings_load(&cfg);

    /* Initialize display */
    /*
     * Start from the saved mode if there is one, otherwise the console's own
     * region. A saved mode cannot strand anyone: L2/R2 still work even when
     * nothing is visible.
     */
    boot_mark("[3] init_display\r\n");
    init_display(cfg.video_index ? cfg.video_index - 1
                                 : video_default_index(hw.is_pal));

    /* Initialize controller */
    boot_mark("[4] InitPAD\r\n");
    InitPAD(pad_buff[0], 34, pad_buff[1], 34);
    StartPAD();
    ChangeClearPAD(1);

    /* Now that PAD is running, detect memory cards safely */
    memcard_set_pad_active(1);
    /* Slot numbering is (port << 2) | floor now, so port 2's floor A is
     * slot 4, not slot 1. The menu below still calls these "Card 1/2"
     * because they are the two console ports; the Memory Card Manager
     * shows all eight. */
    boot_mark("[5] memcard_detect\r\n");
    hw.card1_present = memcard_detect(0);
    hw.card2_present = memcard_detect(4);
    if (hw.card1_present) hw.card1_size_kb = 128;
    if (hw.card2_present) hw.card2_size_kb = 128;

    boot_mark("[6] main loop\r\n");

    /* Main loop */
    {
    uint16_t prev_video_btn = 0;
    int first_frame = 1;
    while (1) {
        /* Draw menu */
        FntPrint(0, "\n");
        /* The codename has been defined since 0.3 and never shown. It is how
         * a disc identifies itself across the room - a burned CD-R keeps
         * whatever version it was built with, so the menu is the only place
         * you can tell 0.4 from 0.5 without a serial cable. */
        FntPrint(0, "  BLACKROO LINUX v" BLACKROO_VERSION
                    " \"" BLACKROO_CODENAME "\"\n");
        FntPrint(0, "  ==============================\n\n");

        FntPrint(0, "  %s Serial Shell (115200)\n",
                 selected == MENU_SERIAL_SLOW ? ">" : " ");
        FntPrint(0, "  %s Serial Shell (518400)\n",
                 selected == MENU_SERIAL_FAST ? ">" : " ");
        FntPrint(0, "  %s Boot from Memory Card\n",
                 selected == MENU_BOOT_CARD ? ">" : " ");
        FntPrint(0, "  %s Boot from CD-ROM\n",
                 selected == MENU_BOOT_CDROM ? ">" : " ");
        FntPrint(0, "  %s Memory Card Manager\n",
                 selected == MENU_CARD_MANAGER ? ">" : " ");
        FntPrint(0, "  %s System Settings\n",
                 selected == MENU_SETTINGS ? ">" : " ");
        FntPrint(0, "  %s PIO Flash Manager\n",
                 selected == MENU_PIO_FLASH ? ">" : " ");
        FntPrint(0, "  %s Hardware Info\n",
                 selected == MENU_HWINFO ? ">" : " ");

        FntPrint(0, "\n");
        FntPrint(0, "  RAM: %d KB\n", hw.ram_size_kb);
        FntPrint(0, "  Card 1: %s",
                 hw.card1_present ? "Present" : "Empty");
        if (hw.card1_present)
            FntPrint(0, " (%dKB)", hw.card1_size_kb);
        FntPrint(0, "\n");
        FntPrint(0, "  Card 2: %s",
                 hw.card2_present ? "Present" : "Empty");
        if (hw.card2_present)
            FntPrint(0, " (%dKB)", hw.card2_size_kb);
        FntPrint(0, "\n\n");
        FntPrint(0, "  PIO:    %s\n",
                 hw.pio_present ? "Device detected" : "Empty");
        FntPrint(0, "\n  Video:  %s\n", video_modes[video_idx].name);
        FntPrint(0, "\n  %s\n", status);
        FntPrint(0, "\n  D-pad:Navigate  Start:Select\n");
        FntPrint(0, "  L2/R2: Video mode  (boots in this mode)\n");

        /* DEBUG: show raw pad state */
        {
            PADTYPE *dbgpad = (PADTYPE *)pad_buff[0];
            uint16_t dbgbtn = (dbgpad->stat == 0) ? ~dbgpad->btn : 0;
            FntPrint(0, "  pad: stat=%d type=%02X btn=%04X\n",
                     dbgpad->stat, dbgpad->type, dbgbtn);
            FntPrint(0, "  sel=%d conf=%d\n", selected, confirmed);
        }

        if (first_frame) boot_mark("  f1 pre-logo\r\n");
        /*
         * Tux, top right, scaled to the screen: a quarter of the width means
         * he is the same size relative to the menu in every mode. Drawn
         * before the text so that text wins any overlap.
         */
        {
            int w = video_modes[video_idx].width;
            int size = w / 8;

            logo_draw(w - size - 8, 8, size);
        }
        if (first_frame) boot_mark("  f2 post-logo\r\n");

        FntFlush(-1);
        if (first_frame) boot_mark("  f3 post-FntFlush\r\n");
        swap_buffers();
        if (first_frame) { boot_mark("  f4 post-swap\r\n"); first_frame = 0; }

        /*
         * L2/R2 walk the display modes, exactly as the PS2's own loader does,
         * and the kernel boots in whichever one is live.
         *
         * This is why there is no "are you sure" and no revert timer: if a
         * mode shows nothing, the next press moves on. The escape does not
         * depend on being able to read the screen you are escaping from.
         *
         * Handled before menu_input() so a shoulder press cannot also be read
         * as a menu action.
         */
        {
            PADTYPE *vpad = (PADTYPE *)pad_buff[0];
            uint16_t vbtn = (vpad->stat == 0) ? (uint16_t)~vpad->btn : 0;
            uint16_t vpressed = vbtn & ~prev_video_btn;

            prev_video_btn = vbtn;

            if (vpressed & (PAD_L2 | PAD_R2)) {
                init_display(video_idx + ((vpressed & PAD_R2) ? 1 : -1));

                /* Remember it, so the next boot starts here. Stored as
                 * index+1; 0 stays "follow the console's region". */
                cfg.video_index = (uint8_t)(video_idx + 1);

                /* The pad system is re-entered by init_display's ResetGraph,
                 * so let the button come up before reading again. */
                continue;
            }
        }

        /* Handle input */
        confirmed = menu_input(&selected);

        if (confirmed) {
            switch (selected) {
                case MENU_SERIAL_SLOW:
                    do_serial_shell(0);
                    break;
                case MENU_SERIAL_FAST:
                    do_serial_shell(1);
                    break;
                case MENU_BOOT_CARD:
                    do_boot_card();
                    break;
                case MENU_BOOT_CDROM:
                    do_boot_cdrom();
                    break;
                case MENU_CARD_MANAGER:
                    memcard_manager_menu();
                    break;
                case MENU_SETTINGS:
                    settings_menu(&cfg);
                    break;
                case MENU_PIO_FLASH:
                    pioflash_manager_menu();
                    break;
                case MENU_HWINFO:
                    hwinfo_display(&hw);
                    break;
            }
        }
    }
    }

    return 0;
}
