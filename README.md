# Quake core for Karabas Go / Mini / Mini G

[Русская версия](README.ru.md)

Runs Quake on the Karabas Go board family (Xilinx Spartan-6 XC6SLX25): a VexRiscv RISC-V soft CPU running Quake (the GPL source release of id Software, as ported to a RISC-V soft CPU by [PocketQuake](https://github.com/thinkelastic/PocketQuake)), on the gateware of the Doom, Heretic and Hexen cores of the same family (SDRAM, frame buffer, palette, sound).

Status: `src/quake.kg` for the Go, Mini and Mini G. Playable on all three boards: a hardware FPU (RV32F) in the CPU and a span drawer with perspective, a polygon rasterizer and the model spans in the FPGA give about 15-28 frames per second depending on the scene; sound, keyboard, mouse and pads work, saves and the settings are kept on the SD card. The game files (your own copies, in `/quake` on the SD card) are chosen in the OSD items PAK0 / PAK1 and read from the card as needed. The OSD item Fast turn limits how much new surface is built per frame (quick turns stay smoother, a bit blurrier for a moment). The plan and the measurements are in `docs/PLAN.md` (Russian).

The core will contain no game data: the user puts an own `pak0.pak` (the shareware one or the full game) on the SD card.

## Licence

GPL-3.0 (`LICENSE`), as the other karabas-go cores. Imported parts keep their own licences: the Quake engine (also in PocketQuake) is GPL-2.0-or-later, the rest of PocketQuake is MIT, Chocolate Doom (and doomgeneric) are GPL-2.0-or-later, VexRiscv is MIT, opl3_fpga is LGPL-3.0.
