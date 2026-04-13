/*
 * menu.h — GPU menu rendering for Blackroo bootloader
 * Attribution: New Blackroo work (2026, GPL v2)
 */
#ifndef BLACKROO_MENU_H
#define BLACKROO_MENU_H

/* Menu item IDs */
#define MENU_SERIAL_SLOW    0
#define MENU_SERIAL_FAST    1
#define MENU_BOOT_CARD      2
#define MENU_BOOT_CDROM     3
#define MENU_CARD_MANAGER   4
#define MENU_SETTINGS       5
#define MENU_PIO_FLASH      6
#define MENU_HWINFO         7
#define MENU_COUNT          8

void menu_init(void);
void menu_draw(int selected, const char *status);
int  menu_input(int *selected);  /* returns 1 if confirmed */
void menu_wait_release(void);    /* wait for all buttons released */
void menu_swap_buffers(void);    /* swap display double-buffer — call this instead of DrawSync+VSync */

/* Text drawing helpers */
void draw_text(int x, int y, const char *text);
void draw_text_highlight(int x, int y, const char *text);
void clear_screen(void);

#endif
