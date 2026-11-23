// Quake Ф3: functional model of the span drawer (quake_span, MMIO 0x100..0x13c, src/fw/mmio.h), shared by the
// iss and tb_soc. A command runs at once at its SPAN / ZSPAN write (tb_soc times it separately: per pixel
// the texel source and whether it missed the line cache are recorded in `miss`).
// TEX bit 28: 1 = the texture is in the SDRAM (offset 24..0), 0 = in the SRAM (offset 21..0).
// Counts what the RTL will pay: texel line fills (SDRAM texture) through a small cache (QS_LINES lines of 32
// bytes) and an estimate of the busy clocks (QS_PIX per pixel byte, QS_ZPIX per z halfword: the SRAM port;
// QS_FILL per line fill: the SDRAM, 8 words as a CPU cache line; QS_SRD per texel read from the SRAM).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <climits>

#ifndef QS_NLINES
#define QS_NLINES 4
#endif

struct QSpan {
    static const uint32_t ZB = 0x3b0000u, FB = 0x3f0000u;
    // Ф3е: the clocks of the RTL drawer port (an SRAM access through doom_sram: ~4), QS_SEG per perspective segment
    static const int QS_LINES = QS_NLINES, QS_PIX = 4, QS_ZPIX = 4, QS_FILL = 28, QS_SRD = 4, QS_SEG = 30;
    uint32_t tex = 0, tw = 0, izi = 0, izistep = 0;
    int32_t s = 0, t = 0, sstep = 0, tstep = 0;
    uint32_t tag[QS_LINES];
    int victim = 0;
    uint64_t cmds = 0, zcmds = 0, pix = 0, zpix = 0, fills = 0, srd = 0;
    std::vector<uint8_t> miss;   // the last SPAN command: per pixel 0 hit, 1 SDRAM line fill, 2 SRAM texel read

    // Ф3е: perspective in the drawer (MMIO 0x140..0x178). Per surface (fixed point, a scale per quantity, chosen
    // by the CPU so that the value fits 2^30 over the screen): S = sdivz * 2^ks, T = tdivz * 2^kt, Z = zi * 2^kz
    // as origin + u * grad_u + v * grad_v (u, v: screen pixel); sadjust / tadjust; bbextents / bbextentt.
    // PSPAN = { count - 1 (25..17), v (16..9), u (8..0) }: the span as D_DrawSpans8 (KG_QOPT: the last steps
    // by the reciprocal table) + its z per pixel (Z >> (kz - 15)), see persp().
    int32_t so = 0, su = 0, sv = 0, to_ = 0, tu = 0, tv = 0, zo = 0, zu = 0, zv = 0, sadj = 0, tadj = 0, bbs = 0, bbt = 0;
    int ks = 0, kt = 0, kz = 0;
    uint64_t pcmds = 0, psegs = 0;
    uint64_t p_trd = 0, p_pwr = 0;     // Ф3е RTL estimate: texel reads with a last-halfword reuse, frame writes in pairs
    int last_segs = 0;

