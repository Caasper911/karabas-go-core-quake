/* Hexen Ф2 (karabas): the files of Hexen that live on, in the save file of the SD card (the backup RAM
 * request of doom_sd.v, kind 0: <WAD dir>/saves/<WAD name>.sav, 1 MB = 2048 sectors of 512 bytes), as a
 * small file system of packed files (kg_lz). The same interface as kg_save.c (Doom / Heretic: fixed
 * slots), see kg_save.h; the RAM files of syscalls.c stay the working copies.
 *
 * Kept: hexen.cfg, chocolate-hexen.cfg and the savegames of the slots 0..5 (hex<n>.hxs, hex<n><map>.hxs).
 * The slots 6 (the base of the hub) and 7 (the reborn copy) are RAM files only: Hexen makes them again
 * when a hub is entered or a slot is loaded.
 *
 * Layout: sectors 0..7 the directory: magic KGHEXEN2 (8 bytes), 8 bytes free, then DIR_N entries of 32
 * bytes { name[20], start sector u16, sectors u16, packed size u32, raw size u32 } (name[0] 0: free).
 * Sectors 8..2047: the data, a file in contiguous sectors (first fit; when nothing fits, the files are
 * moved down (compaction) and it is tried again). A file is written before its entry: a new version
 * goes to free sectors first (the old one stays until the entry points to the new one); only when that
 * cannot fit, the old sectors are given back first.
 * A save file of Ф1 (KGHEXEN1: hexen.cfg / chocolate-hexen.cfg at fixed sectors) is converted.
 * When a file does not fit at all it stays a RAM file (the game goes on); kg_save_full is set and the
 * game shows SAVE FILE FULL (hexen/kg_hexen_hw.c).
 * Quake Ф2 (KG_QUAKE, src/fw/quake/kg_qsave.c): magic KGQUAKE1, kept config.cfg and s0..s9.sav; no Ф1 conversion. */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "mmio.h"
#include "kg_save.h"
#include "kg_lz.h"

#define SD_SECTOR  MMIO_REG(0x80)
#define SD_CMD     MMIO_REG(0x84)
#define SD_STAT    MMIO_REG(0x88)
#define SD_SIZE    MMIO_REG(0x8c)
#define SD_BKREQ   MMIO_REG(0x90)
#define SD_BUF(i)  MMIO_REG(0x1000 + 4 * (i))

#define FILE_SECTORS 2048
#define DIR_SECTORS  8
#define DATA_FIRST   DIR_SECTORS
#define DIR_N        ((DIR_SECTORS * 512 - 16) / 32)
#define NAME_LEN     20

typedef struct {
    char name[NAME_LEN];
    uint16_t start, sectors;
    uint32_t packed, raw;
} entry_t;
_Static_assert(sizeof(entry_t) == 32, "the entry is 32 bytes");

#ifdef KG_QUAKE
static const char MAGIC[8] = "KGQUAKE1";   /* Quake Ф2: config.cfg and the savegames s0..s9.sav */
#else
static const char MAGIC[8] = "KGHEXEN2";
#endif
static union { uint8_t b[DIR_SECTORS * 512]; struct { char magic[8]; uint32_t pad[2]; entry_t e[DIR_N]; } d; } dir;
static int ready;
int kg_save_full;                   /* a file did not fit (hexen/kg_hexen_hw.c shows it) */
const int kg_save_drop = 1;         /* syscalls.c: kept files leave RAM after their close */

/* debug: the last console byte (DEBUG_ADDR low byte of the board) says the state of the saves */
static char last_st = '?';
static void st(char c) { last_st = c; MMIO_UART = (uint8_t) c; }
void kg_save_show(void) { MMIO_UART = (uint8_t) last_st; }

static int wait_idle(void) {
    uint32_t t0 = MMIO_MS;
    while (SD_STAT & 1) if (MMIO_MS - t0 > 500) { printf("Saves: SD timeout, off\n"); ready = 0; st('T'); return 0; }
    if (SD_STAT & 2) { printf("Saves: SD error, off\n"); ready = 0; st('E'); return 0; }
    return 1;
}
static int sec_read(uint32_t s, uint8_t *buf) {
    SD_SECTOR = s;
    SD_CMD = 1;
    if (!wait_idle()) return 0;
    for (int i = 0; i < 512; i++) buf[i] = (uint8_t) SD_BUF(i);
    return 1;
}
static int sec_write(uint32_t s, const uint8_t *buf) {
    for (int i = 0; i < 512; i++) SD_BUF(i) = buf[i];
    SD_SECTOR = s;
    SD_CMD = 2;
    return wait_idle();
}
static int dir_write_sector(int s) { return sec_write((uint32_t) s, dir.b + 512 * s); }
static int dir_write_entry(int i) {
    int lo = (16 + 32 * i) / 512, hi = (16 + 32 * i + 31) / 512;
    return dir_write_sector(lo) && (hi == lo || dir_write_sector(hi));
}
static int dir_write_all(void) {
    for (int s = 0; s < DIR_SECTORS; s++) if (!dir_write_sector(s)) return 0;
    return 1;
}

