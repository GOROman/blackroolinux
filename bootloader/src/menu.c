/*
 * menu.c — Controller input handling for bootloader menu
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <psxgpu.h>
#include <psxpad.h>
#include "menu.h"
extern uint8_t pad_buff[2][34];

static uint16_t prev_buttons = 0xFFFF;

int menu_input(int *selected) {
    PADTYPE *pad = (PADTYPE *)pad_buff[0]; /* from main.c extern */
    uint16_t buttons;
    int confirmed = 0;

    /* Check pad is connected and digital */
    if (pad->stat != 0) return 0;
    if ((pad->type != 0x04) && (pad->type != 0x05) &&
        (pad->type != 0x07)) return 0;

    buttons = ~pad->btn;  /* Active high */

    /* D-pad up */
    if ((buttons & PAD_UP) && !(prev_buttons & PAD_UP)) {
        if (*selected > 0) (*selected)--;
    }
    /* D-pad down */
    if ((buttons & PAD_DOWN) && !(prev_buttons & PAD_DOWN)) {
        if (*selected < MENU_COUNT - 1) (*selected)++;
    }
    /* Start button = confirm (X/O/Triangle/Square not detected in DuckStation) */
    if ((buttons & PAD_START) && !(prev_buttons & PAD_START)) {
        confirmed = 1;
    }
    /* Also accept Select as alternate confirm */
    if ((buttons & PAD_SELECT) && !(prev_buttons & PAD_SELECT)) {
        confirmed = 1;
    }
    /* Keep X/O as backup in case they work on real hardware */
    if ((buttons & PAD_CROSS) && !(prev_buttons & PAD_CROSS)) {
        confirmed = 1;
    }
    if ((buttons & PAD_CIRCLE) && !(prev_buttons & PAD_CIRCLE)) {
        confirmed = 1;
    }

    prev_buttons = buttons;
    return confirmed;
}

/*
 * Swap display double-buffers. ALL screens must call this
 * instead of raw DrawSync+VSync to actually show their output.
 * Without this, drawing goes to the hidden buffer and nothing
 * appears on the TV.
 */
extern DISPENV disp[2];
extern DRAWENV draw[2];
extern int db;

void menu_swap_buffers(void) {
    DrawSync(0);
    VSync(0);
    db = !db;
    PutDispEnv(&disp[db]);
    PutDrawEnv(&draw[db]);
}

/*
 * Wait until all buttons are released.
 * Call this at the start of any sub-screen to prevent the
 * confirming button press from the main menu from triggering
 * an immediate action or exit in the sub-screen.
 */
void menu_wait_release(void) {
    PADTYPE *pad;
    uint16_t btn;

    /* Wait for at least one frame with no buttons held */
    while (1) {
        VSync(0);
        pad = (PADTYPE *)pad_buff[0];
        btn = (pad->stat == 0) ? ~pad->btn : 0;
        if (btn == 0) break;
    }
    /* Extra frame to be safe */
    VSync(0);
}