    // Ф7: the polygon rasterizer (MMIO 0x180..0x184). PVTX = { v (31..16), u (15..0) }: a screen vertex, signed
    // 12.4 fixed point (Quake clamps u to [-0.5, width - 0.5]); PDRAW (any value) draws the convex polygon of
    // the vertices written since the last PDRAW with the surface of 0x100 / 0x104 / 0x140..0x174: the rows
    // ceil(vmin) .. ceil(vmax) - 1, per row the pixels ceil(xl) .. ceil(xr) - 1 (x of the two edges that cross
    // the row: closed form = a DDA in 2^-16 pixels, see poly()), each pixel as a PSPAN one but z tested:
    // drawn (texel, frame, z) only when PDRAW bit 0 = 0 (the world: it comes front to back from the BSP, so no compare,
    // which the 16-bit z, coarse far away, would get wrong): the pixel is not covered yet (the coverage mask: a bit per
    // pixel drawn since ZCLR, in the SRAM at MASK, read / written by halfwords of 16 pixels; ZSPAN marks its pixels
    // too); bit 0 = 1 (bmodels, after the world): its z halfword >= the stored one (signed, as d_polyse.c).
    // ZCLR (0x188) clears the mask.
    // ops: the memory accesses / arithmetic clocks of the last PDRAW for the tb_soc timing.
    static const int QP_MAXV = 64;
    struct Op { uint8_t mem; uint16_t cyc; };   // mem 1: an SRAM access of cyc clocks, 0: cyc clocks of arithmetic
    int32_t pvu[QP_MAXV], pvv[QP_MAXV];
    int pvn = 0;
    std::vector<Op> ops;
    uint64_t polys = 0, prows = 0, ptest = 0, ppass = 0, pops_clk = 0;
    static const int QP_ROW = 2, QP_VTX = 4, QP_EDGE = 36, QP_POLY = 8;
    static const uint32_t MASK = 0x1fe000u;   // the coverage mask in the SRAM: bit = pixel (320 x 200), 8000 bytes
    bool polymode = false;                    // a ZCLR was seen (the older firmware keeps its surface cache there)
    static bool mbit(const uint8_t *sram, uint32_t o) { return (sram[MASK + (o >> 3)] >> (o & 7)) & 1; }
    static void mset(uint8_t *sram, uint32_t o) { sram[MASK + (o >> 3)] |= (uint8_t)(1u << (o & 7)); }
    QSpan() { for (auto &g : tag) g = 0xffffffffu; }
    bool tex_sdram() const { return (tex >> 28) & 1; }
    uint64_t busy() const { return pix * QS_PIX + zpix * QS_ZPIX + fills * QS_FILL + srd * QS_SRD; }

    // returns true when the register was one of the drawer
    bool write(uint32_t o, uint32_t v, uint8_t *sram, const uint8_t *sdram, uint32_t sdram_mask) {
        switch (o) {
        case 0x100: tex = v & 0x11ffffffu; for (auto &g : tag) g = 0xffffffffu; return true;   // a new texture: forget the lines
        case 0x104: tw = v; return true;
        case 0x108: s = (int32_t)v; return true;
        case 0x10c: t = (int32_t)v; return true;
        case 0x110: sstep = (int32_t)v; return true;
        case 0x114: tstep = (int32_t)v; return true;
        case 0x118: izi = v; return true;
        case 0x11c: izistep = v; return true;
        case 0x120: {
            uint32_t off = v & 0xffff, n = ((v >> 16) & 0x1ff) + 1;
            int32_t ss = s, tt = t;
            cmds++;
            miss.assign(n, 0);
            for (uint32_t i = 0; i < n; i++, off++) {
                uint32_t a = (tex + (uint32_t)(ss >> 16) + (uint32_t)(tt >> 16) * tw);
                uint8_t texel;
                if (tex_sdram()) {
                    a &= sdram_mask;
                    uint32_t l = a >> 5;
                    bool hit = false;
                    for (auto g : tag) hit |= g == l;
                    if (!hit) { tag[victim] = l; victim = (victim + 1) % QS_LINES; fills++; miss[i] = 1; }
                    texel = sdram[a];
                } else {
                    texel = sram[a & 0x3fffff]; srd++; miss[i] = 2;
                }
                if (off < 64000) sram[FB + off] = texel;
                pix++;
                ss += sstep; tt += tstep;
            }
            return true;
        }
        case 0x140: so = (int32_t)v; return true;
        case 0x144: su = (int32_t)v; return true;
        case 0x148: sv = (int32_t)v; return true;
        case 0x14c: to_ = (int32_t)v; return true;
        case 0x150: tu = (int32_t)v; return true;
        case 0x154: tv = (int32_t)v; return true;
        case 0x158: zo = (int32_t)v; return true;
        case 0x15c: zu = (int32_t)v; return true;
        case 0x160: zv = (int32_t)v; return true;
        case 0x164: ks = (int8_t)(v & 0xff); kt = (int8_t)((v >> 8) & 0xff); kz = (int8_t)((v >> 16) & 0xff); return true;
        case 0x168: sadj = (int32_t)v; return true;
        case 0x16c: tadj = (int32_t)v; return true;
        case 0x170: bbs = (int32_t)v; return true;
        case 0x174: bbt = (int32_t)v; return true;
        case 0x178: persp(v, sram); return true;
        case 0x180: if (pvn < QP_MAXV) { pvu[pvn] = (int16_t)(v & 0xffff); pvv[pvn] = (int16_t)(v >> 16); pvn++; } return true;
        case 0x184: poly(sram, v & 1); pvn = 0; return true;
        case 0x188:                                           // ZCLR: the coverage mask, 4000 halfword writes
            memset(&sram[MASK], 0, 8000); polymode = true;   // from now on ZSPAN marks the mask too
            ops.assign(4000, Op{1, (uint16_t)QS_ZPIX});
            return true;
        case 0x124: {
            uint32_t off = v & 0xffff, n = ((v >> 16) & 0x1ff) + 1, z = izi;
            zcmds++;
            for (uint32_t i = 0; i < n; i++, off++) {
                if (off < 64000) { uint16_t h = (uint16_t)(z >> 16); memcpy(&sram[ZB + 2 * off], &h, 2); if (polymode) mset(sram, off); }
                z += izistep;
                zpix++;
            }
            return true;
        }
        }
        return false;
    }
    static uint32_t stat() { return 0x95000000u; }   // QSP_ID, never busy (the model runs at once)

