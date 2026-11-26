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

#ifdef KG_HWPERSP
/* Ф3е: the scale 2^k of a linear quantity o + u * gu + v * gv so that it fits 2^30 over the screen (u < 320,
   v < 200): k = 29 - floor(log2(bound)) */
static inline int kg_pscale(float o, float gu, float gv)
{
    union { float f; unsigned u; } b;
    b.f = __builtin_fabsf(o) + 320.0f * __builtin_fabsf(gu) + 200.0f * __builtin_fabsf(gv);
    if (b.f == 0.0f)
        return 0;
    int k = 29 - ((int)((b.u >> 23) & 255) - 127);
    return k > 100 ? 100 : k < -100 ? -100 : k;
}
/* x * 2^k as int32 (|x * 2^k| < 2^30 by the choice of k): a power of two made from the exponent bits */
static inline int kg_pfix(float x, int k)
{
    union { float f; unsigned u; } p;
    int e = k + 127;
    p.u = (unsigned)(e < 1 ? 1 : e > 254 ? 254 : e) << 23;
    return (int)(x * p.f);
}
/* the surface: the gradients of D_CalcGradients as fixed point (kg_span_tex sets TEX / TW) */
static inline void kg_persp_surface(float so, float su, float sv, float to, float tu, float tv,
                                    float zo, float zu, float zv, int sadj, int tadj, int bbs, int bbt)
{
    int ks = kg_pscale(so, su, sv), kt = kg_pscale(to, tu, tv), kz = kg_pscale(zo, zu, zv);
    MMIO_QSP_SO = (unsigned)kg_pfix(so, ks);
    MMIO_QSP_SU = (unsigned)kg_pfix(su, ks);
    MMIO_QSP_SV = (unsigned)kg_pfix(sv, ks);
    MMIO_QSP_TO = (unsigned)kg_pfix(to, kt);
    MMIO_QSP_TU = (unsigned)kg_pfix(tu, kt);
    MMIO_QSP_TV = (unsigned)kg_pfix(tv, kt);
    MMIO_QSP_ZO = (unsigned)kg_pfix(zo, kz);
    MMIO_QSP_ZU = (unsigned)kg_pfix(zu, kz);
    MMIO_QSP_ZV = (unsigned)kg_pfix(zv, kz);
    MMIO_QSP_K = ((unsigned)(kz & 255) << 16) | ((unsigned)(kt & 255) << 8) | (unsigned)(ks & 255);
    MMIO_QSP_SADJ = (unsigned)sadj;
    MMIO_QSP_TADJ = (unsigned)tadj;
    MMIO_QSP_BBS = (unsigned)bbs;
    MMIO_QSP_BBT = (unsigned)bbt;
}
/* Ф7: the same registers from precomputed fixed point (k = { kz, kt, ks } as MMIO_QSP_K) */
static inline void kg_persp_regs(int so, int su, int sv, int to, int tu, int tv, int zo, int zu, int zv, unsigned k,
                                 int sadj, int tadj, int bbs, int bbt)
{
    MMIO_QSP_SO = (unsigned)so;
    MMIO_QSP_SU = (unsigned)su;
    MMIO_QSP_SV = (unsigned)sv;
    MMIO_QSP_TO = (unsigned)to;
    MMIO_QSP_TU = (unsigned)tu;
    MMIO_QSP_TV = (unsigned)tv;
    MMIO_QSP_ZO = (unsigned)zo;
    MMIO_QSP_ZU = (unsigned)zu;
    MMIO_QSP_ZV = (unsigned)zv;
    MMIO_QSP_K = k;
    MMIO_QSP_SADJ = (unsigned)sadj;
    MMIO_QSP_TADJ = (unsigned)tadj;
    MMIO_QSP_BBS = (unsigned)bbs;
    MMIO_QSP_BBT = (unsigned)bbt;
}
static inline void kg_pspan(int u, int v, int count)
{
    MMIO_QSP_PSPAN = ((unsigned)(count - 1) << 17) | ((unsigned)v << 9) | (unsigned)u;
}
#ifdef KG_POLY
/* Ф7: a polygon vertex (u, v: 12.4 fixed point) and the draw of the polygon (the surface as kg_persp_surface) */
static inline void kg_poly_vtx(int u, int v)
{
    MMIO_QSP_PVTX = ((unsigned)v << 16) | ((unsigned)u & 0xffffu);
}
static inline void kg_poly_draw(int ztest)   /* 0: the world (pixels not covered yet), 1: z >= stored */
{
    MMIO_QSP_PDRAW = (unsigned)ztest;
}
#endif
#endif

#endif
