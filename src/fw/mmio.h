/* Ф0 bench MMIO (src/sim/iss.cpp). Memory map: SRAM 4 MB at 0x00000000, SDRAM 32 MB at 0x10000000,
   MMIO at 0x80000000 (VexRiscv IO range = bit 31, uncached). */
#pragma once
#include <stdint.h>

#define MMIO_REG(o) (*(volatile uint32_t *)(0x80000000u + (o)))
#define MMIO_UART    MMIO_REG(0x00) /* write: console byte */
#define MMIO_FRAME   MMIO_REG(0x04) /* write: frame index, the sim logs the counters of the frame */
#define MMIO_EXIT    MMIO_REG(0x08) /* write: stop the simulation with this code */
#define MMIO_CYCLES  MMIO_REG(0x0c) /* read: clock cycles (ISS: instructions), low 32 bits */
#define MMIO_MS      MMIO_REG(0x1c) /* read: milliseconds (ISS: instructions / 50000) */
#define MMIO_WADSIZE MMIO_REG(0x20) /* read: size of the WAD in memory at WAD_BASE */
#define MMIO_PAL(i)  MMIO_REG(0x400 + 4 * (i)) /* write: palette entry i = 0x00RRGGBB (256 entries) */

#define SRAM_BASE  0x00000000u
#define SDRAM_BASE 0x10000000u
#define WAD_BASE   0x10c00000u      /* the WAD (up to 20 MB), loaded by the sim (on the board: by the FL item) */
#define FB_BASE    (SRAM_BASE + 0x3f0000u) /* 320x200x8 frame in SRAM (below the 4 KB resume area) */

/* Ф2: timer, switches, sound (doom_soc.v, doom_audio.v) */
#define MMIO_US       MMIO_REG(0x40)  /* read: microseconds since reset */
#define MMIO_TCMP     MMIO_REG(0x44)  /* write: the timer interrupt when US - TCMP >= 0 (and TEN) */
#define MMIO_TEN      MMIO_REG(0x48)  /* write: bit 0 enables the timer interrupt */
#define MMIO_SW       MMIO_REG(0x50)  /* read: OSD switches; bits 15..8 = Music (0 OPL3, 1 MIDI, 2 off) */
#define MMIO_OPL_ADDR MMIO_REG(0x60)  /* write: { bank, register } */
#define MMIO_OPL_DATA MMIO_REG(0x64)  /* write: the value */
#define MMIO_MIDI     MMIO_REG(0x68)  /* write: a MIDI byte */
#define MMIO_SFX_POS  MMIO_REG(0x70)  /* read: the ring index played now */
#define MMIO_SFX(i)   MMIO_REG(0x2000 + 4 * (i)) /* write: ring entry i = { right16, left16 } */
#define SFX_RING      2048
#define SFX_RATE      11025

/* Ф1в: the drawer (doom_draw.v): columns and spans of the view drawn by the FPGA into the frame FB_BASE.
   DRW0 = offset in the frame (15..0) | pixels - 1 (24..16) | colormap index (30..25); FRAC / STEP: the
   column frac and step (16.16), or the span position and step (x 6.10 high, y 6.10 low); a write of COL
   (the column source) or SPAN (the 64x64 flat) as an SDRAM byte offset queues the command. CMADR / CMAP:
   the upload of the colormaps table (COLORMAP / FOGMAP, 8704 bytes; CMAP auto-increments). STAT: bits
   31..24 = DRW_ID when present, bit 0 busy; a write forgets the cached flat. The CPU accesses to the
   SRAM wait while the drawer is busy (the order of the frame); the frame is read through SRAM_UC
   (uncached: the drawer writes behind the D$). */
#define MMIO_DRW0      MMIO_REG(0xc0)
#define MMIO_DRW_FRAC  MMIO_REG(0xc4)
#define MMIO_DRW_STEP  MMIO_REG(0xc8)
#define MMIO_DRW_COL   MMIO_REG(0xcc)
#define MMIO_DRW_SPAN  MMIO_REG(0xd0)
#define MMIO_DRW_CMAP  MMIO_REG(0xd4)
#define MMIO_DRW_CMADR MMIO_REG(0xd8)
#define MMIO_DRW_STAT  MMIO_REG(0xdc)
#define DRW_ID         0xd7u
#define SRAM_UC        0x90000000u  /* the SRAM, uncached (bit 31: the IO range of VexRiscv) */

