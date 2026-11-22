/*
 * sram_fill_accel.h -- karabas: replaces the PocketQuake SRAM fill accelerator (the z-buffer clear
 * of its SRAM, 0x5C000000) with no-ops: with HW_* = 0 the software renderer writes the z of every
 * world span (D_DrawZSpans), as WinQuake, so the z-buffer needs no clear.
 */

#ifndef SRAM_FILL_ACCEL_H
#define SRAM_FILL_ACCEL_H

static inline void sram_fill_start(unsigned int dst_addr, unsigned int length, unsigned int fill_value)
{
    (void)dst_addr; (void)length; (void)fill_value;
}

static inline void sram_fill_wait(void)
{
}

#endif /* SRAM_FILL_ACCEL_H */
