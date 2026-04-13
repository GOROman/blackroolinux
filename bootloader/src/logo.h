/*
 * logo.h — the Linux boot logo on the kloader menu
 *
 * Larry Ewing's Tux, taken straight from this tree's own
 * blackroo/include/linux/linux_logo.h - the logo of the kernel kloader
 * boots. Converted by bootloader/tools/mklogo.py into logo_data.c.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 *              logo artwork (C) 1996 Larry Ewing
 */
#ifndef BLACKROO_LOGO_H
#define BLACKROO_LOGO_H

#define LOGO_W          80
#define LOGO_H          80
#define LOGO_CLUT_LEN   256

/*
 * Where the logo lives in VRAM.
 *
 * VRAM is 1024x512. The framebuffers occupy x 0..639 at the very widest mode
 * and both halves of the height, and PSn00bSDK's debug font is loaded at
 * x=960 - so x 640..959 is the only region free in every mode, and that is
 * what these use. Nothing here needs to move when the display mode changes.
 *
 * At 8bpp the GPU packs two texels per VRAM halfword, so 80 texels of width
 * occupy 40 VRAM columns: x 640..679, y 0..79.
 *
 * The CLUT is 256 entries on one line. Its X must be a multiple of 16 - 640
 * is - and it sits below the pixels rather than beside them so that adding a
 * second image later is a matter of moving one constant.
 */
#define LOGO_VRAM_X     640
#define LOGO_VRAM_Y     0
#define LOGO_CLUT_X     640
#define LOGO_CLUT_Y     100

extern const unsigned short logo_clut[LOGO_CLUT_LEN];
extern const unsigned char  logo_pix[LOGO_W * LOGO_H];

/* Upload the logo to VRAM. Call once, after the display is up, and again
 * after any ResetGraph - a mode change wipes VRAM. */
void logo_upload(void);

/* Draw it at (x, y), scaled to size x size pixels, in the current buffer. */
void logo_draw(int x, int y, int size);

#endif