/* Quake Ф3: the span drawer (quake_span.v; functional model in src/sim/qspan_model.h). Texture spans of the
   surface cache (SDRAM) into the work frame FB_BASE and z spans into the z-buffer QSP_ZB (SRAM), queued.
   TEX: the texture (the surface cache block): bit 28 = 1 SDRAM (offset 24..0) / 0 SRAM (offset 21..0); TW: its
   row bytes (cachewidth);
   S / T / SSTEP / TSTEP: 16.16 texel position and steps (as d_scan.c); IZI / IZISTEP: 1/z 16.16 (z = izi >> 16);
   a write of SPAN / ZSPAN = { count - 1 (24..16, up to 512), pixel offset in the frame (15..0) } queues the
   command with the values written before. STAT: 31..24 = QSP_ID, bit 0 busy. The CPU accesses to the SRAM wait
   while the drawer is busy (the order of the frame); the CPU reads the z-buffer through SRAM_UC. */
#define MMIO_QSP_TEX     MMIO_REG(0x100)
#define MMIO_QSP_TW      MMIO_REG(0x104)
#define MMIO_QSP_S       MMIO_REG(0x108)
#define MMIO_QSP_T       MMIO_REG(0x10c)
#define MMIO_QSP_SSTEP   MMIO_REG(0x110)
#define MMIO_QSP_TSTEP   MMIO_REG(0x114)
#define MMIO_QSP_IZI     MMIO_REG(0x118)
#define MMIO_QSP_IZISTEP MMIO_REG(0x11c)
#define MMIO_QSP_SPAN    MMIO_REG(0x120)
#define MMIO_QSP_ZSPAN   MMIO_REG(0x124)
#define MMIO_QSP_STAT    MMIO_REG(0x13c)
#define QSP_ID           0x95u
#define QSP_ZB           0x3b0000u   /* the z-buffer in the SRAM: 320 x 200 x 16 bit (to 0x3cf3ff) */

/* Quake Ф3е: perspective in the drawer. Per surface (fixed point with a scale per quantity: S = sdivz * 2^ks,
   T = tdivz * 2^kt, Z = zi * 2^kz, each origin + u * grad_u + v * grad_v at screen pixel u, v): SO..ZV, K =
   { kz (23..16), kt (15..8), ks (7..0) } signed bytes, SADJ / TADJ (sadjust / tadjust), BBS / BBT (bbextents /
   bbextentt). PSPAN = { count - 1 (25..17), v (16..9), u (8..0) } queues a span: texels and z (the z-buffer
   QSP_ZB) as D_DrawSpans8 + D_DrawZSpans. Model: src/sim/qspan_model.h persp(). */
#define MMIO_QSP_SO      MMIO_REG(0x140)
#define MMIO_QSP_SU      MMIO_REG(0x144)
#define MMIO_QSP_SV      MMIO_REG(0x148)
#define MMIO_QSP_TO      MMIO_REG(0x14c)
#define MMIO_QSP_TU      MMIO_REG(0x150)
#define MMIO_QSP_TV      MMIO_REG(0x154)
#define MMIO_QSP_ZO      MMIO_REG(0x158)
#define MMIO_QSP_ZU      MMIO_REG(0x15c)
#define MMIO_QSP_ZV      MMIO_REG(0x160)
#define MMIO_QSP_K       MMIO_REG(0x164)
#define MMIO_QSP_SADJ    MMIO_REG(0x168)
#define MMIO_QSP_TADJ    MMIO_REG(0x16c)
#define MMIO_QSP_BBS     MMIO_REG(0x170)
#define MMIO_QSP_BBT     MMIO_REG(0x174)
#define MMIO_QSP_PSPAN   MMIO_REG(0x178)
/* Quake Ф7: the polygon rasterizer. PVTX = { v (31..16), u (15..0) }: a screen vertex, signed 12.4 fixed point;
   PDRAW draws the convex polygon of the vertices written since the last PDRAW with the surface above (TEX / TW,
   SO..BBT): rows ceil(vmin) .. ceil(vmax) - 1, pixels ceil(xl) .. ceil(xr) - 1. PDRAW bit 0 = 0 (the world,
   front to back from the BSP): a pixel is drawn where the coverage mask (below) is 0 (not covered yet); 1
   (bmodels): where its z >= the stored one. Model: src/sim/qspan_model.h poly(). */
#define MMIO_QSP_PVTX    MMIO_REG(0x180)
#define MMIO_QSP_PDRAW   MMIO_REG(0x184)
/* Ф7: the coverage mask of the rasterizer in the SRAM at QSP_MASK (a bit per pixel drawn since ZCLR, bit k of
   byte i = pixel 8 i + k; ZSPAN marks its pixels too; the end of the surface cache): ZCLR clears it (the frame
   start); the CPU reads it through SRAM_UC when the drawer is idle. */
#define MMIO_QSP_ZCLR    MMIO_REG(0x188)
#define QSP_MASK         0x1fe000u
