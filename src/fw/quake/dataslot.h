/*
 * dataslot.h -- karabas: replaces the Analogue Pocket data slot interface of PocketQuake (APF bridge
 * DMA through the sysreg at 0x40000000, bounce buffer at 0x13F00000, uncached alias +0x40000000).
 *
 * The same calls, implemented by kg_plat.c: slot 1 = id1/pak0.pak. Ф0 (ISS): the pak lies in memory
 * at WAD_BASE; on the board (Ф1) dataslot_read becomes sector reads of an F image through the MCU.
 * The bounce buffer is a bss array; there is no uncached alias (the D$ is write-through and nothing
 * writes behind it), so SDRAM_UNCACHED is the address itself.
 */
#ifndef DATASLOT_H
#define DATASLOT_H

#include <stdint.h>
#include <stddef.h>

extern unsigned char kg_dma_buffer[];
#define SDRAM_UNCACHED(addr) ((void *)(addr))
#define DMA_BUFFER       ((uint32_t)kg_dma_buffer)
#define DMA_CHUNK_SIZE   (32 * 1024)

typedef struct __attribute__((packed)) {
    char     filename[256];
    uint32_t flags;
    uint32_t size;
} dataslot_open_param_t;

#define DS_OPEN_CREATE      (1 << 0)
#define DS_OPEN_RESIZE      (1 << 1)

int dataslot_wait_complete(void);
void dataslot_read_start(uint32_t slot_id, uint32_t offset, void *dest, uint32_t length);
int  dataslot_read_poll(void);
int dataslot_open_file(uint16_t slot_id, const char *filename, uint32_t flags, uint32_t size);
int dataslot_read(uint32_t slot_id, uint32_t offset, void *dest, uint32_t length);
int dataslot_write(uint16_t slot_id, uint32_t offset, const void *src, uint32_t length);
int32_t dataslot_load(uint16_t slot_id, void *dest, uint32_t max_length);
int dataslot_get_size(uint16_t slot_id, uint32_t *size_out);

#endif /* DATASLOT_H */
