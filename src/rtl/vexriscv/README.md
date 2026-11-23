Pre-generated VexRiscv (SpinalHDL/VexRiscv, MIT) from litex-hub/pythondata-cpu-vexriscv 642ecfe.
Generator options per file: see that repo verilog/Makefile.
- VexRiscv_Min: rv32i, no caches, no bypass
- VexRiscv_Lite: rv32im (iterative mul/div), I$ 2K, no D$
- VexRiscv: rv32im (single-cycle mul), **I$ 32K, D$ 32K** (direct mapped, 32-byte lines, D$ write-through)
- VexRiscv_IMAC: VexRiscv + C + A, full CSR
IO range = address bit 31 (uncached).

VexRiscv.v of the Hexen core (Ф0б 16K, Ф1б 32K, 2026-11-21) is the litex default variant with bigger caches, generated in
pythondata-cpu-vexriscv 642ecfe (submodule ext/VexRiscv b6118e5, SpinalHDL 1.9.4) with sbt 1.9.7 + Java 8:

    cd pythondata_cpu_vexriscv/verilog
    sbt -batch "runMain vexriscv.GenCoreDefault --iCacheSize 32768 --dCacheSize 32768 --outputFile VexRiscv_32K"

(Windows: --outputFile takes a plain name, the files land in the current dir.) The same command without the
cache options gives the former 4K / 4K VexRiscv.v byte for byte (only the "Git hash" comment differs). The
ports are the same; only the tag / index widths change.

Quake Ф0 (2026-11-22): `VexRiscv_FPU.v` = the same 32K / 32K core + `FpuPlugin` (RV32F, single only, internal FPU,
div / sqrt iterative: FpuDiv 14 steps, FpuSqrt 26), the generator of PocketQuake's `GenPocketQuake.scala` idea on
the litex default. Its generator `GenCoreKgFpu.scala` (here) is GenCoreDefault in package `vexriscv.kg` with an
option `--fpu`; copy it to `pythondata_cpu_vexriscv/verilog/src/main/scala/vexriscv/kg/` and run there:

    sbt -batch "runMain vexriscv.kg.GenCoreKgFpu --iCacheSize 32768 --dCacheSize 32768 --fpu true --outputFile VexRiscv_FPU"

The top module (`VexRiscv`) has the same ports as `VexRiscv.v`. XST alone (`src/measure/xst_cpu.sh`,
xc6slx25-2): rv32im 2235 LUT / 1385 FF / 34 RAMB16 + 4 RAMB8 / 4 DSP / 106 MHz; with the FPU 5141 / 3459 /
34 + 7 / 8 / 100.5 MHz.

Quake Ф6 (2026-11-23): `GenCoreKgFpu.scala` has `--iCacheWays` / `--dCacheWays` (default 1). The cache size must be a
power of two (DBusCachedPlugin asserts `isPow2(cacheSize)`: no 48K), lines per way a power of two too.
`VexRiscv_FPU.v` is now the 2-way variant:

    sbt -batch "runMain vexriscv.kg.GenCoreKgFpu --iCacheSize 32768 --dCacheSize 32768 --fpu true --iCacheWays 2 --dCacheWays 2 --outputFile VexRiscv_FPU_i2d2"

XST alone: 5250 LUT / 3491 FF / 36 RAMB16 + 5 RAMB8 / 8 DSP / 98.3 MHz (1-way: 5141 / 3459 / 34 + 7 / 8 / 100.5).
