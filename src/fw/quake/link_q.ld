/* Quake (karabas): program, data, the Quake heap (hunk / zone / cache: _heap_start.._heap_end) and the
   stack in the first 12 MB of the SDRAM; the pak at 0x10c00000 (Ф0: loaded by the sim, up to 20 MB).
   SRAM 4 MB at 0 (frame 0x3f0000), MMIO at 0x80000000. PocketQuake's .fastdata / .fastrodata (its BRAM
   placement hints) go with the normal data here. */
OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY { RAM (rwx) : ORIGIN = 0x10000000, LENGTH = 0x0c00000 }

STACK_SIZE = 512K;

SECTIONS
{
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    } > RAM
    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        *(.fastrodata .fastrodata.*)
    } > RAM
    .data : ALIGN(8) {
        __data_start = .;
        *(.data .data.*)
        *(.fastdata .fastdata.*)
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
        . = ALIGN(8);
        __data_end = .;
    } > RAM
    .bss (NOLOAD) : ALIGN(8) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM
    /* the copy of .data as loaded (crt0_q.S with KG_HW: a restart restores it) */
    .data_save (NOLOAD) : ALIGN(8) {
        __data_save = .;
        . += __data_end - __data_start;
    } > RAM
    . = ALIGN(64);
    _heap_start = .;
    __stack_top = ORIGIN(RAM) + LENGTH(RAM);
    _heap_end = __stack_top - STACK_SIZE;
}
