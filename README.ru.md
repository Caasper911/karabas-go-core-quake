# Ядро Quake для Karabas Go / Mini / Mini G

[English version](README.md)

**В разработке, пока не играет.** Попытка запустить Quake на платах семейства Karabas Go (Xilinx Spartan-6 XC6SLX25): софт-процессор VexRiscv (RISC-V) исполняет Quake (открытые под GPL исходники id Software в переносе на софт-процессор RISC-V из [PocketQuake](https://github.com/thinkelastic/PocketQuake)) на прошивке ПЛИС ядер Doom, Heretic и Hexen этого семейства (SDRAM, буфер кадра, палитра, звук).

Состояние: структура проекта. Первая фаза — замер Quake на симуляторе системы команд процессора и выбор, как считать плавающую точку (FPU у процессора пока нет). План и замеры — в `docs/PLAN.md`.

Данных игры в ядре не будет: пользователь кладёт на SD-карту свой `pak0.pak` (shareware или полной версии).

## Лицензия

GPL-3.0 (`LICENSE`), как у других ядер karabas-go. Взятые части сохраняют свои лицензии: движок Quake (и в PocketQuake) — GPL-2.0-or-later, остальное в PocketQuake — MIT, Chocolate Doom (и doomgeneric) — GPL-2.0-or-later, VexRiscv — MIT, opl3_fpga — LGPL-3.0.
