// Ф0 fast RV32IM instruction-set simulator for the Doom bench firmware (from core-pico8 iss.cpp).
// Memory: SRAM 4 MB at 0x00000000, SDRAM 32 MB at 0x10000000, MMIO at 0x80000000 (uncached).
// No timing: MMIO_CYCLES = retired instructions, MMIO_MS = instructions / 50000. Cache model
// (--icache/--dcache bytes, default 32K / 32K as the core since Hexen Ф1б, 32-byte lines, direct-mapped as VexRiscv 1-way, D$ write-through
// without write allocate) counts per frame and per memory: I$ misses, D$ read misses, stores
// (bytes / halfwords / words), so model.py can price each memory without Verilator.
//
// Quake Ф0: RV32F (FF,<frame>,... per frame and an FP line at the end), the PocketQuake debug VRAM / sysreg
// (0x2xxxxxxx / 0x4xxxxxxx: writes dropped, reads 0, 0x40000004 = cycles; "pocket-io" at the end).
// usage: iss <fw.bin> --org 0x10000000 [--wad file] [--max Minstr] [--icache B] [--dcache B]
//            [--prof file: pc + ra every 256 instr] [--prof-from F]
//            [--dump-at F file: after frame F write SRAM + SDRAM (36 MB) with a resume stub at 0x11fff000]
//            [--resume file: start from such a dump (registers from it, cold caches), fw.bin ignored]
//            [--frames N: stop after N frames]
//            Ф2: [--music N: MMIO_SW bits 15..8] [--fps N: MMIO_SW bits 7..0 (the OSD item FPS)] [--wav file: the SFX ring as played, 11025 Hz stereo]
//            [--opl-log file: "us reg value" of the OPL writes] [--midi-log file: "us byte"]
//            Ф3: [--sd file: the 1 MB save file of doom_sd (made if absent)] [--keys F:U,F:U...: at frame F the
//            USB key U is down (0: all up)]
//            Timer: US = instructions / 32 (~ CPI 2.4 at 75 MHz); the timer interrupt and the CSRs
//            mstatus / mie / mtvec / mepc / mcause / mtval / mip, mret.
//            [--shot F file.ppm: the frame F (FB 0x3f0000 + palette) as a PPM, repeatable]
// prints per frame: "F,<frame>,<instr>,<im_sram>,<im_sdram>,<dm_sram>,<dm_sdram>,<st_sram>,<st_sdram>,<st8_sram>,<st8_sdram>,<fb hash>,
//   <uncached SRAM loads>,<drawer columns>,<drawer spans>,<drawer pixels>" (Ф1в: SRAM 0x9xxxxxxx uncached, the drawer at MMIO C0..DC)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>

static const uint32_t SRAM_SIZE = 4u << 20, SDRAM_BASE = 0x10000000u, SDRAM_SIZE = 32u << 20;
static std::vector<uint8_t> sram(SRAM_SIZE + 8), sdram(SDRAM_SIZE + 8);

// region of an address: 0 SRAM, 1 SDRAM; anything else stops the run
static inline uint8_t *host(uint32_t a, int &reg) {
    if (a < SRAM_SIZE) { reg = 0; return &sram[a]; }
    if (a - SDRAM_BASE < SDRAM_SIZE) { reg = 1; return &sdram[a - SDRAM_BASE]; }
    reg = -1; return nullptr;
}

struct Cache {
    uint32_t lines = 0;
    std::vector<uint32_t> tag;
    void init(uint32_t bytes) { lines = bytes / 32; tag.assign(lines ? lines : 1, 0xffffffffu); }
    bool miss(uint32_t a) {
        if (!lines) return true;
        uint32_t l = a >> 5, i = l % lines;
        if (tag[i] != l) { tag[i] = l; return true; }
        return false;
    }
    bool hit(uint32_t a) const { return lines && tag[(a >> 5) % lines] == (a >> 5); }
};

static uint64_t misaligned = 0;
struct Cnt { uint64_t instr = 0, im[2] = {0, 0}, dm[2] = {0, 0}, st[2] = {0, 0}, st8[2] = {0, 0},
             uc = 0, dcol = 0, dspan = 0, dpx = 0; };   // uncached SRAM accesses; the drawer: commands, pixels