    // s (or t) of a point: (S * r) >> (k - kz + 8 + e), with Z = m * 2^(e - 23), m in [2^23, 2^24), r = 2^47 / m;
    // saturated to int32 (s = sdivz * 65536 / zi as d_scan.c; a Z <= 0 gives the biggest value)
    static int64_t persp_q(int32_t S, int32_t Z, int k, int kz) {
        if (Z <= 0) return S < 0 ? INT32_MIN : INT32_MAX;
        int e = 31 - __builtin_clz((uint32_t)Z);
        uint32_t m = e >= 23 ? (uint32_t)Z >> (e - 23) : (uint32_t)Z << (23 - e);
        uint64_t r = (1ull << 47) / m;                       // (2^23, 2^24]
        int64_t prod = (int64_t)S * (int64_t)r;              // |S| < 2^31: fits 2^55
        int sh = k - kz + 8 + e;
        int64_t q;
        if (sh >= 63) q = prod < 0 ? -1 : 0;
        else if (sh >= 0) q = prod >> sh;
        else if (sh > -32 && (prod == 0 || (prod > 0 ? prod <= (INT64_MAX >> -sh) : prod >= (INT64_MIN >> -sh)))) q = prod << -sh;
        else q = prod < 0 ? INT32_MIN : INT32_MAX;
        if (q > INT32_MAX) q = INT32_MAX;
        if (q < INT32_MIN) q = INT32_MIN;
        return q;
    }
    // as d_scan.c: the upper bound first (a tiny surface may have bbextents < lo)
    static int32_t clampi(int64_t x, int32_t lo, int32_t hi) { return x > hi ? hi : x < lo ? lo : (int32_t)x; }
    // the z halfword of a pixel: Zp >> sh (arithmetic; sh >= 32: the sign), sh < 0: Zp << -sh (-sh >= 32: 0)
    static uint16_t zshift(int32_t Zp, int sh) {
        if (sh >= 32) return Zp < 0 ? 0xffff : 0;
        if (sh >= 0) return (uint16_t)(Zp >> sh);
        if (sh <= -32) return 0;
        return (uint16_t)((uint32_t)Zp << -sh);
    }

    void persp(uint32_t v, uint8_t *sram) {
        persp_span(v & 0x1ff, (v >> 9) & 0xff, ((v >> 17) & 0x1ff) + 1, sram, 0);
    }

