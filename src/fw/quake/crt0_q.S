/* Quake bench / board start (karabas): gp, stack, FPU on (mstatus.FS = initial when built with F),
   clear .bss, main; then exit via MMIO. No C++ constructors (the PocketQuake libc has none). */
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