// Ф1в: the drawer of doom_draw.v (functional, at once): MMIO C0..DC (src/fw/mmio.h). The frame is the
// work buffer FB 0x3f0000; the colormaps table (COLORMAP / FOGMAP, 8704 bytes) is a copy uploaded by the CPU.
struct Drawer {
    uint32_t w0 = 0, frac = 0, step = 0, cm_adr = 0;
    uint8_t cmap[16384] = {0};
    void column(uint32_t src) {           // src: SDRAM byte offset of the texture column
        uint32_t off = w0 & 0xffff, n = ((w0 >> 16) & 0x1ff) + 1, cm = ((w0 >> 25) & 0x3f) << 8, f = frac;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t t = sdram[(src + ((f >> 16) & 127)) & (SDRAM_SIZE - 1)];
            if (off < 64000) sram[0x3f0000 + off] = cmap[(cm + t) & 16383];
            off += 320; f += step;
        }
    }
    void span(uint32_t src) {             // src: SDRAM byte offset of the 64x64 flat
        uint32_t off = w0 & 0xffff, n = ((w0 >> 16) & 0x1ff) + 1, cm = ((w0 >> 25) & 0x3f) << 8, p = frac;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t spot = ((p >> 4) & 0x0fc0) | (p >> 26);
            uint8_t t = sdram[(src + spot) & (SDRAM_SIZE - 1)];
            if (off < 64000) sram[0x3f0000 + off] = cmap[(cm + t) & 16383];
            off += 1; p += step;
        }
    }
};
static Drawer drw;