    // Ф7: x of an edge (Xa, Ya) -> (Xb, Yb) (12.4, Ya < Yb) at row y in 2^-16 pixels: from its first row
    // yf = ceil(Ya / 16) on by step per row (the RTL: a DDA; the same integers)
    static int32_t edge_x(int32_t Xa, int32_t Ya, int32_t Xb, int32_t Yb, int y) {
        int64_t step = ((int64_t)(Xb - Xa) * 65536) / (Yb - Ya);          // truncated (C), |step| < 2^29
        int yf = (Ya + 15) >> 4;
        int64_t x0 = (int64_t)Xa * 4096 + (((int64_t)(16 * yf - Ya) * step) >> 4);
        return (int32_t)(x0 + (int64_t)(y - yf) * step);
    }
    void poly(uint8_t *sram, int zmode) {
        ops.clear();
        int n = pvn;
        if (n < 3) return;
        polys++;
        ops.push_back({0, (uint16_t)(QP_POLY + QP_VTX * n)});
        int vmin = INT32_MAX, vmax = INT32_MIN;
        for (int i = 0; i < n; i++) { if (pvv[i] < vmin) vmin = pvv[i]; if (pvv[i] > vmax) vmax = pvv[i]; }
        int y0 = (vmin + 15) >> 4, y1 = (vmax + 15) >> 4;
        for (int i = 0; i < n; i++) {   // the edge setup (a division) of each edge that covers a row
            int j = i + 1 == n ? 0 : i + 1;
            int ya = pvv[i] < pvv[j] ? pvv[i] : pvv[j], yb = pvv[i] < pvv[j] ? pvv[j] : pvv[i];
            if (((ya + 15) >> 4) < ((yb + 15) >> 4)) ops.push_back({0, (uint16_t)QP_EDGE});
        }
        if (y0 < 0) y0 = 0;
        if (y1 > 200) y1 = 200;
        for (int y = y0; y < y1; y++) {
            int32_t xl = INT32_MAX, xr = INT32_MIN;
            int found = 0;
            for (int i = 0; i < n; i++) {
                int j = i + 1 == n ? 0 : i + 1;
                int32_t Xa = pvu[i], Ya = pvv[i], Xb = pvu[j], Yb = pvv[j];
                if (Ya == Yb) continue;
                if (Ya > Yb || (Ya == Yb && Xa > Xb)) { int32_t t = Xa; Xa = Xb; Xb = t; t = Ya; Ya = Yb; Yb = t; }
                if (16 * y < Ya || 16 * y >= Yb) continue;   // the row samples at y: Ya <= 16 y < Yb
                int32_t x = edge_x(Xa, Ya, Xb, Yb, y);
                if (x < xl) xl = x;
                if (x > xr) xr = x;
                found++;
            }
            prows++;
            ops.push_back({0, (uint16_t)QP_ROW});
            if (found < 2) continue;
            int xs = (int)(((int64_t)xl + 0xffff) >> 16), xe = (int)(((int64_t)xr + 0xffff) >> 16);
            if (xs < 0) xs = 0;
            if (xe > 320) xe = 320;
            if (xs < xe) persp_span(xs, y, xe - xs, sram, zmode + 1);
        }
        for (auto &o : ops) pops_clk += o.cyc;
    }

