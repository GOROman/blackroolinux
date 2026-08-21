/*
 *	Virtual Console over PlayStation GPU.
 * FIXME: Virtual screen resolution is not supported now !!!
 */

#undef FBCONDEBUG

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/delay.h>	/* MSch: for IRQ probe */
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/console_struct.h>
#include <linux/string.h>
#include <linux/kd.h>
#include <linux/malloc.h>
#include <linux/fb.h>
#include <linux/vt_kern.h>
#include <linux/selection.h>
#include <linux/smp.h>
#include <linux/init.h>

#include <asm/ps/libpsx.h>
#include <asm/io.h>

/*
 * BLACKROO 2026-08-22: the console geometry is chosen at run time.
 *
 * It used to be two compile-time pairs selected by CONFIG_VT_CONSOLE_HIRES,
 * and the display mode handed to InitGPU() was a magic constant with the PAL
 * bit set in *both* variants - so an NTSC console got PAL timing and the
 * resolution could not be changed without rebuilding.
 *
 * kloader now lets the user walk display modes with L2/R2 and passes the one
 * they settled on as `psxvideo=WxH@R` on the command line. Honouring that is
 * what makes the choice mean anything once Linux takes over.
 *
 * The buffer is sized for the largest mode and the live geometry is a pair of
 * variables; the array's stride stays PSXVGA_MAX_W so indexing is unaffected.
 */
#define PSXVGA_MAX_W	80	/* 640 / 8 */
#define PSXVGA_MAX_H	21	/* 256 / 12 */
#define PSXVGA_FNT_H	12
#define PSXVGA_FNT_W	 8

static int psxvga_cols = 37;	/* replaced in psxvga_startup() */
static int psxvga_rows = 21;


static unsigned int psxvga_scrbuf[PSXVGA_MAX_H][PSXVGA_MAX_W];  //PSX TEXT SCREEN BUFFER
static int psxvga_cury;				// POINTER TO CURRENT STRING
static int psxvga_curx;				// POINTER TO CURRENT POSITION IN STR
static int psxvga_bottom;


/*
 *  Interface used by the world
 */

static const char *psxvga_startup(void);
static void psxvga_init(struct vc_data *conp, int init);
static void psxvga_deinit(struct vc_data *conp);
static void psxvga_clear(struct vc_data *conp, int sy, int sx, int height,
		       int width);
static void psxvga_putc(struct vc_data *conp, int c, int ypos, int xpos);
static void psxvga_putcs(struct vc_data *conp, const unsigned short *s, int count,
			int ypos, int xpos);
static void psxvga_cursor(struct vc_data *conp, int mode);
static int  psxvga_scroll(struct vc_data *conp, int t, int b, int dir,
			 int count);
static void psxvga_bmove(struct vc_data *conp, int sy, int sx, int dy, int dx,
			int height, int width);
static int  psxvga_switch(struct vc_data *conp);
static int  psxvga_blank(struct vc_data *conp, int blank);
static int  psxvga_font_op(struct vc_data *conp, struct console_font_op *op);
static int  psxvga_set_palette(struct vc_data *conp, unsigned char *table);
static int  psxvga_scrolldelta(struct vc_data *conp, int lines);


/*
 *  Low Level Operations
 */


/*
 * GP1(08h) display mode, per psx-spx and PSn00bSDK's GPU_DispFlags:
 *
 *   bits 0-1  horizontal   0=256  1=320  2=512  3=640
 *   bit  2    vertical     0=240-class  1=480-class
 *   bit  3    video mode   0=NTSC 60Hz  1=PAL 50Hz
 *   bit  4    colour depth
 *   bit  5    interlace
 */
#define GP1		(*(volatile unsigned long *)0xbf801814)

#define DISP_H_256	0
#define DISP_H_320	1
#define DISP_H_512	2
#define DISP_H_640	3
#define DISP_PAL	(1 << 3)

/*
 * Vertical display range, GP1(07h): first and last scanline, ten bits each.
 *
 * InitGPU() writes the PAL pair (37..292, so 255 lines) unconditionally, which
 * is 15 lines too many for a 60 Hz field and rolls the picture on an NTSC set.
 * The NTSC pair below is 16..256 - 240 lines - which is what psx-spx gives as
 * typical, and matches the PAL numbers already proven on this console.
 */
#define VRANGE_PAL	0x07049025UL	/* 37..292 : 255 lines */
#define VRANGE_NTSC	0x07040010UL	/* 16..256 : 240 lines */

