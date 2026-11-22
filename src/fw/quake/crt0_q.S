/* Quake bench / board start (karabas): gp, stack, FPU on (mstatus.FS = initial when built with F),
   KG_HW: .data saved / restored, clear .bss, main; then exit via MMIO. No C++ constructors (the
   PocketQuake libc has none). */
#ifdef KG_HW
    .section .data
    .align 2
    .global kg_boot_magic
kg_boot_magic:
    .word 0x600d0001
#endif

    .section .text.start
    .global _start
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top
#ifdef __riscv_flen
    li      t0, 0x2000
    csrs    mstatus, t0
    fscsr   zero
#endif
#ifdef KG_HW
    /* the board: the CPU restarts at 0x10000000 after a new pak or the Reset item, .data as the last run
       left it; kg_boot_magic tells the first start (save .data to __data_save) from the next ones
       (restore it), as crt0_hw.S of Hexen */
    la      a0, __data_start
    la      a1, __data_end
    la      a2, __data_save
    la      t0, kg_boot_magic
    lw      t1, 0(t0)
    li      t2, 0x600d0002
    beq     t1, t2, 5f
4:  bgeu    a0, a1, 6f
    lw      t3, 0(a0)
    sw      t3, 0(a2)
    addi    a0, a0, 4
    addi    a2, a2, 4
    j       4b
5:  bgeu    a0, a1, 6f
    lw      t3, 0(a2)
    sw      t3, 0(a0)
    addi    a0, a0, 4
    addi    a2, a2, 4
    j       5b
6:  la      t0, kg_boot_magic
    li      t1, 0x600d0002
    sw      t1, 0(t0)
#endif
    la      a0, __bss_start
    la      a1, __bss_end
1:  bgeu    a0, a1, 2f
    sw      zero, 0(a0)
    addi    a0, a0, 4
    j       1b
2:  li      a0, 0
    li      a1, 0
    call    main
    li      t0, 0x80000008
    sw      a0, 0(t0)
3:  j       3b