    // a span from screen pixel (u0, v0); zt (Ф7): z tested, the ops recorded
    void persp_span(int u0, int v0, int count, uint8_t *sram, int zt) {
        static const int recip[16] = { 0, 65536, 32768, 21845, 16384, 13107, 10923, 9362,
                                       8192, 7282, 6554, 5958, 5461, 5041, 4681, 4369 };
        bool prev_drawn = false, mdirty = false;
        int mh = -1;                  // Ф7: the mask halfword held (16 pixels), written back when it changed
        int32_t S = (int32_t)((uint32_t)so + (uint32_t)u0 * (uint32_t)su + (uint32_t)v0 * (uint32_t)sv);
        int32_t T = (int32_t)((uint32_t)to_ + (uint32_t)u0 * (uint32_t)tu + (uint32_t)v0 * (uint32_t)tv);
        int32_t Z = (int32_t)((uint32_t)zo + (uint32_t)u0 * (uint32_t)zu + (uint32_t)v0 * (uint32_t)zv);
        int32_t Zp = Z;                                      // the z of the pixels
        uint32_t last_ta = 0xffffffffu;
        int32_t s = clampi(persp_q(S, Z, ks, kz) + sadj, 0, bbs), t = clampi(persp_q(T, Z, kt, kz) + tadj, 0, bbt);
        int32_t sstep = 0, tstep = 0;
        uint32_t off = (uint32_t)(v0 * 320 + u0);
        pcmds++;
        last_segs = 0;
        miss.assign(count, 2);
        while (count > 0) {
            int seg = count >= 16 ? 16 : count;
            count -= seg;
            int32_t snext, tnext;
            if (count) {
                S = (int32_t)((uint32_t)S + ((uint32_t)su << 4)); T = (int32_t)((uint32_t)T + ((uint32_t)tu << 4));
                Z = (int32_t)((uint32_t)Z + ((uint32_t)zu << 4));
                snext = clampi(persp_q(S, Z, ks, kz) + sadj, 16, bbs);
                tnext = clampi(persp_q(T, Z, kt, kz) + tadj, 16, bbt);
                sstep = (snext - s) >> 4;
                tstep = (tnext - t) >> 4;
            } else {
                S = (int32_t)((uint32_t)S + (uint32_t)su * (uint32_t)(seg - 1));
                T = (int32_t)((uint32_t)T + (uint32_t)tu * (uint32_t)(seg - 1));
                Z = (int32_t)((uint32_t)Z + (uint32_t)zu * (uint32_t)(seg - 1));
                snext = clampi(persp_q(S, Z, ks, kz) + sadj, 8, bbs);
                tnext = clampi(persp_q(T, Z, kt, kz) + tadj, 8, bbt);
                if (seg > 1) {
                    sstep = (int32_t)(((int64_t)(snext - s) * recip[seg - 1]) >> 16);
                    tstep = (int32_t)(((int64_t)(tnext - t) * recip[seg - 1]) >> 16);
                }
            }
            psegs++; last_segs++;
            // Ф7: the arithmetic of the first segment of a span (the next ones overlap the pixels of the previous)
            if (zt && last_segs == 1) ops.push_back({0, (uint16_t)QS_SEG});
            for (int i = 0; i < seg; i++, off++) {
                uint32_t a = tex + (uint32_t)(s >> 16) + (uint32_t)(t >> 16) * tw;
                uint16_t zv16 = zshift(Zp, kz - 15);
                if (zt) {
                    uint32_t o = off < 64000 ? off : 0;
                    bool hide;
                    ptest++;
                    if (zt == 1) {                                   // the world: the coverage mask, a halfword per 16 pixels
                        if ((int)(o >> 4) != mh) {
                            if (mdirty) ops.push_back({1, (uint16_t)QS_ZPIX});
                            ops.push_back({1, (uint16_t)QS_SRD});
                            mh = (int)(o >> 4); mdirty = false;
                        }
                        hide = mbit(sram, o);
                        ops.push_back({0, 1});
                    } else {                                         // bmodels: the z-buffer
                        int16_t old;
                        memcpy(&old, &sram[ZB + 2 * o], 2);
                        hide = (int16_t)zv16 < old;
                        ops.push_back({1, (uint16_t)QS_ZPIX});
                    }
                    if (hide) {
                        prev_drawn = false;
                        s = (int32_t)((uint32_t)s + (uint32_t)sstep); t = (int32_t)((uint32_t)t + (uint32_t)tstep);
                        Zp = (int32_t)((uint32_t)Zp + (uint32_t)zu);
                        continue;
                    }
                    ppass++;
                    mset(sram, o);
                    mdirty = true;
                    if ((a >> 1) != last_ta) ops.push_back({1, (uint16_t)QS_SRD});
                    if (!(off & 1) || !prev_drawn) ops.push_back({1, (uint16_t)QS_PIX});
                    ops.push_back({1, (uint16_t)QS_ZPIX});
                    prev_drawn = true;
                }
                if ((a >> 1) != last_ta) { p_trd++; last_ta = a >> 1; }
                if (!(off & 1) || i == 0) p_pwr++;   // a pair: the even pixel opens a halfword write, the odd one joins it
                if (off < 64000) {
                    sram[FB + off] = sram[a & 0x3fffff];
                    memcpy(&sram[ZB + 2 * off], &zv16, 2);
                }
                srd++; pix++; zpix++;
                s = (int32_t)((uint32_t)s + (uint32_t)sstep); t = (int32_t)((uint32_t)t + (uint32_t)tstep);
                Zp = (int32_t)((uint32_t)Zp + (uint32_t)zu);
            }
            s = snext; t = tnext;
        }
        if (mdirty) ops.push_back({1, (uint16_t)QS_ZPIX});
    }
};