static void psxvga_set_vrange (unsigned long mode)
{
   GP1 = (mode & DISP_PAL) ? VRANGE_PAL : VRANGE_NTSC;
}

/*
 * Read `psxvideo=WxH@R` off the kernel command line, e.g. psxvideo=640x256@50.
 *
 * kloader writes it there from whatever mode the user left its menu on. With
 * no such argument - a kernel booted by hand, or by an older kloader - fall
 * back to what this driver did before: PAL, and the width that
 * CONFIG_VT_CONSOLE_HIRES used to select.
 */
static unsigned long psxvga_setup_mode (void)
{
   extern char saved_command_line[];
   unsigned long mode;
   int w, h, rate;
   char *p;

#ifdef CONFIG_VT_CONSOLE_HIRES
   w = 640;
#else
   w = 320;
#endif
   h = 256;
   rate = 50;

   p = strstr (saved_command_line, "psxvideo=");
   if (p) {
      int v[3], i;

      p += 9;
      for (i = 0; i < 3; i++) {
         v[i] = 0;
         while (*p >= '0' && *p <= '9')
            v[i] = v[i] * 10 + (*p++ - '0');
         if (*p == 'x' || *p == '@')
            p++;
         else
            break;
      }

      /* Only take it if all three parsed as something sane. */
      if (v[0] >= 256 && v[1] >= 200 && (v[2] == 50 || v[2] == 60)) {
         w = v[0];
         h = v[1];
         rate = v[2];
      } else {
         printk (KERN_WARNING "psxcon: ignoring malformed psxvideo= "
                 "argument\n");
      }
   }

   switch (w) {
      case 256: mode = DISP_H_256; break;
      case 512: mode = DISP_H_512; break;
      case 640: mode = DISP_H_640; break;
      default:  mode = DISP_H_320; w = 320; break;
   }

   mode |= 0x08000000UL;
   if (rate == 50)
      mode |= DISP_PAL;

   /*
    * Two columns and one row are given up as a margin, the way the old fixed
    * geometry did: 640 wide gave 78 columns, not 80.
    */
   psxvga_cols = (w / PSXVGA_FNT_W) - 2;
   psxvga_rows = (h / PSXVGA_FNT_H);

   if (psxvga_cols > PSXVGA_MAX_W) psxvga_cols = PSXVGA_MAX_W;
   if (psxvga_rows > PSXVGA_MAX_H) psxvga_rows = PSXVGA_MAX_H;
   if (psxvga_cols < 20) psxvga_cols = 20;
   if (psxvga_rows < 8)  psxvga_rows = 8;

   printk (KERN_INFO "psxcon: %dx%d %s, console %dx%d\n",
           w, h, (rate == 50) ? "PAL 50Hz" : "NTSC 60Hz",
           psxvga_cols, psxvga_rows);

   return mode;
}

static const char *psxvga_startup (void)
{
   const char *display_desc = "PSXGPU console";
   int  x, y;
   unsigned long mode;
   for (y = 0; y < psxvga_rows; y++)
      for (x = 0; x < psxvga_cols; x++)
         psxvga_scrbuf[y][x] = 0;
         
   psxvga_cury = 0;	
   psxvga_curx = 0;
   
   mode = psxvga_setup_mode ();
   InitGPU (mode);
   psxvga_set_vrange (mode);
   cls ();
   LoadFont ();
    psxvga_bottom = 0;

   return  display_desc;
}


static void psxvga_init(struct vc_data *conp, int init)
{

	conp->vc_can_do_color = 0;
	conp->vc_cols = psxvga_cols;
	conp->vc_rows = psxvga_rows;
	conp->vc_x=(unsigned int)psxvga_curx;
	conp->vc_y=(unsigned int)psxvga_cury;
	conp->vc_origin=(unsigned long)0;
	conp->vc_complement_mask=0x7700;
	conp->vc_size_row=psxvga_cols*2;
}


static void psxvga_deinit(struct vc_data *conp)
{

}

static inline void psxvga_writew2 (unsigned int val, int y, int x)
{
   if (y < psxvga_rows && x < psxvga_cols)
   {
	    line(((y*PSXVGA_FNT_H)<<16)+((x*PSXVGA_FNT_W)),((PSXVGA_FNT_H)<<16)+(PSXVGA_FNT_W),0x000100);
	   print2 (x*PSXVGA_FNT_W, y*PSXVGA_FNT_H, val);   
	   gpu_dma_gpu_idle();                            
      
      y += psxvga_bottom;
      if (y >= psxvga_rows) y -= psxvga_rows;
      psxvga_scrbuf[y][x] = val;
   }
}

