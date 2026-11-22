/*
 * kg_plat.c -- the rest of the PocketQuake platform for the karabas-go core (karabas: replaces
 * terminal.c, dataslot.c, main.c of PocketQuake): the text terminal goes to MMIO_UART, the data slots
 * do not exist (the pak is read by sys_kg.c; libc/file.c only uses them for saves, Ф2).
 */

#include <stdint.h>
#include <stdarg.h>
#include "libc/libc.h"
#include "dataslot.h"
#undef SDRAM_BASE
#include "mmio.h"   /* src/fw (-I) */

void term_init(void) {}
void term_clear(void) {}
void term_setpos(int row, int col) { (void)row; (void)col; }
int term_getpos(void) { return 0; }
void term_putchar(char c) { MMIO_UART = (unsigned char)c; }
void term_puts(const char *s) { while (*s) MMIO_UART = (unsigned char)*s++; }

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
int dataslot_read(uint32_t slot_id, uint32_t offset, void *dest, uint32_t length)
{
    (void)slot_id; (void)offset; (void)dest; (void)length;
    return -1;
}
int dataslot_write(uint16_t slot_id, uint32_t offset, const void *src, uint32_t length)
{
    (void)slot_id; (void)offset; (void)src; (void)length;
    return -1;
}
int32_t dataslot_load(uint16_t slot_id, void *dest, uint32_t max_length)
{
    (void)slot_id; (void)dest; (void)max_length;
    return -1;
}
int dataslot_get_size(uint16_t slot_id, uint32_t *size_out)
{
    (void)slot_id;
    *size_out = 0;
    return -1;
}
