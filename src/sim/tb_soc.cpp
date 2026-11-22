// Ф0.4 Verilator testbench (from core-pico8 tb_soc.cpp): a VexRiscv variant on C++ models of the
// board memories, started from an iss --dump-at image (reset vector 0x11fff000 = resume stub).
//
// Memories (separate buses, the I and D buses of the CPU share each one, data first):
//   SRAM  4 MB at 0x00000000, async 2M x16: word read = --sr-rd cycles, write = --sr-wr per halfword
//   SDRAM 32 MB at 0x10000000, 16 bit: first word of a run --sd-first, next word of a burst (CTI 010,
//         address + 4) --sd-next, a write --sd-wr
// --wbuf: a write is acked after 1 cycle and the memory stays busy for its cost (a 1-deep posted
// write buffer per memory). --ideal: every access 1 cycle. Costs are in CPU clocks.
// MMIO at 0x80000000 as src/fw/mmio.h (MS = cycles / 50000, WADSIZE from the dump).
// Prints "F,<frame>,<cycles>" per frame and the console.
//
// Quake Ф0: MMIO US / SFX_POS at --mhz (79.545), the PocketQuake debug VRAM / sysreg (0x2/0x4xxxxxxx) answered
// (0x40000004 = cycles), counted as "pocket-io".
// usage: tb_soc <dump.bin> [--sr-rd N] [--sr-wr N] [--sd-first N] [--sd-next N] [--sd-wr N] [--wbuf] [--ideal]
//               [--frames N] [--max Mcycles] [--mhz F]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include "Vcpu.h"
#include "verilated.h"
// the Verilator of msys2 (Windows) links verilated.cpp that calls it
double sc_time_stamp() { return 0; }

static const uint32_t SRAM_SIZE = 4u << 20, SDRAM_BASE = 0x10000000u, SDRAM_SIZE = 32u << 20;
static std::vector<uint8_t> sram(SRAM_SIZE), sdram(SDRAM_SIZE);

static uint8_t *host(uint32_t a, int &reg) {
    if (a < SRAM_SIZE) { reg = 0; return &sram[a]; }
    if ((a >> 28) == 9) { reg = 0; return &sram[a & (SRAM_SIZE - 1)]; }   // Quake Ф3: the SRAM uncached (SRAM_UC)
    if (a - SDRAM_BASE < SDRAM_SIZE) { reg = 1; return &sdram[a - SDRAM_BASE]; }
    reg = -1; return nullptr;
}

struct Port { bool active = false; int left = 0; int reg = -1; };   // one bus transaction in flight
struct Mem { int owner = 0; int busy = 0; uint32_t last = 0xffffffffu; }; // owner 0 none / 1 I / 2 D; busy = posted write

