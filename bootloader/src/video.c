/*
 * video.c — selectable PS1 display modes for the Blackroo bootloader
 *
 * See video.h for why this list is the shape it is.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <stdio.h>
#include "video.h"

/*
 * Ordered by width, with the two refresh rates adjacent, so that L2/R2 walk
 * the list in a way that makes sense on screen: each press is a small change,
 * and the neighbouring mode is the same size at the other rate.
 *
 * A PAL console will happily emit 60 Hz timing and vice versa - that is all
 * bit 3 does - so both are offered regardless of region. Whether the
 * television accepts it is exactly the question this menu exists to answer.
 */
const video_mode_t video_modes[VIDEO_MODE_COUNT] = {
    { 256, 240, 0, "256x240 60Hz" },
    { 256, 256, 1, "256x256 50Hz" },
    { 320, 240, 0, "320x240 60Hz" },
    { 320, 256, 1, "320x256 50Hz" },   /* PAL default */
    { 512, 240, 0, "512x240 60Hz" },
    { 512, 256, 1, "512x256 50Hz" },
    { 640, 240, 0, "640x240 60Hz" },
    { 640, 256, 1, "640x256 50Hz" }
};

int video_default_index(int is_pal)
{
    /* 320-wide at the console's own rate: what every PS1 game assumes, and
     * therefore the mode most likely to produce a picture on any set. */
    return is_pal ? 3 : 2;
}

int video_wrap_index(int index)
{
    while (index < 0)
        index += VIDEO_MODE_COUNT;

    return index % VIDEO_MODE_COUNT;
}

const char *video_cmdline_arg(int index)
{
    static char buf[32];
    const video_mode_t *m;

    index = video_wrap_index(index);
    m = &video_modes[index];

    snprintf(buf, sizeof(buf), "psxvideo=%dx%d@%d",
             m->width, m->height, m->pal ? 50 : 60);

    return buf;
}
