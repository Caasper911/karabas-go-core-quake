// Ф1 Verilator testbench of the real SoC RTL (src/rtl/doom_soc.v: VexRiscv + doom_sdram + doom_sram
// + MMIO), started from an iss --dump-at image (reset vector = the resume stub at the end of the
// SDRAM). C++ models of the board memories at the pins:
//   SDRAM: commands at the rising edge (ACTIVE / READ / WRITE / PRECHARGE / REFRESH / MODE, A10 auto
//     precharge), CAS latency 2 counted in clk (the data of a READ at edge k are on DQ between edges
//     k+2 and k+3, as doom_sdram RD_DELAY 4 takes them), DQM on writes; checks: a READ / WRITE to a
//     bank without its row open, tRCD < 2, a command to a bank precharging (tRP < 2), refresh
//     interval > 7.8 us (in clocks of 75 MHz).
//   SRAM: asynchronous: MD_I after edge k = the halfword at the MA of edge k-1 (10 ns chip: data are
//     valid only a whole clock after an address change), a write while /WE of a lane is low.
// The frame hash (FNV-1a of 0x3f0000..+64000 and the palette) is printed at each FRAME write, as the
// iss prints it: run both on the same dump and compare.
//
// Quake Ф1: built with -DKG_QUAKE (doom_soc WITH_DRAW 0, VexRiscv_FPU): no drawer state; compare the frame
// hashes with tb_soc (the same CPU on memory models), not with the iss (its fmadd is not FpuPlugin's).
// usage: tb_rtl <dump.bin> [--frames N] [--max Mcycles] [--trace-sdram N: print the first N commands]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include "Vdoom_soc.h"
#include "Vdoom_soc___024root.h"
#include "Vdoom_soc_doom_soc.h"
#include "Vdoom_soc_VexRiscv.h"
#include "verilated.h"
// the Verilator of msys2 (Windows) links verilated.cpp that calls it
double sc_time_stamp() { return 0; }

