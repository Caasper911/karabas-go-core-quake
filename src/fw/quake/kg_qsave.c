/*
 * kg_qsave.c -- Quake Ф2 (karabas, KG_HW): config.cfg and the savegames s0..s9.sav in the save file of the SD
 * card (doom_sd: the backup RAM request kind 0, 1 MB, <pak dir>/saves/<pak name>.sav; no MCU change), as the
 * packed files of ../kg_save_fs.c (KG_QUAKE).
 *
 * The libc of PocketQuake (libc/file.c) keeps these files in a region of the memory: slot k (0..9: s<k>.sav,
 * 10: config.cfg) at SAV_BASE + k * 128 KB = { size (4 bytes), data }; its fopen("...", "r") reads a slot,
 * its fclose after a write puts the slot there and calls dataslot_write(20 + k or 30, 0, slot, 4 + size).
 * Here: kg_qsave_boot (before Host_Init) fills the slots from the save file; dataslot_write stores a slot
 * into it; kg_sav_file lets sys_kg.c open them (exec config.cfg goes through Sys_FileOpenRead).
 * malloc of kg_save_fs (the packing buffers) gets the free SRAM KG_HEAP_BASE.. (below the z-buffer).
 */

#include <stdint.h>
#include "libc/libc.h"
#include "dataslot.h"
#undef SDRAM_BASE
#include "mmio.h"       /* src/fw (-I) */
#include "kg_save.h"    /* src/fw */

#define SAV_BASE     0x00200000u     /* = SAV_REGION_BASE of libc/file.c (sed in src/fw/Makefile) */
#define SAV_SLOT     0x20000u
#define SAV_N        10              /* s0..s9; slot 10 = config.cfg */
#define SAV_FIRST_ID 20
#define SAV_CFG_ID   30
#define KG_HEAP_BASE 0x00360000u     /* free SRAM up to the z-buffer (0x3b0000) */
#define KG_HEAP_SIZE 0x00050000u

static uint8_t *slot_addr(int k) { return (uint8_t *)(SAV_BASE + (uint32_t)k * SAV_SLOT); }

static void name_of(int k, char *n)
{
    if (k == SAV_N) { strcpy(n, "config.cfg"); return; }
    n[0] = 's'; n[1] = (char)('0' + k); strcpy(n + 2, ".sav");
}

static int slot_of_path(const char *path)
{
    const char *b = path, *p;
    for (p = path; *p; p++) if (*p == '/') b = p + 1;
    if (!strcmp(b, "config.cfg")) return SAV_N;
    if (b[0] == 's' && b[1] >= '0' && b[1] <= '9' && !strcmp(b + 2, ".sav")) return b[1] - '0';
    return -1;
}

void kg_qsave_boot(void)
{
    char n[12];
    int k;
    heap_init((void *)KG_HEAP_BASE, KG_HEAP_SIZE);
    for (k = 0; k <= SAV_N; k++) *(volatile uint32_t *)slot_addr(k) = 0;   /* the SRAM is not cleared at power-up */
    kg_save_init();
    for (k = 0; k <= SAV_N; k++) {
        int sz;
        name_of(k, n);
        sz = kg_save_size(n);
        if (sz > 0 && (uint32_t)sz <= SAV_SLOT - 4 && kg_save_load(n, slot_addr(k) + 4, SAV_SLOT - 4) == sz)
            *(volatile uint32_t *)slot_addr(k) = (uint32_t)sz;
    }
}

int dataslot_write(uint16_t slot_id, uint32_t offset, const void *src, uint32_t length)
{
    char n[12];
    uint32_t sz;
    int k = slot_id == SAV_CFG_ID ? SAV_N : (int)slot_id - SAV_FIRST_ID;
    if (k < 0 || k > SAV_N || offset != 0 || length < 4)
        return -1;
    memcpy(&sz, src, 4);
    name_of(k, n);
    { extern void kg_snd_clear(void); kg_snd_clear(); }   /* Ф7: no looped sound while the SD card is written */
    return kg_save_store(n, (const uint8_t *)src + 4, sz) ? 0 : -1;
}

/* a .cfg / .sav of the slots as a memory file (sys_kg.c); NULL: none */
const unsigned char *kg_sav_file(const char *path, int *len)
{
    int k = slot_of_path(path);
    uint32_t sz;
    if (k < 0)
        return NULL;
    sz = *(volatile uint32_t *)slot_addr(k);
    if (sz == 0 || sz > SAV_SLOT - 4)
        return NULL;
    *len = (int)sz;
    return slot_addr(k) + 4;
}