/* the kept name of a path (its last element), NULL: not kept */
static const char *kept_name(const char *path) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
#ifdef KG_QUAKE
    /* Quake Ф2: config.cfg, s<0..9>.sav (the slots of the libc of PocketQuake) */
    if (!strcmp(b, "config.cfg")) return b;
    if (b[0] == 's' && b[1] >= '0' && b[1] <= '9' && !strcmp(b + 2, ".sav")) return b;
    return NULL;
#endif
    if (!strcmp(b, "hexen.cfg") || !strcmp(b, "chocolate-hexen.cfg")) return b;
    /* hex<slot 0..5>.hxs or hex<slot><2 digits>.hxs */
    if (strncmp(b, "hex", 3) || b[3] < '0' || b[3] > '5') return NULL;
    if (!strcmp(b + 4, ".hxs")) return b;
    if (b[4] >= '0' && b[4] <= '9' && b[5] >= '0' && b[5] <= '9' && !strcmp(b + 6, ".hxs")) return b;
    return NULL;
}
static int find(const char *name) {
    for (int i = 0; i < DIR_N; i++) if (dir.d.e[i].name[0] && !strncmp(dir.d.e[i].name, name, NAME_LEN)) return i;
    return -1;
}

/* the first free run of n data sectors, -1: none; ignore: an entry whose sectors count as free */
static int alloc(uint32_t n, int ignore) {
    uint32_t at = DATA_FIRST;
    for (;;) {
        uint32_t next = FILE_SECTORS;
        int hit = 0;
        for (int i = 0; i < DIR_N; i++) {
            entry_t *e = &dir.d.e[i];
            if (!e->name[0] || i == ignore || !e->sectors) continue;
            if (e->start < at + n && e->start + e->sectors > at) { hit = 1; if (e->start + e->sectors > next || next == FILE_SECTORS) next = e->start + e->sectors; }
        }
        if (!hit) return at + n <= FILE_SECTORS ? (int) at : -1;
        /* go past the highest end of the files in the way */
        at = next;
        if (at + n > FILE_SECTORS) return -1;
    }
}

/* move the files down to the start of the data (in their order), then the directory */
static int compact(void) {
    uint8_t b[512];
    uint32_t at = DATA_FIRST;
    printf("Saves: compacting\n");
    for (;;) {
        int best = -1;
        for (int i = 0; i < DIR_N; i++) {
            entry_t *e = &dir.d.e[i];
            if (e->name[0] && e->sectors && e->start >= at && (best < 0 || e->start < dir.d.e[best].start)) best = i;
        }
        if (best < 0) break;
        entry_t *e = &dir.d.e[best];
        if (e->start != at) {
            for (uint32_t k = 0; k < e->sectors; k++)
                if (!sec_read(e->start + k, b) || !sec_write(at + k, b)) return 0;
            e->start = (uint16_t) at;
            if (!dir_write_entry(best)) return 0;
        }
        at += e->sectors;
    }
    return 1;
}

static int write_file(uint32_t start, const uint8_t *data, uint32_t n) {
    uint8_t b[512];
    for (uint32_t off = 0; off < n; off += 512) {
        uint32_t k = n - off < 512 ? n - off : 512;
        memset(b, 0, sizeof b);
        memcpy(b, data + off, k);
        if (!sec_write(start + off / 512, b)) return 0;
    }
    return 1;
}

/* ---- Ф1 save files (KGHEXEN1): the two configs at sectors 1 and 17, 16 sectors each */
#ifndef KG_QUAKE
static int convert_v1(const uint8_t *s0) {
    static const char *const names[2] = { "hexen.cfg", "chocolate-hexen.cfg" };
    uint32_t size[2];
    memcpy(size, s0 + 8, sizeof size);
    uint8_t *buf[2] = { NULL, NULL };
    for (int c = 0; c < 2; c++) {
        if (size[c] == 0 || size[c] > 16 * 512) continue;
        buf[c] = malloc(size[c]);
        uint8_t b[512];
        for (uint32_t off = 0; buf[c] && off < size[c]; off += 512) {
            if (!sec_read(1 + 16 * c + off / 512, b)) { free(buf[c]); buf[c] = NULL; break; }
            memcpy(buf[c] + off, b, size[c] - off < 512 ? size[c] - off : 512);
        }
    }
    memset(&dir, 0, sizeof dir);
    memcpy(dir.d.magic, MAGIC, 8);
    int ok = dir_write_all();
    for (int c = 0; c < 2; c++) {
        if (ok && buf[c]) kg_save_store(names[c], buf[c], size[c]);
        free(buf[c]);
    }
    printf("Saves: converted the save file of Ф1\n");
    return ok;
}
#endif