static const uint32_t SRAM_SIZE = 4u << 20, SDRAM_SIZE = 32u << 20;
static std::vector<uint8_t> sram(SRAM_SIZE), sdram(SDRAM_SIZE);

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s dump.bin [--frames N] [--max Mcycles]\n", argv[0]); return 2; }
    int stop_frames = -1, trace = 0;
    int raw_at = -1; const char *raw_path = 0;   // Quake Ф3е: --raw N file: the SRAM 0x3b0000..0x3fffff after the N-th frame write (from 0)
    FILE *prof = nullptr;                  // --prof file: the PC in execute and ra every 256 clocks (prof.py)
    uint32_t us0 = 0;                      // --us N: US / MS of the dump (iss prints it), held during the reset
                                           // (else the time of the resumed firmware jumps back)
    uint64_t max_cycles = 3000ull * 1000 * 1000;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames")) stop_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max")) max_cycles = strtoull(argv[++i], 0, 10) * 1000000ull;
        else if (!strcmp(argv[i], "--trace-sdram")) trace = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prof")) prof = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--us")) us0 = strtoul(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--raw")) { raw_at = atoi(argv[++i]); raw_path = argv[++i]; }
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    uint32_t wad_size = 0;
    size_t n = fread(sram.data(), 1, SRAM_SIZE, f) + fread(sdram.data(), 1, SDRAM_SIZE, f);
    if (fread(&wad_size, 4, 1, f) != 1) wad_size = 0;
    std::vector<uint32_t> drw_cmap(2048, 0);       // Ф1в: the colormaps of the drawer (iss appends them)
    bool has_cmap = fread(drw_cmap.data(), 4, 2048, f) == 2048;
    fclose(f);
    fprintf(stderr, "loaded %zu bytes\n", n);

    Verilated::commandArgs(argc, argv);
    Vdoom_soc *t = new Vdoom_soc;
#ifndef KG_QUAKE   // Quake: doom_soc WITH_DRAW 0 (no drawer)
    if (has_cmap) for (int i = 0; i < 2048; i++) t->rootp->doom_soc->__PVT__g_drw__DOT__drw__DOT__cm_mem[i] = drw_cmap[i];
#else
    (void)has_cmap;
#endif
    t->reset = 1;
    t->ld_kind = 0; t->ld_slot = 0; t->ld_start = 0; t->ld_active = 0; t->ld_end = 0;
    t->ld_addr = 0; t->ld_data = 0; t->ld_wr = 0;
    t->kbd = 0; t->kbd_wr = 0; t->ms_x = 0; t->ms_y = 0; t->ms_b = 0; t->ms_wr = 0; t->joy_l = 0; t->joy_r = 0;
    t->vid_req = 0; t->vid_adr = 0; t->vid_len = 0;
    // Quake Ф7: --vid P [phase]: the video reads the shown buffer from the SRAM, a line of 160 halfwords every P clocks
    // (as doom_video; 0 = none), starting at clock phase
    long vid_p = 0, vid_ph = 0; int vid_line = 0;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--vid")) { vid_p = atol(argv[i + 1]); if (i + 2 < argc && argv[i + 2][0] != '-') vid_ph = atol(argv[i + 2]); }
    t->sdr_dq_i = 0; t->MD_I = 0; t->disp_shown = 0;

    // SDRAM model state
    bool open[4] = {false, false, false, false};
    uint32_t row[4] = {0, 0, 0, 0};
    int64_t act_at[4] = {-100, -100, -100, -100}, pre_at[4] = {-100, -100, -100, -100};
    struct Rd { int64_t at; uint32_t addr; };
    std::vector<Rd> rd;           // reads in flight
    int64_t last_ref = 0;
    int errors = 0, traced = 0;
    uint32_t prev_ma = 0;
    uint32_t pal[256] = {0};
    uint16_t last_frame = 0;
    std::vector<uint8_t> work_copy(64000);
    bool last_front = false;
    int copies = 0, copy_bad = 0;
    int nframes = 0;
    uint64_t cyc = 0, last_cyc = 0;
    uint64_t n_busy = 0, n_hold = 0, n_fence = 0, n_st[8] = {0};   // Ф1в: drawer busy, queue full holds, its states
    uint64_t n_qps[16] = {0}, n_qrs[16] = {0};                     // Quake Ф7: quake_span pixel / rasterizer states
    std::string line;
    uint8_t last_uart = 0;
    auto err = [&](const char *m, int b) {
        if (errors++ < 20) fprintf(stderr, "SDRAM %s bank %d at cycle %llu\n", m, b, (unsigned long long)cyc);
    };

    for (int i = 0; i < 16; i++) { t->clk = 0; t->eval(); t->clk = 1; t->eval(); }
    t->reset = 0;
    bool done = false;
    while (!done && cyc < max_cycles) {
        // the data of the pins for this clock (set before the edges)
        // SDRAM: a READ issued at edge k drives DQ between k+2 and k+3
        t->sdr_dq_i = 0xdead;
        for (auto &r : rd) if (cyc == (uint64_t)r.at + 2) { uint16_t v; memcpy(&v, &sdram[r.addr * 2], 2); t->sdr_dq_i = v; }

        t->vid_req = 0;
        if (vid_p && cyc >= (uint64_t)vid_ph && (cyc - vid_ph) % vid_p == 0) {
            t->vid_req = 1; t->vid_len = 160;
            t->vid_adr = (t->disp_shown ? 0x1f0000u : 0x1e8000u) + (uint32_t)vid_line * 160u;
            vid_line = (vid_line + 1) % 200;
        }
        t->clk = 0; t->eval();
        t->clk = 1; t->eval();
        cyc++;
        {   // also one clock after the reset: that edge still cleared them
            static bool was_reset = true;
            bool r = t->rootp->doom_soc->__PVT__cpu_reset;
            if (us0 && (r || was_reset)) { t->rootp->doom_soc->__PVT__us = us0; t->rootp->doom_soc->__PVT__ms = us0 / 1000; }
            // Quake Ф1: WADSIZE as the dump has it (the pak is read through it: dataslot_read)
            if (r || was_reset) t->rootp->doom_soc->__PVT__wad_size = wad_size;
            was_reset = r;
        }
        {
            auto *s = t->rootp->doom_soc;
#ifndef KG_QUAKE
            if (s->__PVT__drw_busy) n_busy++;
            if (s->__PVT__io_hold) n_hold++;
            auto *c = s->cpu;   // the CPU on the SRAM (cached or 0x9...) while the drawer is busy: the fence
            uint32_t da = c->__PVT__dBusWishbone_ADR;
            bool sr = !(da >> 29 & 1) ? !(da >> 26 & 1) : (da >> 26 & 1);
            if (c->__PVT__dBusWishbone_CYC && c->__PVT__dBusWishbone_STB && sr && s->__PVT__drw_busy) n_fence++;
            n_st[s->__PVT__g_drw__DOT__drw__DOT__st & 7]++;
#else
            // Quake Ф7: quake_span busy, its FIFO holds, the CPU fenced off the z-buffer / frames, the pixel states
            if (s->__PVT__qsp_busy) n_busy++;
            if (s->__PVT__qsp_io_hold) n_hold++;
            auto *c = s->cpu;
            uint32_t da = c->__PVT__dBusWishbone_ADR;
            bool sr = !(da >> 29 & 1) && !(da >> 26 & 1);
            if (c->__PVT__dBusWishbone_CYC && c->__PVT__dBusWishbone_STB && sr && (da & 0xfffff) >= 0xEC000 && s->__PVT__qsp_busy) n_fence++;
            n_qps[s->__PVT__g_qsp__DOT__qsp__DOT__ps & 15]++;
            n_qrs[s->__PVT__g_qsp__DOT__qsp__DOT__rs & 15]++;
#endif
        }
        if (prof && (cyc & 255) == 0) fprintf(prof, "%08x %08x\n", (unsigned) t->rootp->doom_soc->cpu->__PVT__decode_to_execute_PC,
                                                      (unsigned) t->rootp->doom_soc->cpu->RegFilePlugin_regFile[1]);
        // outputs after edge cyc (registered): the command the SDRAM takes at this edge
        uint8_t c = (t->sdr_ras_n << 2) | (t->sdr_cas_n << 1) | t->sdr_we_n;
        int b = t->sdr_ba;
        uint32_t a = t->sdr_a;
        int64_t k = (int64_t)cyc;
        if (c != 7 && trace && traced < trace) { traced++; fprintf(stderr, "CMD %llu c%d b%d a%04x dq%04x dqm%d\n", (unsigned long long)cyc, c, b, a, t->sdr_dq_o, t->sdr_dqm); }
        switch (c) {
        case 3: // ACTIVE
            if (open[b]) err("ACTIVE to an open bank", b);
            if (k - pre_at[b] < 2) err("tRP", b);
            open[b] = true; row[b] = a; act_at[b] = k; break;
        case 5: case 4: { // READ / WRITE
            if (!open[b]) { err("READ/WRITE to a closed bank", b); break; }
            if (k - act_at[b] < 2) err("tRCD", b);
            uint32_t addr = ((uint32_t)b << 22) | (row[b] << 9) | (a & 0x1ff);   // halfword
            if (c == 5) rd.push_back({k, addr});
            else {
                uint16_t v = t->sdr_dq_o;
                if (t->sdr_dq_oe_n != 0) err("WRITE without DQ driven", b);
                if (!(t->sdr_dqm & 1)) sdram[addr * 2] = v & 0xff;
                if (!(t->sdr_dqm & 2)) sdram[addr * 2 + 1] = v >> 8;
            }
            if (a & 0x400) { open[b] = false; pre_at[b] = k + 1; }
            break;
        }
        case 2: // PRECHARGE
            if (a & 0x400) { for (int i = 0; i < 4; i++) { open[i] = false; pre_at[i] = k; } }
            else { open[b] = false; pre_at[b] = k; }
            break;
        case 1: // REFRESH
            for (int i = 0; i < 4; i++) if (open[i]) err("REFRESH with a bank open", i);
            if (last_ref && k - last_ref > 585 + 600) err("refresh interval", 0);
            last_ref = k; break;
        default: break;
        }
        rd.erase(std::remove_if(rd.begin(), rd.end(), [&](const Rd &r) { return k > r.at + 3; }), rd.end());

        // SRAM: a write lane while /WE is low; MD_I = the halfword at the MA of the previous edge
        uint32_t ma = t->MA & 0x1fffff;
        if (!(t->MWR_N & 1) && !(t->MD_OE_N & 0xff)) sram[ma * 2] = t->MD_O & 0xff;
        if (!(t->MWR_N & 2) && !(t->MD_OE_N & 0xff00)) sram[ma * 2 + 1] = t->MD_O >> 8;
        { uint16_t v; memcpy(&v, &sram[prev_ma * 2], 2); t->MD_I = v; }
        prev_ma = ma;

        t->disp_shown = t->disp_front;   // the video flips at once
        if (t->pal_we) pal[t->pal_wa] = t->pal_wd;
        if (t->dbg_uart != last_uart) { last_uart = t->dbg_uart; if (last_uart) { putchar(last_uart); fflush(stdout); } }
        if (t->frame_n != last_frame) {
            last_frame = t->frame_n;
            uint32_t h = 2166136261u;
            for (int i = 0; i < 64000; i++) h = (h ^ sram[0x3f0000 + i]) * 16777619u;
            for (int i = 0; i < 256; i++) h = (h ^ pal[i]) * 16777619u;
            memcpy(work_copy.data(), &sram[0x3f0000], 64000);
            printf("F,%d,%llu,%08x\n", nframes, (unsigned long long)(cyc - last_cyc), h);
            if (raw_at >= 0 && nframes == raw_at) { FILE *rf = fopen(raw_path, "wb"); fwrite(&sram[0x3b0000], 1, 0x50000, rf); fclose(rf); }
            fflush(stdout);
            last_cyc = cyc;
            if (++nframes == stop_frames) done = true;
        }
        if ((bool)t->disp_front != last_front) {
            // a copy is done: the buffer it went to must hold the frame of the FRAME write
            last_front = t->disp_front;
            uint32_t dst = last_front ? 0x3d0000 : 0x3e0000;
            copies++;
            if (memcmp(&sram[dst], work_copy.data(), 64000)) { if (copy_bad++ < 5) fprintf(stderr, "DMA copy %d differs\n", copies); }
        }
        if (t->dbg_exit & 0x100) { printf("EXIT %d\n", t->dbg_exit & 0xff); done = true; }
    }
#ifdef KG_QUAKE
    printf("QSPAN busy %.1f %% FIFO-full holds %.2f %% fence %.2f %%\n", 100.0 * n_busy / cyc, 100.0 * n_hold / cyc, 100.0 * n_fence / cyc);
    printf("QSPAN pixel states:");
    for (int k = 0; k < 13; k++) printf(" %d:%.1f", k, 100.0 * n_qps[k] / cyc);
    printf(" %%\nQSPAN raster states:");
    for (int k = 0; k < 11; k++) printf(" %d:%.1f", k, 100.0 * n_qrs[k] / cyc);
    printf(" %%\n");
#endif
    printf("DRAWER busy %.1f %% queue-full holds %.1f %% fence %.1f %%; states idle %.1f pix %.1f ft %.1f cm %.1f fill %.1f flat %.1f %%\n",
           100.0 * n_busy / cyc, 100.0 * n_hold / cyc, 100.0 * n_fence / cyc, 100.0 * n_st[0] / cyc, 100.0 * n_st[1] / cyc, 100.0 * n_st[2] / cyc,
           100.0 * n_st[3] / cyc, 100.0 * n_st[4] / cyc, 100.0 * n_st[5] / cyc);
    printf("END cycles %llu frames %d sdram errors %d dma copies %d bad %d\n", (unsigned long long)cyc, nframes, errors, copies, copy_bad);
    t->final();
    delete t;
    return (errors || copy_bad) ? 1 : 0;
}
