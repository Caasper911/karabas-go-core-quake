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
#include "Vcpu.h"
#include "verilated.h"
// the Verilator of msys2 (Windows) links verilated.cpp that calls it
double sc_time_stamp() { return 0; }

static const uint32_t SRAM_SIZE = 4u << 20, SDRAM_BASE = 0x10000000u, SDRAM_SIZE = 32u << 20;
static std::vector<uint8_t> sram(SRAM_SIZE), sdram(SDRAM_SIZE);

static uint8_t *host(uint32_t a, int &reg) {
    if (a < SRAM_SIZE) { reg = 0; return &sram[a]; }
    if (a - SDRAM_BASE < SDRAM_SIZE) { reg = 1; return &sdram[a - SDRAM_BASE]; }
    reg = -1; return nullptr;
}

struct Port { bool active = false; int left = 0; int reg = -1; };   // one bus transaction in flight
struct Mem { int owner = 0; int busy = 0; uint32_t last = 0xffffffffu; }; // owner 0 none / 1 I / 2 D; busy = posted write

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s dump.bin [options]\n", argv[0]); return 2; }
    int sr_rd = 4, sr_wr = 6, sd_first = 8, sd_next = 2, sd_wr = 7;
    bool wbuf = false, ideal = false;
    int stop_frames = -1, nframes = 0;
    uint64_t max_cycles = 2000ull * 1000 * 1000;
    double mhz = 79.545;          // Quake Ф0: the clock for US / SFX_POS
    uint64_t n_pq = 0;            // Quake Ф0: accesses to the PocketQuake debug VRAM / sysreg
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
        bool dio = daddr & 0x80000000u;

        // data bus
        if (dreq) {
            if (!dp.active) {
                if (dio) { dp.active = true; dp.left = 1; dp.reg = -1; }
                else if ((daddr >> 28) == 2 || (daddr >> 28) == 4) { dp.active = true; dp.left = 1; dp.reg = -2; n_pq++; } // Quake: PocketQuake debug VRAM / sysreg
                else {
                    int r; if (!host(daddr, r)) { fprintf(stderr, "D access %08x\n", daddr); return 3; }
                    if (mem[r].owner == 0 && mem[r].busy == 0) {
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
            if (dp.active && --dp.left == 0) {
                dp.active = false;
                if (dp.reg >= 0) mem[dp.reg].owner = 0;
                top->dBusWishbone_ACK = 1;
                if (dio) {
                    uint32_t o = daddr & 0xff, v = top->dBusWishbone_DAT_MOSI;
                    if (top->dBusWishbone_WE) {
                        if (o == 0x00) { char c = (char)(v & 0xff); if (c == '\n') { printf("%s\n", line.c_str()); line.clear(); } else line += c; }
                        else if (o == 0x04) { printf("F,%u,%llu\n", v, (unsigned long long)(cyc - last_frame)); fflush(stdout); last_frame = cyc; if (++nframes == stop_frames) done = true; }
                        else if (o == 0x08) { exit_code = (int)v; done = true; }
                    } else {
                        // Quake Ф0: US / SFX_POS at --mhz (Sys_FloatTime, the sound DMA position)
                        uint64_t us = (uint64_t)(cyc / mhz);
                        top->dBusWishbone_DAT_MISO = o == 0x0c ? (uint32_t)cyc : o == 0x1c ? (uint32_t)(cyc / 50000) : o == 0x20 ? wad_size :
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
    printf("END cycles %llu exit %d%s pocket-io %llu\n", (unsigned long long)cyc, exit_code, done ? "" : " (max)",
           (unsigned long long)n_pq);
    top->final();
    delete top;
    return done ? 0 : 1;
}
