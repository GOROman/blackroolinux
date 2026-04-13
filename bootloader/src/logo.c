/*
 * logo.c — draw the Linux boot logo on the kloader menu
 *
 * See logo.h for the VRAM layout and logo_data.c for the artwork, which is
 * Larry Ewing's Tux lifted straight out of this tree's own kernel headers.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <stdint.h>
#include <psxgpu.h>
#include "logo.h"

void logo_upload(void)
{
    RECT r;

    /*
     * At 8bpp the GPU packs two texels into each VRAM halfword, so an
     * 80-texel-wide image is 40 VRAM columns. Getting this wrong does not
     * fail loudly - it draws a smeared double-width penguin.
     */
    setRECT(&r, LOGO_VRAM_X, LOGO_VRAM_Y, LOGO_W / 2, LOGO_H);
    LoadImage(&r, (const uint32_t *)logo_pix);
    DrawSync(0);

    /* The CLUT is 256 entries on a single line, one halfword each. */
    setRECT(&r, LOGO_CLUT_X, LOGO_CLUT_Y, LOGO_CLUT_LEN, 1);
    LoadImage(&r, (const uint32_t *)logo_clut);
    DrawSync(0);
}

void logo_draw(int x, int y, int size)
{
    static POLY_FT4 poly;
    static DR_TPAGE tpage;

    /*
     * A scaled quad rather than a sprite, because a sprite is fixed at the
     * texture's own size and the menu runs at widths from 256 to 640. Tux
     * scales with the screen instead of being a postage stamp in one mode and
     * covering the text in another.
     */
    setPolyFT4(&poly);

    setXY4(&poly,
           x,        y,
           x + size, y,
           x,        y + size,
           x + size, y + size);

    /*
     * Texture coordinates are within the page. The page origin is the VRAM
     * address rounded down to a 64-halfword boundary, and 640 is already a
     * multiple of 64, so the image starts at U=0.
     *
     * The far edges are size-1, not size: they are inclusive, and using 80
     * here samples one column past the penguin.
     */
    poly.u0 = 0;            poly.v0 = 0;
    poly.u1 = LOGO_W - 1;   poly.v1 = 0;
    poly.u2 = 0;            poly.v2 = LOGO_H - 1;
    poly.u3 = LOGO_W - 1;   poly.v3 = LOGO_H - 1;

    /* 128 is neutral: the texture's own colours, untinted. */
    setRGB0(&poly, 128, 128, 128);

    setClut(&poly, LOGO_CLUT_X, LOGO_CLUT_Y);
    poly.tpage = getTPage(1 /* 8bpp */, 0, LOGO_VRAM_X, LOGO_VRAM_Y);

    /*
     * Chain the two primitives into a properly terminated list and hand that
     * to DrawOTag, rather than calling DrawPrim on each.
     *
     * The GPU walks a DMA linked list: every primitive's tag carries a 24-bit
     * "next" address, and the list ends at 0xffffff. setPolyFT4() and
     * setDrawTPage() fill in the length and the command code but NOT that
     * pointer - so a primitive drawn straight from the stack or .bss has
     * whatever happened to be in those bytes as its successor, and the DMA
     * follows it into memory and never comes back.
     *
     * The symptom is not a wrong-looking penguin. It is the *next* GPU call
     * hanging for ever: this drew fine and then FntFlush() never returned.
     */
    setDrawTPage(&tpage, 0, 1, poly.tpage);
    catPrim(&tpage, &poly);     /* tpage -> poly */
    termPrim(&poly);            /* poly  -> end of list */

    DrawOTag((const uint32_t *)&tpage);
}