// Quake Ф3: the span drawer (qspan_model.h for the data, at the command write) and its timing: a queue of
// QDEPTH commands (a SPAN / ZSPAN write waits while it is full), per pixel a texel (an SDRAM line fill
// QS_FILL or an SRAM read QS_SRD, both keep the memory busy) then a frame write (SRAM QS_PIX), per z a
// halfword write (QS_ZPIX); the CPU accesses to the SRAM at >= QFENCE (z-buffer + frames) and the FRAME
// write wait while the drawer has work.
#include "qspan_model.h"
// Ф3е: pz = a perspective span (PSPAN): per pixel also a z write, and QSpan::QS_SEG clocks of arithmetic before
// each 16-pixel segment (the reciprocal and the multiplies; no memory)
struct QCmd { bool z; int n; std::vector<uint8_t> miss; bool pz = false; };

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s dump.bin [options]\n", argv[0]); return 2; }
    int sr_rd = 4, sr_wr = 6, sd_first = 8, sd_next = 2, sd_wr = 7;
    bool wbuf = false, ideal = false;
    int stop_frames = -1, nframes = 0;
    uint64_t max_cycles = 2000ull * 1000 * 1000;
    double mhz = 79.545;          // Quake Ф0: the clock for US / SFX_POS
    bool qsp_hold = false;        // Quake Ф3е-б: the surface registers wait for an idle drawer
    int raw_at = -1; const char *raw_path = 0;   // --raw N file: the SRAM 0x3b0000..0x3fffff after the N-th frame write
    uint64_t n_pq = 0;            // Quake Ф0: accesses to the PocketQuake debug VRAM / sysreg
    uint32_t pal[256] = {0};      // Quake Ф1: the palette for the frame hash
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--sr-rd")) sr_rd = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sr-wr")) sr_wr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sd-first")) sd_first = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sd-next")) sd_next = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sd-wr")) sd_wr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wbuf")) wbuf = true;
        else if (!strcmp(argv[i], "--ideal")) ideal = true;
        else if (!strcmp(argv[i], "--frames")) stop_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max")) max_cycles = strtoull(argv[++i], 0, 10) * 1000000ull;
        else if (!strcmp(argv[i], "--mhz")) mhz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--qsp-hold")) qsp_hold = true;
        else if (!strcmp(argv[i], "--raw")) { raw_at = atoi(argv[++i]); raw_path = argv[++i]; }
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    uint32_t wad_size = 0;
    size_t n = fread(sram.data(), 1, SRAM_SIZE, f) + fread(sdram.data(), 1, SDRAM_SIZE, f);
    if (fread(&wad_size, 4, 1, f) != 1) wad_size = 0;
    fclose(f);
    fprintf(stderr, "loaded %zu bytes; sram rd %d wr %d, sdram first %d next %d wr %d%s%s\n", n, sr_rd, sr_wr,
            sd_first, sd_next, sd_wr, wbuf ? " wbuf" : "", ideal ? " ideal" : "");

    Verilated::commandArgs(argc, argv);
    Vcpu *top = new Vcpu;
    top->externalResetVector = 0x11fff000;
    top->timerInterrupt = 0;
    top->softwareInterrupt = 0;
    top->externalInterruptArray = 0;
    top->iBusWishbone_ACK = 0;
    top->dBusWishbone_ACK = 0;
    top->iBusWishbone_ERR = 0;
    top->dBusWishbone_ERR = 0;
    top->reset = 1;
    for (int i = 0; i < 8; i++) { top->clk = 0; top->eval(); top->clk = 1; top->eval(); }
    top->reset = 0;

    uint64_t cyc = 0, last_frame = 0;
    int exit_code = -1;
    bool done = false;
    Port ip, dp;
    QSpan qs;
    std::deque<QCmd> qq;
    int qi = 0, qphase = 0, qwait = 0;
    bool qseg = false;
    const size_t QDEPTH = 4;
    const uint32_t QFENCE = QSpan::ZB;
    uint64_t q_busy_cyc = 0, q_full_cyc = 0, q_fence_cyc = 0;
    Mem mem[2];
    std::string line;

    // cost of a read of word a on memory r (burst continuation on the SDRAM)
    auto rd_cost = [&](int r, uint32_t a, bool burst) {
        if (ideal) return 1;
        if (r == 0) return sr_rd;
        int c = (burst && a == mem[1].last + 4) ? sd_next : sd_first;
        return c;
    };

    while (!done && cyc < max_cycles) {
        top->iBusWishbone_ACK = 0;
        top->dBusWishbone_ACK = 0;
        for (int r = 0; r < 2; r++) if (mem[r].busy > 0) mem[r].busy--;

        bool dreq = top->dBusWishbone_CYC && top->dBusWishbone_STB;
        bool ireq = top->iBusWishbone_CYC && top->iBusWishbone_STB;
        uint32_t daddr = (uint32_t)top->dBusWishbone_ADR << 2;
        uint32_t iaddr = (uint32_t)top->iBusWishbone_ADR << 2;
        bool dio = (daddr >> 28) == 8;

        // Quake Ф3: the drawer takes the memories before the CPU in this clock
        if (qwait > 0) qwait--;
        else if (!qq.empty()) {
            QCmd &c = qq.front();
            if (c.z) {
                if (mem[0].owner == 0 && mem[0].busy == 0) {
                    mem[0].busy = QSpan::QS_ZPIX; qwait = QSpan::QS_ZPIX - 1;
                    if (++qi == c.n) { qq.pop_front(); qi = 0; }
                }
            } else {
                if (qphase == 0 && c.pz && (qi & 15) == 0 && !qseg) { qseg = true; qwait = QSpan::QS_SEG - 1; }
                else if (qphase == 0) {
                    uint8_t m = c.miss[qi];
                    if (m == 0) qphase = 1;
                    else {
                        int r = m == 1 ? 1 : 0, cost = m == 1 ? QSpan::QS_FILL : QSpan::QS_SRD;
                        if (mem[r].owner == 0 && mem[r].busy == 0) { mem[r].busy = cost; qwait = cost - 1; qphase = 1; }
                    }
                } else if (qphase == 1 && mem[0].owner == 0 && mem[0].busy == 0) {
                    mem[0].busy = QSpan::QS_PIX; qwait = QSpan::QS_PIX - 1;
                    qphase = c.pz ? 2 : 0;
                    if (!c.pz) { qseg = false; if (++qi == c.n) { qq.pop_front(); qi = 0; } }
                } else if (qphase == 2 && mem[0].owner == 0 && mem[0].busy == 0) {
                    mem[0].busy = QSpan::QS_ZPIX; qwait = QSpan::QS_ZPIX - 1; qphase = 0;
                    if (((qi + 1) & 15) == 0) qseg = false;
                    if (++qi == c.n) { qq.pop_front(); qi = 0; qseg = false; }
                }
            }
        }
        bool qbusy = !qq.empty();
        if (qbusy) q_busy_cyc++;

        // data bus
        if (dreq) {
            if (!dp.active) {
                if (dio) { dp.active = true; dp.left = 1; dp.reg = -1; }
                else if ((daddr >> 28) == 2 || (daddr >> 28) == 4) { dp.active = true; dp.left = 1; dp.reg = -2; n_pq++; } // Quake: PocketQuake debug VRAM / sysreg
                else {
                    int r; if (!host(daddr, r)) { fprintf(stderr, "D access %08x\n", daddr); return 3; }
                    bool fenced = qbusy && r == 0 && (daddr & (SRAM_SIZE - 1)) >= QFENCE;   // Quake Ф3
                    if (fenced) q_fence_cyc++;
                    if (mem[r].owner == 0 && mem[r].busy == 0 && !fenced) {
                        mem[r].owner = 2; dp.active = true; dp.reg = r;
                        if (top->dBusWishbone_WE) {
                            int hw = ((top->dBusWishbone_SEL & 3) ? 1 : 0) + ((top->dBusWishbone_SEL & 12) ? 1 : 0);
                            int c = ideal ? 1 : r == 0 ? hw * sr_wr : sd_wr;
                            if (wbuf && !ideal) { dp.left = 1; mem[r].busy = c; } else dp.left = c;
                            mem[r].last = 0xffffffffu;
                        } else {
                            dp.left = rd_cost(r, daddr, top->dBusWishbone_CTI == 2 || mem[r].last != 0xffffffffu);
                            mem[r].last = daddr;
                        }
                    }
                }
            }
            // Quake Ф3: a SPAN / ZSPAN write waits while the queue is full, the FRAME write until it is empty
            if (dp.active && dio && dp.left == 1 && top->dBusWishbone_WE) {
                uint32_t o = daddr & 0x3fff;
                if (((o == 0x120 || o == 0x124 || o == 0x178) && qq.size() >= QDEPTH) || (o == 0x04 && qbusy)) { dp.left = 2; q_full_cyc++; }
                // Ф3е-б (--qsp-hold): the surface registers (TEX / TW, 0x140..0x174) wait while the drawer has work
                else if (qsp_hold && qbusy && (o == 0x100 || o == 0x104 || (o >= 0x140 && o <= 0x174))) { dp.left = 2; q_full_cyc++; }
            }
            if (dp.active && --dp.left == 0) {
                dp.active = false;
                if (dp.reg >= 0) mem[dp.reg].owner = 0;
                top->dBusWishbone_ACK = 1;
                if (dio) {
                    // Quake Ф0: the full offset (the palette at 0x400.., the SFX ring at 0x2000.. aliased FRAME / EXIT)
                    uint32_t o = daddr & 0x3fff, v = top->dBusWishbone_DAT_MOSI;
                    if (top->dBusWishbone_WE) {
                        if (o == 0x00) { char c = (char)(v & 0xff); if (c == '\n') { printf("%s\n", line.c_str()); line.clear(); } else line += c; }
                        else if (o == 0x04) {
                            // Quake Ф1: + the frame hash (FNV-1a of 0x3f0000..+64000 and the palette), as iss / tb_rtl
                            uint32_t h = 2166136261u;
                            for (int i = 0; i < 64000; i++) h = (h ^ sram[0x3f0000 + i]) * 16777619u;
                            for (int i = 0; i < 256; i++) h = (h ^ pal[i]) * 16777619u;
                            printf("F,%u,%llu,%08x\n", v, (unsigned long long)(cyc - last_frame), h);
                            if (raw_at >= 0 && nframes == raw_at) { FILE *rf = fopen(raw_path, "wb"); fwrite(&sram[0x3b0000], 1, 0x50000, rf); fclose(rf); }
                            fflush(stdout); last_frame = cyc; if (++nframes == stop_frames) done = true;
                        }
                        else if (o >= 0x400 && o < 0x800) pal[(o - 0x400) >> 2] = v;
                        else if (o >= 0x100 && o < 0x180) {   // Quake Ф3: the drawer (Ф3е: 0x140..)
                            qs.write(o, v, sram.data(), sdram.data(), SDRAM_SIZE - 1);
                            if (o == 0x120) qq.push_back({false, (int)((v >> 16) & 0x1ff) + 1, qs.miss});
                            if (o == 0x124) qq.push_back({true, (int)((v >> 16) & 0x1ff) + 1, {}});
                            if (o == 0x178) { QCmd q{false, (int)((v >> 17) & 0x1ff) + 1, qs.miss}; q.pz = true; qq.push_back(q); }
                        }
                        else if (o == 0x08) { exit_code = (int)v; done = true; }
                    } else {
                        // Quake Ф0: US / SFX_POS at --mhz (Sys_FloatTime, the sound DMA position)
                        uint64_t us = (uint64_t)(cyc / mhz);
                        top->dBusWishbone_DAT_MISO = o == 0x0c ? (uint32_t)cyc : o == 0x1c ? (uint32_t)(cyc / 50000) : o == 0x20 ? wad_size :
                                                     o == 0x13c ? (QSpan::stat() | (qbusy ? 1u : 0u)) :
                                                     o == 0x40 ? (uint32_t)us : o == 0x70 ? (uint32_t)(us * 11025 / 1000000) & 2047 : 0;
                    }
                } else if (dp.reg == -2) {
                    top->dBusWishbone_DAT_MISO = daddr == 0x40000004u ? (uint32_t)cyc : 0;
                } else {
                    int r; uint8_t *p = host(daddr, r);
                    if (top->dBusWishbone_WE) {
                        uint32_t v = top->dBusWishbone_DAT_MOSI;
                        for (int b = 0; b < 4; b++) if (top->dBusWishbone_SEL & (1 << b)) p[b] = (v >> (8 * b)) & 0xff;
                    } else {
                        uint32_t v; memcpy(&v, p, 4); top->dBusWishbone_DAT_MISO = v;
                    }
                }
            }
        }
        // instruction bus
        if (ireq) {
            int r; if (!host(iaddr, r)) { fprintf(stderr, "I access %08x\n", iaddr); return 3; }
            bool d_wants = dreq && !dp.active && !dio;
            if (!ip.active && mem[r].owner == 0 && mem[r].busy == 0 && !(d_wants && host(daddr, dp.reg) && dp.reg == r)) {
                mem[r].owner = 1; ip.active = true; ip.reg = r;
                ip.left = rd_cost(r, iaddr, true);
                mem[r].last = iaddr;
            }
            if (ip.active && --ip.left == 0) {
                ip.active = false; mem[ip.reg].owner = 0;
                uint32_t v; memcpy(&v, host(iaddr, r), 4);
                top->iBusWishbone_DAT_MISO = v;
                top->iBusWishbone_ACK = 1;
            }
        }
        // the end of a bus cycle closes the SDRAM burst (next read pays sd_first)
        if (!ireq && !dreq) mem[1].last = 0xffffffffu;
        top->clk = 0; top->eval();
        top->clk = 1; top->eval();
        cyc++;
    }
    if (!line.empty()) printf("%s\n", line.c_str());
    printf("QSPAN cmds %llu z %llu pixels %llu z %llu fills %llu sram reads %llu; busy %.1f %%, CPU waits: queue / frame %.2f %%, SRAM fence %.2f %%\n",
           (unsigned long long)qs.cmds, (unsigned long long)qs.zcmds, (unsigned long long)qs.pix, (unsigned long long)qs.zpix,
           (unsigned long long)qs.fills, (unsigned long long)qs.srd, 100.0 * q_busy_cyc / cyc, 100.0 * q_full_cyc / cyc, 100.0 * q_fence_cyc / cyc);
    printf("END cycles %llu exit %d%s pocket-io %llu\n", (unsigned long long)cyc, exit_code, done ? "" : " (max)",
           (unsigned long long)n_pq);
    top->final();
    delete top;
    return done ? 0 : 1;
}
