# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

A Quake core for the Karabas Go, Mini and Mini G boards (Xilinx Spartan-6 XC6SLX25, ISE 14.7, XST), cloned on
2026-11-22 from karabas-go-core-hexen (523ca1d, history kept: Doom, Heretic, Hexen). The starting point is the Hexen
SoC and bitstreams: a soft CPU (VexRiscv rv32im, I$ / D$ 32K, 79.545 MHz), SDRAM 32 MB, SRAM 4 MB with the
320x200x8 frame, palette, frame DMA, sound mixer, OPL3, MIDI, the Doom column / span drawer.

The game: the Quake engine of `upstream/pocketquake` (thinkelastic/PocketQuake, `src/firmware/quake/`, GPL-2;
bare-metal Quake on VexiiRiscv rv32imafc in the Analogue Pocket FPGA), with its FPGA accelerators off (`HW_*` = 0)
and own platform files in `src/fw/quake/` instead of its `*_pocket.c`; `upstream/quake` (id-Software/Quake, GPL-2)
is the reference. PocketQuake already reads the pak on demand (`dataslot_read` in `sys_pocket.c`): it maps to
sector reads of an `F` image through the MCU. Submodules are read only; changed files are copied into
`src/fw/quake/` and marked `karabas:`. Docs, in Russian: `docs/PLAN.md` (Quake on top, the Hexen / Heretic /
Doom history below). Follow the phases; record a changed decision in PLAN.md.

The main open question is floating point: Quake is float everywhere, the CPU and the ISS (`src/sim/iss.cpp`) are
rv32im. Ф0 measures soft-float in the ISS and decides between soft-float, a VexRiscv FPU (RV32F) and fixed point.

No game data in git or in the .kg: the user picks `pak0.pak` (and `pak1.pak`) on the SD card (Ф1б: OSD `F` items PAK0 / PAK1, image slots 2 / 3, dir `/quake`, read by sectors through doom_sd + `src/fw/quake/kg_pak.c`; test copy
`~/Downloads/quake-sw/game/ID1/PAK0.PAK`, shareware 1.06).

State (Ф4, branch `f4-boards`; Ф0..Ф4 done, all boards ok): all boards = `VexRiscv_FPU` (32K / 32K + FpuPlugin), `doom_soc` WITH_DRAW 0,
`doom_audio` WITH_OPL 0; `quake.kg` for Go / Mini / Mini G (Ф4: Go / Mini 26112205 ok on the boards). Firmware: `make -C
src/fw quake [FLOAT=soft|f]` (ISS bench, timedemo), `quake-hw` (board; `make -C src rom kg`). The engine tree is
copied to the build dir and patched there (src/fw/Makefile, `karabas:`). Checks: `src/sim/ftest/run.sh` (RV32F ISS
vs FpuPlugin), `obj_rtl_q/tb_rtl` vs `obj_VexRiscv_FPU/tb_soc` frame hashes (not vs the ISS: FpuPlugin fmadd
rounds its own way), `qmodel.py` / `qcal.py` (fps). Big sim files go to `D:\.claude\karabas-go-core-quake`.

## Rules of the core

The rules of the other karabas-go cores apply: standard video timing with syncs always present, `FT_CLK_OUT` about 8 MHz on Mini / Mini G, Go `FDC_*` outputs 0 when unused, Mini / Mini G drive T3 (`FLASH_CS_N`) = 0, OSD switch index = item position unless `"index"`, an OSD value shows 10 characters at most, FL slots 0..7 only, Verilog-2001 for XST, imported code marked `karabas:`.

- **Ask the user before every check on the board** (and the HDMI capture). Ask before a firmware flash.
- One branch per phase of `docs/PLAN.md`, one PR at its end.
- Heavy builds: Mini G only while working on one feature, all boards for a release; `src/tools/par_multi.sh` (2 cost tables at most, time limits, the first that meets wins; cost tables: Makefile `MAP_T_*`).
- Tests and sims run in seconds; longer runs only after asking.
