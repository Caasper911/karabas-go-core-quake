# Quake core for Karabas Go / Mini / Mini G

[Русская версия](README.ru.md)

**In development, nothing playable yet.** An attempt to run Quake on the Karabas Go board family (Xilinx Spartan-6 XC6SLX25): a VexRiscv RISC-V soft CPU running Quake (the GPL source release of id Software, as ported to a RISC-V soft CPU by [PocketQuake](https://github.com/thinkelastic/PocketQuake)), on the gateware of the Doom, Heretic and Hexen cores of the same family (SDRAM, frame buffer, palette, sound).

Status: `src/quake.kg` for the Mini G (not yet tried on the board): the CPU got a hardware FPU (RV32F), which simulation says gives about 16 frames per second on average in the first demo (soft floating point gave 3). The game file `pak0.pak` is loaded whole into the SDRAM for now; no saves and a simple sound path yet; the Go and Mini come later. The plan and the measurements are in `docs/PLAN.md` (Russian).

The core will contain no game data: the user puts an own `pak0.pak` (the shareware one or the full game) on the SD card.

## Licence

GPL-3.0 (`LICENSE`), as the other karabas-go cores. Imported parts keep their own licences: the Quake engine (also in PocketQuake) is GPL-2.0-or-later, the rest of PocketQuake is MIT, Chocolate Doom (and doomgeneric) are GPL-2.0-or-later, VexRiscv is MIT, opl3_fpga is LGPL-3.0.
