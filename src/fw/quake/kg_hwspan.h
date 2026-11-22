/* kg_hwspan.h -- karabas Ф3: the span drawer of the core (quake_span, MMIO 0x100.., src/fw/mmio.h) for
   d_scan.c (KG_HWSPAN). The frame is vid.buffer = FB_BASE (SRAM), the z-buffer QSP_ZB through SRAM_UC. */
#ifndef KG_HWSPAN_H
#define KG_HWSPAN_H
#undef SDRAM_BASE
#include "mmio.h"   /* src/fw (-I) */

static inline void kg_span_tex(const unsigned char *pbase, int cachewidth)
{
    MMIO_QSP_TEX = (unsigned)pbase & 0x11ffffffu;   /* bit 28: SDRAM (1) / SRAM (0) */
    MMIO_QSP_TW = (unsigned)cachewidth;
}

static inline void kg_span(const unsigned char *pdest, int s, int t, int sstep, int tstep, int count)
{
    MMIO_QSP_S = (unsigned)s;
    MMIO_QSP_T = (unsigned)t;
    MMIO_QSP_SSTEP = (unsigned)sstep;
    MMIO_QSP_TSTEP = (unsigned)tstep;
    MMIO_QSP_SPAN = ((unsigned)(count - 1) << 16) | (((unsigned)pdest - FB_BASE) & 0xffffu);
}

static inline void kg_zspan(const short *pdest, int izi, int izistep, int count)
{
    if (count <= 0)
        return;
    MMIO_QSP_IZI = (unsigned)izi;
    MMIO_QSP_IZISTEP = (unsigned)izistep;
    MMIO_QSP_ZSPAN = ((unsigned)(count - 1) << 16) | (((((unsigned)pdest & 0x3fffffu) - QSP_ZB) >> 1) & 0xffffu);
}

#endif
