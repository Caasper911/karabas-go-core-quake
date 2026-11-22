# Ядро Quake для Karabas Go / Mini / Mini G

[English version](README.md)

**В разработке, пока не играет.** Попытка запустить Quake на платах семейства Karabas Go (Xilinx Spartan-6 XC6SLX25): софт-процессор VexRiscv (RISC-V) исполняет Quake (открытые под GPL исходники id Software в переносе на софт-процессор RISC-V из [PocketQuake](https://github.com/thinkelastic/PocketQuake)) на прошивке ПЛИС ядер Doom, Heretic и Hexen этого семейства (SDRAM, буфер кадра, палитра, звук).

Состояние: `src/quake.kg` для Mini G (на плате ещё не проверен): процессор получил аппаратный FPU (RV32F), по симуляции это около 16 кадров в секунду в среднем на первом демо (программная плавающая точка давала 3). Файл игры `pak0.pak` пока грузится в SDRAM целиком; сохранений нет, звук простой; Go и Mini — позже. План и замеры — в `docs/PLAN.md`.

Данных игры в ядре не будет: пользователь кладёт на SD-карту свой `pak0.pak` (shareware или полной версии).

## Лицензия

GPL-3.0 (`LICENSE`), как у других ядер karabas-go. Взятые части сохраняют свои лицензии: движок Quake (и в PocketQuake) — GPL-2.0-or-later, остальное в PocketQuake — MIT, Chocolate Doom (и doomgeneric) — GPL-2.0-or-later, VexRiscv — MIT, opl3_fpga — LGPL-3.0.
