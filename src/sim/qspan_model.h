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

#ifndef QS_NLINES
#define QS_NLINES 4
#endif

struct QSpan {
    static const uint32_t ZB = 0x3b0000u, FB = 0x3f0000u;
    static const int QS_LINES = QS_NLINES, QS_PIX = 3, QS_ZPIX = 3, QS_FILL = 28, QS_SRD = 2;
    uint32_t tex = 0, tw = 0, izi = 0, izistep = 0;
    int32_t s = 0, t = 0, sstep = 0, tstep = 0;
    uint32_t tag[QS_LINES];
    int victim = 0;
    uint64_t cmds = 0, zcmds = 0, pix = 0, zpix = 0, fills = 0, srd = 0;
    std::vector<uint8_t> miss;   // the last SPAN command: per pixel 0 hit, 1 SDRAM line fill, 2 SRAM texel read
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
};
