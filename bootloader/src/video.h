/*
 * video.h — selectable PS1 display modes for the Blackroo bootloader
 *
 * The PS2's own loader lets you walk video modes with L2/R2 until a picture
 * appears, and boots in whatever mode you left it on. That idea is worth
 * copying exactly, because it is self-recovering: if a mode produces no
 * picture there is no dialog you need to be able to read in order to escape,
 * you just press again. No confirmation, no timeout, no way to strand
 * yourself in a mode your television will not display.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */
#ifndef BLACKROO_VIDEO_H
#define BLACKROO_VIDEO_H

/*
 * What the hardware can actually do — GP1(08h), confirmed against
 * PSn00bSDK's GPU_DispFlags enum:
 *
 *   bits 0-1  horizontal   0=256  1=320  2=512  3=640
 *   bit  2    vertical     0=240-class  1=480-class (interlaced only)
 *   bit  3    video mode   0=NTSC 60Hz  1=PAL 50Hz
 *   bit  4    colour       0=15bit  1=24bit
 *   bit  5    interlace
 *   bit  6    horizontal 2 overrides bits 0-1
 *
 * Three of those are deliberately not offered here:
 *
 *   - 24-bit colour is display-only. The GPU cannot draw into a 24-bit
 *     buffer, so it is for full-motion video and useless for a menu.
 *   - The bit-6 width is named DISP_WIDTH_384 by PSn00bSDK and documented as
 *     368 by psx-spx. Two different numbers for one bit, and no way to settle
 *     it except on a television - so it stays out until somebody looks.
 *   - The 480/512-line interlaced modes need two buffers of 480 or 512 lines
 *     in a VRAM that is only 512 lines tall. They would have to be
 *     single-buffered, and a menu that tears while you scroll it is worse
 *     than one that is short.
 *
 * What remains is every double-bufferable mode: four widths times both
 * refresh rates. 240x2 = 480 and 256x2 = 512 both fit, the latter exactly.
 */

typedef struct {
    short       width;
    short       height;     /* per buffer; two of these must fit in 512 */
    char        pal;        /* 1 = 50 Hz PAL timing, 0 = 60 Hz NTSC */
    const char *name;
} video_mode_t;

#define VIDEO_MODE_COUNT 8

extern const video_mode_t video_modes[VIDEO_MODE_COUNT];

/* Index of the mode matching the console's own region — always a safe start. */
int video_default_index(int is_pal);

/* Keep an index inside the table, wrapping in both directions. */
int video_wrap_index(int index);

/*
 * The kernel command line fragment for a mode, e.g. "psxvideo=320x256@50".
 *
 * Named psxvideo= rather than video= on purpose: video= belongs to the
 * framebuffer layer, and this is not a framebuffer. Returns a pointer to a
 * static buffer.
 */
const char *video_cmdline_arg(int index);

#endif