void kg_save_init(void) {
    ready = 0;
    /* as kg_save.c: ask only when nothing is mounted (F12 keeps the mount) */
    if (!(SD_STAT & 4)) SD_BKREQ = FILE_SECTORS;
    uint32_t t0 = MMIO_MS;
    while (!(SD_STAT & 4)) if (MMIO_MS - t0 > 2500) { printf("Saves: no save file\n"); st('N'); return; }
    if (SD_SIZE < FILE_SECTORS * 512u) { printf("Saves: the save file is too small\n"); st('S'); return; }
    ready = 1;
    for (int s = 0; s < DIR_SECTORS; s++)
        if (!sec_read((uint32_t) s, dir.b + 512 * s)) { printf("Saves: read error\n"); ready = 0; return; }
    if (memcmp(dir.d.magic, MAGIC, 8)) {
#ifndef KG_QUAKE
        if (!memcmp(dir.d.magic, "KGHEXEN1", 8)) {
            if (!convert_v1(dir.b)) { printf("Saves: write error\n"); ready = 0; return; }
        } else
#endif
        {
            /* only a new file (all zeros) gets an empty directory: anything else is left alone */
            for (int i = 0; i < DIR_SECTORS * 512; i++) if (dir.b[i]) { printf("Saves: the save file is not ours, off\n"); ready = 0; st('X'); return; }
            memcpy(dir.d.magic, MAGIC, 8);
            if (!dir_write_all()) { printf("Saves: write error\n"); ready = 0; return; }
        }
    }
    /* entries out of the data area are dropped */
    for (int i = 0; i < DIR_N; i++) {
        entry_t *e = &dir.d.e[i];
        if (e->name[0] && (e->start < DATA_FIRST || e->start + e->sectors > FILE_SECTORS)) e->name[0] = 0;
    }
    printf("Saves: on the SD card\n");
    st('R');
}

int kg_save_kept(const char *name) { return ready && kept_name(name) != NULL; }

int kg_save_size(const char *name) {
    const char *k = kept_name(name);
    int i;
    if (!ready || !k || (i = find(k)) < 0) return -1;
    return (int) dir.d.e[i].raw;
}

int kg_save_load(const char *name, uint8_t *out, uint32_t cap) {
    const char *k = kept_name(name);
    int i;
    if (!ready || !k || (i = find(k)) < 0) return -1;
    entry_t *e = &dir.d.e[i];
    if (e->raw > cap) return -1;
    uint8_t *p = malloc(e->sectors * 512u + 1);
    if (!p) return -1;
    for (uint32_t s = 0; s < e->sectors; s++)
        if (!sec_read(e->start + s, p + 512 * s)) { free(p); return -1; }
    uint32_t n = kg_lz_unpack(p, e->packed, out, cap);
    free(p);
    if (n != e->raw) { printf("Saves: %s is damaged\n", k); return -1; }
    return (int) n;
}

int kg_save_store(const char *name, const uint8_t *data, uint32_t size) {
    const char *k = kept_name(name);
    if (!ready || !k) return 0;
    uint8_t *p = malloc(size + size / 128 + 16);
    if (!p) return 0;
    uint32_t packed = kg_lz_pack(data, size, p);
    uint32_t n = (packed + 511) / 512;
    int old = find(k);
    int at = alloc(n, -1);
    if (at < 0 && old >= 0) at = alloc(n, old);          /* over its own old sectors */
    if (at < 0 && compact()) {
        at = alloc(n, -1);
        if (at < 0 && old >= 0) at = alloc(n, old);
    }
    int ok = 0;
    if (at >= 0 && write_file((uint32_t) at, p, packed)) {
        int i = old;
        if (i < 0) for (i = 0; i < DIR_N && dir.d.e[i].name[0]; i++) {}
        if (i < DIR_N) {
            entry_t *e = &dir.d.e[i];
            memset(e, 0, sizeof *e);
            strncpy(e->name, k, NAME_LEN);
            e->start = (uint16_t) at;
            e->sectors = (uint16_t) n;
            e->packed = packed;
            e->raw = size;
            ok = dir_write_entry(i);
        }
    }
    free(p);
    if (!ok) { printf("Saves: %s does not fit\n", k); kg_save_full = 1; st('F'); }
    else st('K');
    return ok;
}

void kg_save_remove(const char *name) {
    const char *k = kept_name(name);
    int i;
    if (!ready || !k || (i = find(k)) < 0) return;
    dir.d.e[i].name[0] = 0;
    dir_write_entry(i);
}
