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

static byte surfcache_storage[SURFCACHE_SIZE] __attribute__((aligned(32)));
static short zbuffer_storage[BASEWIDTH * BASEHEIGHT] __attribute__((aligned(32)));
unsigned short d_8to16table[256];
unsigned d_8to24table[256];
static unsigned int vid_frame;

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
    d_pzbuffer = zbuffer_storage;
    D_InitCaches(surfcache_storage, SURFCACHE_SIZE);
    VID_SetPalette(palette);
}

void VID_Shutdown(void)
{
}

void VID_Update(vrect_t *rects)
{
    (void)rects;
    MMIO_FRAME = vid_frame++;
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