// Quake Ф0: RV32F (single precision; IEEE arithmetic of the host, round to nearest even except the
// fcvt.w(u).s rounding modes; NaN results canonical 0x7fc00000; fflags not kept). Counts per kind
// for the FPU model (VexRiscv FpuPlugin latencies: model.py).
static uint32_t fr[32], fcsr_v, fdummy;
enum { FC_LD, FC_ST, FC_ADD, FC_MUL, FC_DIV, FC_SQRT, FC_FMA, FC_CVT, FC_CMP, FC_MV, FC_MINMAX, FC_N };
static const char *fc_name[FC_N] = { "flw", "fsw", "fadd/sub", "fmul", "fdiv", "fsqrt", "fmadd", "fcvt", "fcmp", "fmv/sgnj/class", "fmin/max" };
static uint64_t fcnt[FC_N];
static inline float F(uint32_t r) { float v; memcpy(&v, &fr[r], 4); return v; }
static inline uint32_t U(float v) { uint32_t u; if (std::isnan(v)) return 0x7fc00000u; memcpy(&u, &v, 4); return u; }
static inline uint32_t fbits(float v) { uint32_t u; memcpy(&u, &v, 4); return u; }
static uint32_t fcvt_w(float v, int rm, bool uns) {
    if (rm == 7) rm = (fcsr_v >> 5) & 7;
    if (std::isnan(v)) return uns ? 0xffffffffu : 0x7fffffffu;
    double d = v;
    switch (rm) {
    case 1: d = std::trunc(d); break;          // RTZ
    case 2: d = std::floor(d); break;          // RDN
    case 3: d = std::ceil(d); break;           // RUP
    case 4: d = std::round(d); break;          // RMM
    default: d = std::nearbyint(d); break;     // RNE
    }
    if (uns) { if (d <= 0) return 0; if (d >= 4294967295.0) return 0xffffffffu; return (uint32_t)d; }
    if (d <= -2147483648.0) return 0x80000000u;
    if (d >= 2147483647.0) return 0x7fffffffu;
    return (uint32_t)(int32_t)d;
}
static uint32_t fclass(float v) {
    uint32_t u = fbits(v); bool s = u >> 31;
    if (std::isinf(v)) return s ? 1u : 128u;
    if (std::isnan(v)) return (u & 0x00400000u) ? 512u : 256u;
    if (v == 0) return s ? 8u : 16u;
    if (std::fpclassify(v) == FP_SUBNORMAL) return s ? 4u : 32u;
    return s ? 2u : 64u;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s fw.bin --org A [--wad f] [--max Minstr] [--icache B] [--dcache B] [--prof f] [--prof-from F]\n", argv[0]); return 2; }
    uint64_t max_instr = 20000ull * 1000 * 1000;
    uint32_t icb = 32768, dcb = 32768, org = SDRAM_BASE;
    const char *wad = 0;
    FILE *prof = 0;
    bool sampling = true;
    int prof_from = -1;
    int dump_at = -1, stop_frames = -1, nframes = 0;
    const char *dump_path = 0, *resume = 0;
    bool dump_now = false;
    int music = 0, fps_sw = 0;
    FILE *wavf = 0, *oplf = 0, *midif = 0;
    uint32_t ring[2048] = {0};
    uint64_t emitted = 0, wav_n = 0;
    uint32_t tcmp = 0, opl_addr = 0; bool ten = false;
    uint32_t mstatus = 0, mie = 0, mtvec = 0, mepc = 0, mcause = 0, mtval = 0;
    uint64_t n_opl = 0, n_midi = 0, n_irq = 0, n_pq = 0;
    const char *sd_path = 0;
    std::vector<uint8_t> sdf;
    uint8_t sdbuf[512] = {0};
    uint32_t sd_sector = 0, kbd0 = 0, kbdn = 0;
    bool sd_mounted = false;
    std::vector<std::pair<int, int>> keys;
    std::vector<std::pair<int, std::string>> shots;
    uint32_t pal[256] = {0};
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--max")) max_instr = strtoull(argv[++i], 0, 10) * 1000000ull;
        else if (!strcmp(argv[i], "--icache")) icb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dcache")) dcb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--org")) org = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--wad")) wad = argv[++i];
        else if (!strcmp(argv[i], "--prof")) prof = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--prof-from")) prof_from = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-at")) { dump_at = atoi(argv[++i]); dump_path = argv[++i]; }
        else if (!strcmp(argv[i], "--resume")) resume = argv[++i];
        else if (!strcmp(argv[i], "--frames")) stop_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--music")) music = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps")) fps_sw = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sd")) sd_path = argv[++i];
        else if (!strcmp(argv[i], "--keys")) {
            char *k = argv[++i];
            while (*k) { int f = strtol(k, &k, 10); if (*k == ':') k++; int u = strtol(k, &k, 0); keys.push_back({f, u}); if (*k == ',') k++; }
        }
        else if (!strcmp(argv[i], "--wav")) { wavf = fopen(argv[++i], "wb"); fseek(wavf, 44, SEEK_SET); }
        else if (!strcmp(argv[i], "--opl-log")) oplf = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--midi-log")) midif = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--shot")) { int fn = atoi(argv[++i]); shots.push_back({fn, argv[++i]}); }
    }
    if (prof_from >= 0) sampling = false;
    if (resume) { org = 0; }
    int reg;
    size_t n = 0;
    uint32_t wad_size = 0;
    if (!resume) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        n = fread(host(org, reg), 1, 16u << 20, f);
        fclose(f);
    } else {
        FILE *f = fopen(resume, "rb");
        if (!f) { perror(resume); return 2; }
        n = fread(sram.data(), 1, SRAM_SIZE, f) + fread(sdram.data(), 1, SDRAM_SIZE, f);
        fread(&wad_size, 4, 1, f);
        fclose(f);
    }
    if (wad && !resume) {
        FILE *w = fopen(wad, "rb");
        if (!w) { perror(wad); return 2; }
        wad_size = fread(&sdram[0x00c00000], 1, 20u << 20, w);
        fclose(w);
    }
    fprintf(stderr, "loaded %zu bytes at %08x, WAD %u bytes, I$ %u D$ %u\n", n, org, wad_size, icb, dcb);
    if (sd_path) {
        sdf.assign(1u << 20, 0);
        if (FILE *q = fopen(sd_path, "rb")) { size_t r = fread(sdf.data(), 1, sdf.size(), q); (void)r; fclose(q); }
    }
    Cache ic, dc; ic.init(icb); dc.init(dcb);

    uint32_t x[32] = {0}, pc = org;
    const uint32_t RES = 0x11fff000; // resume area (the last 4 KB of the SDRAM: the CPU fetches from the SDRAM only): stub, registers at +0x100, pc at +0x180
    if (resume) {
        memcpy(x, &sdram[RES - SDRAM_BASE + 0x100], 128); x[0] = 0; memcpy(&pc, &sdram[RES - SDRAM_BASE + 0x180], 4);
        memcpy(fr, &sdram[RES - SDRAM_BASE + 0x300], 128); memcpy(&fcsr_v, &sdram[RES - SDRAM_BASE + 0x380], 4);  // Quake: F
    }
    uint64_t ninstr = 0;
    Cnt tot, last;
    int exit_code = 0;
    bool done = false;
    std::string line;

    auto bad = [&](const char *what, uint32_t a) {
        fprintf(stderr, "%s %08x at pc %08x ra %08x after %llu\n", what, a, pc, x[1], (unsigned long long)ninstr);
        exit(3);
    };
    auto ld = [&](uint32_t a, int sz) -> uint32_t {
        if ((a >> 28) == 2 || (a >> 28) == 4) {           // Quake: PocketQuake debug VRAM / sysreg (0x40000004 = cycles)
            n_pq++;
            return a == 0x40000004u ? (uint32_t)ninstr : 0u;
        }
        if ((a & 0xf0000000u) == 0x90000000u) {          // Ф1в: the SRAM uncached (the frame)
            uint32_t v = 0; tot.uc++;
            memcpy(&v, &sram[a & (SRAM_SIZE - 1)], sz);
            return v;
        }
        if (a & 0x80000000u) {
            if ((a & 0x3fff) == 0xdc) return 0xd7000000u; // drawer: id, idle
            if ((a & 0x3fff) >= 0x1000 && (a & 0x3fff) < 0x1800) return sdbuf[((a & 0x3fff) - 0x1000) >> 2];
            if ((a & 0x3fff) >= 0x100) return 0;
            switch (a & 0xff) {
            case 0x0c: return (uint32_t)ninstr;
            case 0x1c: return (uint32_t)(ninstr / 32000);
            case 0x40: return (uint32_t)(ninstr / 32);
            case 0x50: return (uint32_t)music << 8 | (uint32_t)fps_sw;
            case 0x70: return (uint32_t)((ninstr / 32) * 11025 / 1000000) & 2047;
            case 0x28: return kbd0;
            case 0x30: return kbdn;
            case 0x88: return sd_mounted ? 4u : 0u;
            case 0x8c: return sd_mounted ? (uint32_t)sdf.size() : 0u;
            case 0x20: return wad_size;
            }
            return 0;
        }
        uint8_t *p = host(a, reg);
        if (!p) bad("load from", a);
        if (a & (sz - 1)) { if (!misaligned++) fprintf(stderr, "MISALIGNED load %d at %08x pc %08x ra %08x\n", sz, a, pc, x[1]); }
        if (dc.miss(a)) tot.dm[reg]++;
        uint32_t v = 0;
        memcpy(&v, p, sz);
        return v;
    };
    auto st = [&](uint32_t a, uint32_t v, int sz) {
        if ((a >> 28) == 2 || (a >> 28) == 4) { n_pq++; return; }   // Quake: PocketQuake debug VRAM / sysreg
        if ((a & 0xf0000000u) == 0x90000000u) {          // Ф1в: the SRAM uncached (the frame)
            tot.st[0]++; if (sz < 4) tot.st8[0]++;
            memcpy(&sram[a & (SRAM_SIZE - 1)], &v, sz);
            return;
        }
        if (a & 0x80000000u) {
            uint32_t o = a & 0x3fff;
            switch (o) {                                  // Ф1в: the drawer
            case 0xc0: drw.w0 = v; return;
            case 0xc4: drw.frac = v; return;
            case 0xc8: drw.step = v; return;
            case 0xcc: drw.column(v); tot.dcol++; tot.dpx += ((drw.w0 >> 16) & 0x1ff) + 1; return;
            case 0xd0: drw.span(v); tot.dspan++; tot.dpx += ((drw.w0 >> 16) & 0x1ff) + 1; return;
            case 0xd4: memcpy(&drw.cmap[drw.cm_adr & 16380], &v, 4); drw.cm_adr += 4; return;
            case 0xd8: drw.cm_adr = v; return;
            case 0xdc: return;                            // the flat cache: forget it
            }
            if (o >= 0x2000) { ring[(o - 0x2000) >> 2] = v; return; }
            if (o >= 0x1000 && o < 0x1800) { sdbuf[(o - 0x1000) >> 2] = v & 0xff; return; }
            if (o == 0x80) { sd_sector = v; return; }
            if (o == 0x90) { sd_mounted = sd_path != 0; return; }
            if (o == 0x84 && sd_mounted && sd_sector < sdf.size() / 512) {
                if (v & 1) memcpy(sdbuf, &sdf[sd_sector * 512], 512);
                if (v & 2) memcpy(&sdf[sd_sector * 512], sdbuf, 512);
                return;
            }
            if (o == 0x00) { char c = (char)v; if (c == '\n') { printf("%s\n", line.c_str()); line.clear(); } else line += c; }
            else if (o >= 0x400 && o < 0x800) pal[(o - 0x400) >> 2] = v;
            else if (o == 0x44) tcmp = v;
            else if (o == 0x48) ten = v & 1;
            else if (o == 0x60) opl_addr = v & 0x1ff;
            else if (o == 0x64) { n_opl++; if (oplf) fprintf(oplf, "%llu %03x %02x\n", (unsigned long long)(ninstr / 32), opl_addr, v & 0xff); }
            else if (o == 0x68) { n_midi++; if (midif) fprintf(midif, "%llu %02x\n", (unsigned long long)(ninstr / 32), v & 0xff); }
            else if (o == 0x04) {
                // FNV-1a of the frame + palette: the oracle for the RTL (same value from tb_soc)
                uint32_t h = 2166136261u;
                for (int k = 0; k < 64000; k++) h = (h ^ sram[0x3f0000 + k]) * 16777619u;
                for (int k = 0; k < 256; k++) h = (h ^ pal[k]) * 16777619u;
                for (auto &sh : shots) if (sh.first == (int)v) {
                    FILE *pp = fopen(sh.second.c_str(), "wb");
                    fprintf(pp, "P6 320 200 255\n");
                    for (int k = 0; k < 64000; k++) { uint32_t c = pal[sram[0x3f0000 + k]]; fputc(c >> 16, pp); fputc((c >> 8) & 255, pp); fputc(c & 255, pp); }
                    fclose(pp);
                }
                tot.instr = ninstr;
                printf("F,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%08x,%llu,%llu,%llu,%llu\n", v,
                       (unsigned long long)(tot.instr - last.instr),
                       (unsigned long long)(tot.im[0] - last.im[0]), (unsigned long long)(tot.im[1] - last.im[1]),
                       (unsigned long long)(tot.dm[0] - last.dm[0]), (unsigned long long)(tot.dm[1] - last.dm[1]),
                       (unsigned long long)(tot.st[0] - last.st[0]), (unsigned long long)(tot.st[1] - last.st[1]),
                       (unsigned long long)(tot.st8[0] - last.st8[0]), (unsigned long long)(tot.st8[1] - last.st8[1]), h,
                       (unsigned long long)(tot.uc - last.uc), (unsigned long long)(tot.dcol - last.dcol),
                       (unsigned long long)(tot.dspan - last.dspan), (unsigned long long)(tot.dpx - last.dpx));
                last = tot;
                {   // Quake Ф0: "FF,<frame>,<count of each FP kind in this frame>" (fc_name order), only with F code
                    static uint64_t flast[FC_N];
                    uint64_t any = 0;
                    for (int k = 0; k < FC_N; k++) any |= fcnt[k];
                    if (any) {
                        printf("FF,%u", v);
                        for (int k = 0; k < FC_N; k++) { printf(",%llu", (unsigned long long)(fcnt[k] - flast[k])); flast[k] = fcnt[k]; }
                        printf("\n");
                    }
                }
                if (prof_from >= 0 && (int)v + 1 >= prof_from) sampling = true;
                if ((int)v == dump_at) dump_now = true;
                for (auto &kk : keys) if (kk.first == nframes) { kbd0 = (uint32_t)kk.second << 8; kbdn++; }
                if (++nframes == stop_frames) done = true;
            }
            else if (o == 0x08) { exit_code = (int)v; done = true; }
            return;
        }
        uint8_t *p = host(a, reg);
        if (!p) bad("store to", a);
        if (a & (sz - 1)) { if (!misaligned++) fprintf(stderr, "MISALIGNED store %d at %08x pc %08x ra %08x\n", sz, a, pc, x[1]); }
        tot.st[reg]++;                 // write-through D$: every store is a bus write (no allocate)
        if (sz < 4) tot.st8[reg]++;    // of them bytes / halfwords (a 16-bit bus needs a mask, a cache line a merge)
        memcpy(p, &v, sz);
    };

    while (!done && ninstr < max_instr) {
        if ((ninstr & 255) == 0) {
            uint64_t us = ninstr / 32;
            if (wavf) { uint64_t play = us * 11025 / 1000000; while (emitted < play) { fwrite(&ring[emitted & 2047], 4, 1, wavf); emitted++; wav_n++; } }
            if (ten && (mstatus & 8) && (mie & 0x80) && (int32_t)((uint32_t)us - tcmp) >= 0) {
                mepc = pc; mcause = 0x80000007u; mstatus = (mstatus & ~0x88u) | ((mstatus & 8) << 4); pc = mtvec & ~3u; n_irq++;
            }
        }
        if (prof && sampling && (ninstr & 255) == 0) fprintf(prof, "%x %x\n", pc, x[1]);
        uint8_t *ip = host(pc, reg);
        if (!ip) bad("fetch from", pc);
        if (ic.miss(pc)) tot.im[reg]++;
        uint32_t in; memcpy(&in, ip, 4);
        uint32_t op = in & 0x7f, rd = (in >> 7) & 31, f3 = (in >> 12) & 7, rs1 = (in >> 15) & 31, rs2 = (in >> 20) & 31, f7 = in >> 25;
        int32_t immi = (int32_t)in >> 20;
        int32_t imms = ((int32_t)in >> 25 << 5) | ((in >> 7) & 31);
        int32_t immb = (((int32_t)in >> 31) << 12) | (((in >> 7) & 1) << 11) | (((in >> 25) & 63) << 5) | (((in >> 8) & 15) << 1);
        int32_t immj = (((int32_t)in >> 31) << 20) | (in & 0xff000) | (((in >> 20) & 1) << 11) | (((in >> 21) & 1023) << 1);
        uint32_t a = x[rs1], b = x[rs2], r = 0, npc = pc + 4;
        bool wr = true;
        switch (op) {
        case 0x37: r = in & 0xfffff000u; break;
        case 0x17: r = pc + (in & 0xfffff000u); break;
        case 0x6f: r = pc + 4; npc = pc + immj; break;
        case 0x67: r = pc + 4; npc = (a + immi) & ~1u; break;
        case 0x63: {
            bool t = false;
            switch (f3) {
            case 0: t = a == b; break; case 1: t = a != b; break;
            case 4: t = (int32_t)a < (int32_t)b; break; case 5: t = (int32_t)a >= (int32_t)b; break;
            case 6: t = a < b; break; case 7: t = a >= b; break;
            default: fprintf(stderr, "bad branch at %08x\n", pc); return 3;
            }
            if (t) npc = pc + immb;
            wr = false; break;
        }
        case 0x03: {
            uint32_t ea = a + immi;
            switch (f3) {
            case 0: r = (uint32_t)(int32_t)(int8_t)ld(ea, 1); break;
            case 1: r = (uint32_t)(int32_t)(int16_t)ld(ea, 2); break;
            case 2: r = ld(ea, 4); break;
            case 4: r = ld(ea, 1) & 0xff; break;
            case 5: r = ld(ea, 2) & 0xffff; break;
            default: fprintf(stderr, "bad load at %08x\n", pc); return 3;
            }
            break;
        }
        case 0x23: {
            uint32_t ea = a + imms;
            st(ea, b, f3 == 0 ? 1 : f3 == 1 ? 2 : 4);
            wr = false; break;
        }
        case 0x13: {
            uint32_t sh = immi & 31;
            switch (f3) {
            case 0: r = a + immi; break;
            case 2: r = (int32_t)a < immi; break;
            case 3: r = a < (uint32_t)immi; break;
            case 4: r = a ^ immi; break;
            case 6: r = a | immi; break;
            case 7: r = a & immi; break;
            case 1: r = a << sh; break;
            case 5: r = (in >> 30 & 1) ? (uint32_t)((int32_t)a >> sh) : a >> sh; break;
            }
            break;
        }
        case 0x33:
            if (f7 == 1) {
                int64_t sa = (int32_t)a, sb = (int32_t)b;
                switch (f3) {
                case 0: r = a * b; break;
                case 1: r = (uint32_t)((sa * sb) >> 32); break;
                case 2: r = (uint32_t)((sa * (int64_t)(uint64_t)b) >> 32); break;
                case 3: r = (uint32_t)(((uint64_t)a * b) >> 32); break;
                case 4: r = b == 0 ? 0xffffffffu : (a == 0x80000000u && b == 0xffffffffu) ? a : (uint32_t)((int32_t)a / (int32_t)b); break;
                case 5: r = b == 0 ? 0xffffffffu : a / b; break;
                case 6: r = b == 0 ? a : (a == 0x80000000u && b == 0xffffffffu) ? 0 : (uint32_t)((int32_t)a % (int32_t)b); break;
                case 7: r = b == 0 ? a : a % b; break;
                }
            } else {
                switch (f3) {
                case 0: r = (f7 & 0x20) ? a - b : a + b; break;
                case 1: r = a << (b & 31); break;
                case 2: r = (int32_t)a < (int32_t)b; break;
                case 3: r = a < b; break;
                case 4: r = a ^ b; break;
                case 5: r = (f7 & 0x20) ? (uint32_t)((int32_t)a >> (b & 31)) : a >> (b & 31); break;
                case 6: r = a | b; break;
                case 7: r = a & b; break;
                }
            }
            break;
        case 0x07:                    // flw
            if (f3 != 2) { fprintf(stderr, "illegal %08x at %08x\n", in, pc); return 3; }
            fr[rd] = ld(a + immi, 4); fcnt[FC_LD]++; wr = false; break;
        case 0x27:                    // fsw
            if (f3 != 2) { fprintf(stderr, "illegal %08x at %08x\n", in, pc); return 3; }
            st(a + imms, fr[rs2], 4); fcnt[FC_ST]++; wr = false; break;
        case 0x43: case 0x47: case 0x4b: case 0x4f: {   // fmadd / fmsub / fnmsub / fnmadd .s
            float p = F(rs1), q = F(rs2), c = F(in >> 27);
            float v = op == 0x43 ? std::fma(p, q, c) : op == 0x47 ? std::fma(p, q, -c)
                    : op == 0x4b ? std::fma(-p, q, c) : std::fma(-p, q, -c);
            fr[rd] = U(v); fcnt[FC_FMA]++; wr = false; break;
        }
        case 0x53: {                  // OP-FP, single
            float p = F(rs1), q = F(rs2);
            wr = false;
            switch (f7) {
            case 0x00: fr[rd] = U(p + q); fcnt[FC_ADD]++; break;
            case 0x04: fr[rd] = U(p - q); fcnt[FC_ADD]++; break;
            case 0x08: fr[rd] = U(p * q); fcnt[FC_MUL]++; break;
            case 0x0c: fr[rd] = U(p / q); fcnt[FC_DIV]++; break;
            case 0x2c: fr[rd] = U(std::sqrt(p)); fcnt[FC_SQRT]++; break;
            case 0x10: {
                uint32_t sa = fr[rs1] & 0x7fffffffu, sb = fr[rs2] & 0x80000000u;
                fr[rd] = f3 == 0 ? sa | sb : f3 == 1 ? sa | (sb ^ 0x80000000u) : sa | ((fr[rs1] ^ fr[rs2]) & 0x80000000u);
                fcnt[FC_MV]++; break;
            }
            case 0x14: {
                float v;
                if (std::isnan(p) && std::isnan(q)) v = NAN;
                else if (std::isnan(p)) v = q; else if (std::isnan(q)) v = p;
                else if (p == 0 && q == 0) v = (f3 == 0) == ((fr[rs1] >> 31) != 0) ? p : q;   // -0 < +0
                else v = f3 == 0 ? (p < q ? p : q) : (p > q ? p : q);
                fr[rd] = U(v); fcnt[FC_MINMAX]++; break;
            }
            case 0x60: r = fcvt_w(p, f3, rs2 == 1); wr = true; fcnt[FC_CVT]++; break;
            case 0x68: fr[rd] = U(rs2 == 1 ? (float)a : (float)(int32_t)a); fcnt[FC_CVT]++; break;
            case 0x70: r = f3 == 0 ? fr[rs1] : fclass(p); wr = true; fcnt[FC_MV]++; break;
            case 0x78: fr[rd] = a; fcnt[FC_MV]++; break;
            case 0x50: r = f3 == 2 ? p == q : f3 == 1 ? p < q : p <= q; wr = true; fcnt[FC_CMP]++; break;
            default: fprintf(stderr, "illegal fp %08x at %08x after %llu\n", in, pc, (unsigned long long)ninstr); return 3;
            }
            break;
        }
        case 0x0f: wr = false; break; // fence
        case 0x73: {
            if (in == 0x30200073u) { // mret
                pc = mepc; npc = mepc; mstatus = (mstatus & ~0x8u) | ((mstatus >> 4) & 8) | 0x80; wr = false; break;
            }
            uint32_t csr = in >> 20, *cp = 0;
            switch (csr) { case 0x300: cp = &mstatus; break; case 0x304: cp = &mie; break; case 0x305: cp = &mtvec; break;
                           case 0x341: cp = &mepc; break; case 0x342: cp = &mcause; break; case 0x343: cp = &mtval; break;
                           case 0x003: cp = &fcsr_v; break; case 0x001: case 0x002: cp = &fdummy; break; }
            uint32_t old = cp ? *cp : (csr == 0x344 ? ((ten && (int32_t)((uint32_t)(ninstr / 32) - tcmp) >= 0) ? 0x80u : 0u) : 0u);
            uint32_t src = (f3 & 4) ? rs1 : a;
            if (cp) { if ((f3 & 3) == 1) *cp = src; else if ((f3 & 3) == 2) *cp |= src; else if ((f3 & 3) == 3) *cp &= ~src; }
            r = old; wr = (rd != 0) && f3 != 0;
            break;
        }
        default:
            fprintf(stderr, "illegal %08x at %08x after %llu\n", in, pc, (unsigned long long)ninstr);
            return 3;
        }
        if (wr && rd) x[rd] = r;
        pc = npc;
        ninstr++;
        if (dump_now) {
            // resume stub (as core-pico8), at the end of the SDRAM: x31 = base, mepc = pc, mstatus.MPP = M,
            // load x1..x31, mret
            auto I = [](int imm, int rs1, int f3, int rd, int op) { return (uint32_t)((imm & 0xfff) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op); };
            std::vector<uint32_t> code;
            code.push_back((0x11fffu << 12) | (31 << 7) | 0x37); // lui x31, 0x11fff
            code.push_back(I(0x180, 31, 2, 5, 0x03));             // lw x5, pc
            code.push_back(I(0x341, 5, 1, 0, 0x73));              // csrw mepc, x5
            // Quake Ф0: code with F (any FP op seen) also gets mstatus.FS and its f registers + fcsr back:
            // a block at +0x400 (fcsr at +0x380, f0..f31 at +0x300), jumped to and back from the stub
            bool withf = false;
            for (int k = 0; k < FC_N; k++) withf |= fcnt[k] != 0;
            auto J = [](int off) { uint32_t u = (uint32_t)off; return ((u >> 20 & 1) << 31) | ((u >> 1 & 0x3ff) << 21) | ((u >> 11 & 1) << 20) | ((u >> 12 & 0xff) << 12) | 0x6f; };
            code.push_back(((withf ? 0x4u : 0x2u) << 12) | (5 << 7) | 0x37); // lui x5, 2 (4 with F)
            code.push_back(I(-2048, 5, 0, 5, 0x13));              // addi x5, x5, -2048 (0x1800: MPP = M; 0x3800: + FS)
            code.push_back(I(0x300, 5, 1, 0, 0x73));              // csrw mstatus, x5
            size_t back = 0;
            if (withf) { code.push_back(J(0x400 - 4 * (int)code.size())); back = code.size(); }
            for (int r = 1; r <= 31; r++) code.push_back(I(0x100 + 4 * r, 31, 2, r, 0x03));
            code.push_back(0x30200073);                           // mret
            uint8_t *rb = &sdram[RES - SDRAM_BASE];
            memcpy(rb, code.data(), code.size() * 4);
            for (int r = 0; r < 32; r++) memcpy(rb + 0x100 + 4 * r, &x[r], 4);
            memcpy(rb + 0x180, &pc, 4);
            if (withf) {
                std::vector<uint32_t> fb;
                fb.push_back(I(0x380, 31, 2, 5, 0x03));           // lw x5, fcsr
                fb.push_back(I(0x003, 5, 1, 0, 0x73));            // csrw fcsr, x5
                for (int r = 0; r < 32; r++) fb.push_back(I(0x300 + 4 * r, 31, 2, r, 0x07));   // flw fr, ..(x31)
                fb.push_back(J(4 * (int)back - (0x400 + 4 * (int)fb.size())));                 // back to the x loads
                memcpy(rb + 0x400, fb.data(), fb.size() * 4);
                memcpy(rb + 0x300, fr, 128);
                memcpy(rb + 0x380, &fcsr_v, 4);
            }
            FILE *d = fopen(dump_path, "wb");
            fwrite(sram.data(), 1, SRAM_SIZE, d);
            fwrite(sdram.data(), 1, SDRAM_SIZE, d);
            fwrite(&wad_size, 4, 1, d);
            fwrite(drw.cmap, 1, 8192, d);   // Ф1в: the colormaps copy of the drawer (tb_rtl loads it)
            fclose(d);
            printf("DUMP %s frame %d pc %08x us %llu\n", dump_path, dump_at, pc, (unsigned long long)(ninstr / 32));
            done = true;
        }
    }
    if (!line.empty()) printf("%s\n", line.c_str());
    if (prof) fclose(prof);
    printf("SOUND irqs %llu opl writes %llu midi bytes %llu wav samples %llu\n", (unsigned long long)n_irq,
           (unsigned long long)n_opl, (unsigned long long)n_midi, (unsigned long long)wav_n);
    if (wavf) {
        uint32_t data = (uint32_t)(wav_n * 4), rate = 11025, br = rate * 4, sz = 36 + data, fmt = 16;
        uint16_t pcm = 1, chn = 2, ba = 4, bits = 16;
        fseek(wavf, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wavf); fwrite(&sz, 4, 1, wavf); fwrite("WAVEfmt ", 1, 8, wavf); fwrite(&fmt, 4, 1, wavf);
        fwrite(&pcm, 2, 1, wavf); fwrite(&chn, 2, 1, wavf); fwrite(&rate, 4, 1, wavf); fwrite(&br, 4, 1, wavf);
        fwrite(&ba, 2, 1, wavf); fwrite(&bits, 2, 1, wavf); fwrite("data", 1, 4, wavf); fwrite(&data, 4, 1, wavf);
        fclose(wavf);
    }
    if (oplf) fclose(oplf);
    if (sd_path) { FILE *q = fopen(sd_path, "wb"); fwrite(sdf.data(), 1, sdf.size(), q); fclose(q); }
    if (midif) fclose(midif);
    tot.instr = ninstr;
    printf("FP");
    for (int k = 0; k < FC_N; k++) printf(" %s %llu", fc_name[k], (unsigned long long)fcnt[k]);
    printf("\n");
    printf("END pc %08x ra %08x misaligned %llu pocket-io %llu\n", pc, x[1], (unsigned long long)misaligned,
           (unsigned long long)n_pq);
    printf("END instr %llu exit %d%s im %llu/%llu dm %llu/%llu st %llu/%llu (sram/sdram)\n", (unsigned long long)ninstr, exit_code, done ? "" : " (max)",
           (unsigned long long)tot.im[0], (unsigned long long)tot.im[1], (unsigned long long)tot.dm[0], (unsigned long long)tot.dm[1],
           (unsigned long long)tot.st[0], (unsigned long long)tot.st[1]);
    return done ? 0 : 1;
}
