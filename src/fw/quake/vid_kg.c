/*
 * vid_kg.c -- Quake video driver of the karabas-go core (karabas: replaces vid_pocket.c of PocketQuake).
 *
 * 320x200x8 into the SRAM frame FB_BASE (0x3f0000) of the Doom / Hexen SoC, palette in MMIO_PAL;
 * VID_Update marks the frame (MMIO_FRAME: the sim logs its counters, on the board the frame DMA
 * copies it to a display buffer). Z-buffer and surface cache in the SDRAM (bss).
 */

#include "quakedef.h"
#include "d_local.h"
#undef SDRAM_BASE
#include "mmio.h"   /* src/fw (-I) */

#define BASEWIDTH   320
#define BASEHEIGHT  200
#define SURFCACHE_SIZE (2 * 1024 * 1024)

#ifndef KG_SCSRAM
static byte surfcache_storage[SURFCACHE_SIZE] __attribute__((aligned(32)));
#endif
#ifndef KG_HWSPAN
static short zbuffer_storage[BASEWIDTH * BASEHEIGHT] __attribute__((aligned(32)));
#endif
unsigned short d_8to16table[256];
unsigned d_8to24table[256];
unsigned int vid_frame;   /* karabas: also the start of KG_SPIN (sys_kg.c) */
#ifdef KG_ACOUNT
int kg_apix, kg_sbuilt[4];
#endif

void VID_SetPalette(unsigned char *palette)
{
    int i;
    unsigned char *p = palette;
    for (i = 0; i < 256; i++) {
        unsigned int r = *p++, g = *p++, b = *p++;
        MMIO_PAL(i) = (r << 16) | (g << 8) | b;
        d_8to16table[i] = (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        d_8to24table[i] = r | (g << 8) | (b << 16);
    }
}

void VID_ShiftPalette(unsigned char *palette)
{
    VID_SetPalette(palette);
}

void VID_Init(unsigned char *palette)
{
    vid.maxwarpwidth = vid.width = vid.conwidth = BASEWIDTH;
    vid.maxwarpheight = vid.height = vid.conheight = BASEHEIGHT;
    vid.aspect = ((float)BASEHEIGHT / (float)BASEWIDTH) * (320.0f / 240.0f);
    vid.numpages = 1;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
    vid.buffer = vid.conbuffer = (pixel_t *)FB_BASE;
    vid.rowbytes = vid.conrowbytes = BASEWIDTH;
#ifdef KG_HWSPAN
    d_pzbuffer = (short *)(SRAM_UC + QSP_ZB);   /* Ф3: the span drawer writes it; the CPU reads it uncached */
#else
    d_pzbuffer = zbuffer_storage;
#endif
#ifdef KG_SCSRAM
#ifdef KG_POLY
    D_InitCaches((byte *)0x00020000, QSP_MASK - 0x20000);   /* Ф7: the coverage mask of the rasterizer after it */
#else
    D_InitCaches((byte *)0x00020000, 0x1e0000);   /* Ф3: the surface cache in the SRAM (the span drawer reads its texels there) */
#endif
#else
    D_InitCaches(surfcache_storage, SURFCACHE_SIZE);
#endif
    VID_SetPalette(palette);
}

void VID_Shutdown(void)
{
}

#ifdef KG_HW
void kg_fps_overlay(unsigned char *fb);   /* kg_qhw.c: the OSD item FPS */
#endif

void VID_Update(vrect_t *rects)
{
    (void)rects;
#ifdef KG_HW
    kg_fps_overlay((unsigned char *)FB_BASE);
#endif
#ifdef KG_ACOUNT   /* karabas bench: "A,<frame>,<alias pixels>" on the console */
    {
        extern int kg_apix;
        extern void term_printf(const char *fmt, ...);
        term_printf("A,%u,%d\n", vid_frame, kg_apix);
        kg_apix = 0;
        term_printf("S,%u,%d,%d,%d,%d\n", vid_frame, kg_sbuilt[0], kg_sbuilt[1], kg_sbuilt[2], kg_sbuilt[3]);
        kg_sbuilt[0] = kg_sbuilt[1] = kg_sbuilt[2] = kg_sbuilt[3] = 0;
    }
#endif
    MMIO_FRAME = vid_frame++;   /* the board: the DMA copies the frame to the buffer not shown */
}

/* PocketQuake: called by SCR_UpdateScreen after a swap; one frame here */
void VID_WaitSync(void)
{
}

void D_BeginDirectRect(int x, int y, byte *pbitmap, int width, int height)
{
    (void)x; (void)y; (void)pbitmap; (void)width; (void)height;
}

void D_EndDirectRect(int x, int y, int width, int height)
{
    (void)x; (void)y; (void)width; (void)height;
}