static inline void psxvga_printscreen (void)
{
   int x, y, z;
   
	cls ();
	
	for (y = 0, z = psxvga_bottom; z < psxvga_rows; z++, y++) // print top piece of screen
	{
      for(x = 0; x < psxvga_cols; x++)
	   {
	      if (psxvga_scrbuf[z][x] != 0 && psxvga_scrbuf[z][x] != ' ')
	      {
	         print2 (x*PSXVGA_FNT_W, y*PSXVGA_FNT_H, psxvga_scrbuf[z][x]);
	         gpu_dma_gpu_idle();
	      }
	   }
   }

	for (z = 0; z < psxvga_bottom; z++, y++) // print bottom piece of screen
	{
      for (x = 0; x < psxvga_cols; x++)
	   {
	      if (psxvga_scrbuf[z][x] != 0 && psxvga_scrbuf[z][x] != ' ')
	      {
	         print2 (x*PSXVGA_FNT_W, y*PSXVGA_FNT_H, psxvga_scrbuf[z][x]);
	         gpu_dma_gpu_idle();
	      }
	   }
	}
}


static inline u16 psxvga_readw (u16 addr)
{
   int x, y;
   
   y = addr/psxvga_cols;
   x = addr-y*psxvga_cols;

   return (u16)psxvga_scrbuf[y][x];
}

static inline void psxvga_memsetw(u16 sx,u16 sy, u16 c, unsigned int count)
{
   while (count) {
	   count--;
	   psxvga_writew2 (c, sy,sx);
   }
}


static inline void psxvga_memmovew(u16 to, u16 from, int count)
{
/*
    if (to < from) {
	while (count) {
	    count--;
	    psxvga_writew(psxvga_readw(from++), to++);
	}
    } else {
	from += count;
	to += count;
	while (count) {
	    count--;
	    psxvga_writew(psxvga_readw(--from), --to);
	}
    }
*/
}

/* ====================================================================== */

/*  fbcon_XXX routines - interface used by the world
 *
 *  This system is now divided into two levels because of complications
 *  caused by hardware scrolling. Top level functions:
 *
 *	fbcon_bmove(), fbcon_clear(), fbcon_putc()
 *
 *  handles y values in range [0, scr_height-1] that correspond to real
 *  screen positions. y_wrap shift means that first line of bitmap may be
 *  anywhere on this display. These functions convert lineoffsets to
 *  bitmap offsets and deal with the wrap-around case by splitting blits.
 *
 *	fbcon_bmove_physical_8()    -- These functions fast implementations
 *	fbcon_clear_physical_8()    -- of original fbcon_XXX fns.
 *	fbcon_putc_physical_8()	    -- (fontwidth != 8) may be added later
 *
 *  WARNING:
 *
 *  At the moment fbcon_putc() cannot blit across vertical wrap boundary
 *  Implies should only really hardware scroll in rows. Only reason for
 *  restriction is simplicity & efficiency at the moment.
 */

static void psxvga_clear(struct vc_data *conp, int sy, int sx, int height,
			int width)
{

int y;
 if((sy>psxvga_rows)){}
 else
  {
 for(y=0;y<height;y++)
  psxvga_memsetw(sx,(sy+y)*psxvga_cols,' ', width);
  }
 
}


static void psxvga_putc(struct vc_data *conp, int c, int ypos, int xpos)
{
   psxvga_writew2 (c, ypos, xpos);
}


static void psxvga_putcs(struct vc_data *conp, const unsigned short * s, int count,
		       int ypos, int xpos)
{
   int i;

   for (i = 0; i < count; i++)
	{
      psxvga_writew2 ((scr_readw(s++)), ypos, xpos+i);
   }
}


static void psxvga_cursor(struct vc_data *conp, int mode)
{int x,y,t;

y=psxvga_cury;
x=psxvga_curx;
line(((y*PSXVGA_FNT_H)<<16)+(((x)*PSXVGA_FNT_W)),((PSXVGA_FNT_H)<<16)+(PSXVGA_FNT_W),0x000100);
    t=y;
      y += psxvga_bottom;
      if (y >= psxvga_rows) y -= psxvga_rows;
    
print2 ((x)*PSXVGA_FNT_W, (t)*PSXVGA_FNT_H, psxvga_scrbuf[y][x]);
gpu_dma_gpu_idle();

x=conp->vc_x;
y=conp->vc_y;
line(((y*PSXVGA_FNT_H)<<16)+((x*PSXVGA_FNT_W)),((PSXVGA_FNT_H)<<16)+(PSXVGA_FNT_W),0x1122FF);
psxvga_cury=y;
psxvga_curx=x;

}



