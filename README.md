# Quake core for Karabas Go / Mini / Mini G

[Русская версия](README.ru.md)

Runs Quake on the Karabas Go board family (Xilinx Spartan-6 XC6SLX25): a VexRiscv RISC-V soft CPU running Quake (the GPL source release of id Software, as ported to a RISC-V soft CPU by [PocketQuake](https://github.com/thinkelastic/PocketQuake)), on the gateware of the Doom, Heretic and Hexen cores of the same family (SDRAM, frame buffer, palette, sound).

Status: `src/quake.kg` for the Go, Mini and Mini G. On the Mini G it is playable: a hardware FPU (RV32F) in the CPU and a perspective span drawer in the FPGA give about 12-28 frames per second depending on the scene; sound, keyboard, mouse and pads work, saves and the settings are kept on the SD card. The Go and Mini bitstreams are new and not yet tried on the boards. The game file `pak0.pak` (your own copy, put it in `/quake` on the SD card) is loaded whole into the SDRAM at start (15-20 s). The plan and the measurements are in `docs/PLAN.md` (Russian).

The core will contain no game data: the user puts an own `pak0.pak` (the shareware one or the full game) on the SD card.

## Licence

GPL-3.0 (`LICENSE`), as the other karabas-go cores. Imported parts keep their own licences: the Quake engine (also in PocketQuake) is GPL-2.0-or-later, the rest of PocketQuake is MIT, Chocolate Doom (and doomgeneric) are GPL-2.0-or-later, VexRiscv is MIT, opl3_fpga is LGPL-3.0.
