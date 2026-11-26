/*
 * kg_plat.c -- the rest of the PocketQuake platform for the karabas-go core (karabas: replaces
 * terminal.c, dataslot.c, main.c of PocketQuake): the text terminal goes to MMIO_UART; data slot 1 is
 * id1/pak0.pak (Ф0: in memory at WAD_BASE). The engine reads the pak both through Sys_File* (sys_kg.c)
 * and through the stdio of their libc (COM_FOpenFile: fopen + fseek + fread -> dataslot_read), e.g. the
 * demos. Other slots (mod paks, saves of libc/file.c: Ф2) do not exist.
 */

#include <stdint.h>
#include <stdarg.h>
#include "libc/libc.h"
#include "dataslot.h"   /* ours (quake/dataslot.h) */
#undef SDRAM_BASE
#include "mmio.h"   /* src/fw (-I) */

void term_init(void) {}
void term_clear(void) {}
void term_setpos(int row, int col) { (void)row; (void)col; }
int term_getpos(void) { return 0; }
#ifdef KG_HW
void kg_log_putc(char c);                /* kg_qhw.c: the log drawn by kg_exit_screen */
#define LOG(c) kg_log_putc(c)
#else
#define LOG(c) ((void)0)
#endif
void term_putchar(char c) { MMIO_UART = (unsigned char)c; LOG(c); }
void term_puts(const char *s) { while (*s) { LOG(*s); MMIO_UART = (unsigned char)*s++; } }

void term_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_puts(buf);
}

int dataslot_wait_complete(void) { return -1; }
void dataslot_read_start(uint32_t slot_id, uint32_t offset, void *dest, uint32_t length)
{
    (void)slot_id; (void)offset; (void)dest; (void)length;
}
int dataslot_read_poll(void) { return -1; }
int dataslot_open_file(uint16_t slot_id, const char *filename, uint32_t flags, uint32_t size)
{
    (void)slot_id; (void)filename; (void)flags; (void)size;
    return -1;
}
unsigned char kg_dma_buffer[DMA_CHUNK_SIZE] __attribute__((aligned(32)));

/* data slots 1 / 2 of libc/file.c = id1/pak0.pak / pak1.pak (Ф1б: kg_pak.c) */
#define PAK0_SLOT_ID 1
uint32_t kg_pak_size(int k);
int kg_pak_read(int k, uint32_t off, void *dest, uint32_t len);

int dataslot_read(uint32_t slot_id, uint32_t offset, void *dest, uint32_t length)
{
    if (slot_id != PAK0_SLOT_ID && slot_id != PAK0_SLOT_ID + 1)
        return -1;
    return kg_pak_read((int)slot_id - PAK0_SLOT_ID, offset, dest, length);
}
#ifndef KG_HW   /* the board: kg_qsave.c (the save file) */
int dataslot_write(uint16_t slot_id, uint32_t offset, const void *src, uint32_t length)
{
    (void)slot_id; (void)offset; (void)src; (void)length;
    return -1;
}
#endif
int32_t dataslot_load(uint16_t slot_id, void *dest, uint32_t max_length)
{
    (void)slot_id; (void)dest; (void)max_length;
    return -1;
}
int dataslot_get_size(uint16_t slot_id, uint32_t *size_out)
{
    *size_out = slot_id == PAK0_SLOT_ID || slot_id == PAK0_SLOT_ID + 1 ? kg_pak_size((int)slot_id - PAK0_SLOT_ID) : 0;
    return *size_out ? 0 : -1;
}

/* Ф3: wait until the span drawer (quake_span) has no work (d_surf.c before a surface is built, KG_HWSPAN) */
void kg_span_idle(void)
{
    while (MMIO_QSP_STAT & 1) {}
}
