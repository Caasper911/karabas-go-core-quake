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
        case 0x124: {
            uint32_t off = v & 0xffff, n = ((v >> 16) & 0x1ff) + 1, z = izi;
            zcmds++;
            for (uint32_t i = 0; i < n; i++, off++) {
                if (off < 64000) { uint16_t h = (uint16_t)(z >> 16); memcpy(&sram[ZB + 2 * off], &h, 2); }
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
    static int32_t clampi(int64_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : (int32_t)x; }

    void persp(uint32_t v, uint8_t *sram) {
        static const int recip[16] = { 0, 65536, 32768, 21845, 16384, 13107, 10923, 9362,
                                       8192, 7282, 6554, 5958, 5461, 5041, 4681, 4369 };
        int u0 = v & 0x1ff, v0 = (v >> 9) & 0xff, count = ((v >> 17) & 0x1ff) + 1;
        int32_t S = so + u0 * su + v0 * sv, T = to_ + u0 * tu + v0 * tv, Z = zo + u0 * zu + v0 * zv;
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
                S += su * 16; T += tu * 16; Z += zu * 16;
                snext = clampi(persp_q(S, Z, ks, kz) + sadj, 16, bbs);
                tnext = clampi(persp_q(T, Z, kt, kz) + tadj, 16, bbt);
                sstep = (snext - s) >> 4;
                tstep = (tnext - t) >> 4;
            } else {
                S += su * (seg - 1); T += tu * (seg - 1); Z += zu * (seg - 1);
                snext = clampi(persp_q(S, Z, ks, kz) + sadj, 8, bbs);
                tnext = clampi(persp_q(T, Z, kt, kz) + tadj, 8, bbt);
                if (seg > 1) {
                    sstep = (int32_t)(((int64_t)(snext - s) * recip[seg - 1]) >> 16);
                    tstep = (int32_t)(((int64_t)(tnext - t) * recip[seg - 1]) >> 16);
                }
            }
            psegs++; last_segs++;
            for (int i = 0; i < seg; i++, off++) {
                uint32_t a = tex + (uint32_t)(s >> 16) + (uint32_t)(t >> 16) * tw;
                if ((a >> 1) != last_ta) { p_trd++; last_ta = a >> 1; }
                if (!(off & 1) || i == 0) p_pwr++;   // a pair: the even pixel opens a halfword write, the odd one joins it
                uint16_t zv16 = (uint16_t)(kz >= 15 ? (Zp >> (kz - 15)) : (Zp << (15 - kz)));
                if (off < 64000) {
                    sram[FB + off] = sram[a & 0x3fffff];
                    memcpy(&sram[ZB + 2 * off], &zv16, 2);
                }
                srd++; pix++; zpix++;
                s += sstep; t += tstep; Zp += zu;
            }
            s = snext; t = tnext;
        }
    }
};