static int psxvga_scroll(struct vc_data *conp, int t, int b, int dir, int count)
{
   int x, y, i;
   
   switch (dir)
   {
      case SM_UP:
		   for (y = psxvga_bottom, i = 0; i < count; i++, y++) {
            if (y >= psxvga_rows) y = 0;
		      for (x = 0; x < psxvga_cols; x++)
		         psxvga_scrbuf[y][x] = 0;
         }      
         psxvga_bottom += count;
	      if (psxvga_bottom >= psxvga_rows)
            psxvga_bottom -= psxvga_rows;
            
         break;
         
      case SM_DOWN:
	 
         break;
   }
   
	psxvga_printscreen ();    
//	scrup();
   return 0;
}


static void psxvga_bmove(struct vc_data *conp, int sy, int sx, int dy, int dx,
			int height, int width)
{

   int y;
 if((sy>psxvga_rows)&&(dy>psxvga_rows)){}
 else
  {
 for(y=0;y<height;y++)
  psxvga_memmovew(sx+(sy+y)*psxvga_cols,dx+(dy+y)*psxvga_cols , width);
  }

}


static int psxvga_switch(struct vc_data *conp)
{
    return 1;
}


static int psxvga_blank(struct vc_data *conp, int blank)
{
    return 0;
}


static int psxvga_font_op(struct vc_data *conp, struct console_font_op *op)
{
	    return -ENOSYS;
}

static int psxvga_set_palette(struct vc_data *conp, unsigned char *table)
{
 return -EINVAL;
}

static u16 *psxvga_screen_pos(struct vc_data *conp, int offset)
{
    return (u16 *)(psxvga_cury+offset);
}

static unsigned long psxvga_getxy(struct vc_data *conp, unsigned long pos, int *px, int *py)
{unsigned long ret;
    
    if (px) *px = psxvga_curx;
    if (py) *py = psxvga_cury;
    ret = pos + (psxvga_cols - psxvga_curx) * 2; // WARNING !!!!
    
    return ret;
}

static void psxvga_invert_region(struct vc_data *conp, u16 *p, int cnt)
{

}

static int psxvga_scrolldelta(struct vc_data *conp, int lines)
{
    return 0;
}

static int psxvga_set_origin(struct vc_data *conp)
{
    return 0;
}

static void psxvga_save_screen(struct vc_data *conp)
{

}
static u8 psxvga_build_attr(struct vc_data *conp,u8 color, u8 intens, u8 blink, u8 underline, u8 reverse)
{
  u8 attr = color;
 return attr;
}

/*
 *  The console `switch' structure for the PSX GPU based console
 */
 
const struct consw psxvga_con = {
    con_startup: 	psxvga_startup, 
    con_init: 		psxvga_init,
    con_deinit: 	psxvga_deinit,
    con_clear: 		psxvga_clear,
    con_putc: 		psxvga_putc,
    con_putcs: 		psxvga_putcs,
    con_cursor: 	psxvga_cursor,
    con_scroll: 	psxvga_scroll,
    con_bmove: 		psxvga_bmove,
    con_switch: 	psxvga_switch,
    con_blank: 		psxvga_blank,
    con_font_op:	psxvga_font_op,
    con_set_palette: 	psxvga_set_palette,
    con_scrolldelta: 	psxvga_scrolldelta,
    con_set_origin: 	psxvga_set_origin,
    con_save_screen:	psxvga_save_screen,
    con_build_attr:	psxvga_build_attr,
    con_invert_region:	psxvga_invert_region,
    con_screen_pos:	psxvga_screen_pos,
    con_getxy:		psxvga_getxy
};


/*
 *  Dummy Low Level Operations
 */

static void psxvga_dummy_op(void) {}

#define DUMMY	(void *)psxvga_dummy_op
/*
struct display_switch psxvga_dummy = {
    setup:	DUMMY,
    bmove:	DUMMY,
    clear:	DUMMY,
    putc:	DUMMY,
    putcs:	DUMMY,
    revc:	DUMMY,
};
*/

/*
 *  Visible symbols for modules
 */

EXPORT_SYMBOL(psxvga_redraw_bmove);
EXPORT_SYMBOL(psxvga_redraw_clear);
EXPORT_SYMBOL(psxvga_dummy);
EXPORT_SYMBOL(psxvga_con);



