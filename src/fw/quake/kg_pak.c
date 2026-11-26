/*
 * kg_pak.c -- Quake Ф1б (karabas): the paks read by sectors from the SD card, through the F items PAK0 / PAK1 of
 * the OSD (image slots 2 / 3: the MCU mounts the file, the core asks for its sectors; no MCU change) and doom_sd
 * (src/rtl/doom_sd.v: CMD bits 6..4 = the slot, STAT bits 3 / 4 = mounted, SIZE2 / SIZE3).
 *
 * Read sectors are kept in a cache in the free SDRAM (the former pak area WAD_BASE..): 32768 lines of one sector
 * (16 MB), direct mapped by the sector number, and a tag per line (128 KB above them, cleared at the first use:
 * the SDRAM is not cleared at power-up). A level is read once; a second visit and the demos hit the cache.
 * The ISS bench (--wad: the pak in memory at WAD_BASE, MMIO_WADSIZE != 0) reads pak0 from memory as before.
 */

#include <stdint.h>
#include "libc/libc.h"
#undef SDRAM_BASE
#include "mmio.h"       /* src/fw (-I) */

#define SD_SECTOR  MMIO_REG(0x80)
#define SD_CMD     MMIO_REG(0x84)
#define SD_STAT    MMIO_REG(0x88)
#define SD_SIZE2   MMIO_REG(0x94)
#define SD_SIZE3   MMIO_REG(0x98)
#define SD_BUF(i)  MMIO_REG(0x1000 + 4 * (i))
#define PAK_SLOT   2                  /* the image slot of PAK0 (core.json); PAK1: PAK_SLOT + 1 */

#define PC_LINES   32768u
#define PC_DATA    ((uint8_t *)WAD_BASE)
#define PC_TAG     ((uint32_t *)(WAD_BASE + PC_LINES * 512u))

void Sys_Error(char *error, ...);

static int pc_ready;
unsigned kg_pak_reads, kg_pak_hits;   /* sectors read from the SD card / found in the cache */

/* the size of pak k (0: pak0, 1: pak1) in bytes, 0: none */
uint32_t kg_pak_size(int k)
{
    if (k == 0 && MMIO_WADSIZE != 0)
        return MMIO_WADSIZE;
    if (!(SD_STAT & (8u << k)))
        return 0;
    return k ? SD_SIZE3 : SD_SIZE2;
}

static void sd_wait(const char *what)
{
    uint32_t t0 = MMIO_MS;
    while (SD_STAT & 1)
        if (MMIO_MS - t0 > 3000)
            Sys_Error("pak: SD %s timeout", what);
}

static void read_sector(int k, uint32_t sec, uint8_t *d)
{
    int i;
    sd_wait("busy");
    SD_SECTOR = sec;
    SD_CMD = 1u | ((uint32_t)(PAK_SLOT + k) << 4);
    sd_wait("read");
    if (SD_STAT & 2)
        Sys_Error("pak%d: SD error at sector %u", k, (unsigned)sec);
    for (i = 0; i < 512; i += 4) {
        uint32_t a = SD_BUF(i) & 0xff, b = SD_BUF(i + 1) & 0xff, c = SD_BUF(i + 2) & 0xff, e = SD_BUF(i + 3) & 0xff;
        *(uint32_t *)(d + i) = a | (b << 8) | (c << 16) | (e << 24);
    }
}

/* len bytes of pak k from offset off; 0 ok, -1 out of the file */
int kg_pak_read(int k, uint32_t off, void *dest, uint32_t len)
{
    uint8_t *d = (uint8_t *)dest;
    uint32_t size = kg_pak_size(k);
    if (off > size || len > size - off)
        return -1;
    if (k == 0 && MMIO_WADSIZE != 0) {
        memcpy(d, (const void *)(WAD_BASE + off), len);
        return 0;
    }
    if (!pc_ready) {
        memset(PC_TAG, 0, PC_LINES * 4);
        pc_ready = 1;
    }
    while (len) {
        uint32_t sec = off >> 9, o = off & 511, n = 512 - o;
        uint32_t line = (sec + (uint32_t)k * (PC_LINES / 2)) & (PC_LINES - 1);
        uint32_t tag = (((uint32_t)k << 31) | sec) + 1;
        uint8_t *l = PC_DATA + line * 512u;
        if (n > len)
            n = len;
        if (PC_TAG[line] != tag) {
            read_sector(k, sec, l);
            PC_TAG[line] = tag;
            kg_pak_reads++;
        } else
            kg_pak_hits++;
        memcpy(d, l + o, n);
        d += n; off += n; len -= n;
    }
    return 0;
}
