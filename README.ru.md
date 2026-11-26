# Ядро Quake для Karabas Go / Mini / Mini G

[English version](README.md)

Quake на платах семейства Karabas Go (Xilinx Spartan-6 XC6SLX25): софт-процессор VexRiscv (RISC-V) исполняет Quake (открытые под GPL исходники id Software в переносе на софт-процессор RISC-V из [PocketQuake](https://github.com/thinkelastic/PocketQuake)) на прошивке ПЛИС ядер Doom, Heretic и Hexen этого семейства (SDRAM, буфер кадра, палитра, звук).

Состояние: `src/quake.kg` для Go, Mini и Mini G. Играется на всех трёх платах: аппаратный FPU (RV32F) в процессоре и рисователь в ПЛИС (спаны с перспективой, растеризатор полигонов, спаны моделей) дают около 15–28 кадров в секунду в зависимости от сцены; звук, клавиатура, мышь и джойстики работают, сохранения и настройки хранятся на SD-карте. Файлы игры (свои, в каталоге `/quake` на SD-карте) выбираются в пунктах OSD PAK0 / PAK1 и читаются с карты по мере надобности. Пункт OSD Fast turn ограничивает объём новых поверхностей за кадр (быстрые повороты плавнее, на миг чуть размытее). План и замеры — в `docs/PLAN.md`.

Данных игры в ядре не будет: пользователь кладёт на SD-карту свой `pak0.pak` (shareware или полной версии).

## Лицензия

GPL-3.0 (`LICENSE`), как у других ядер karabas-go. Взятые части сохраняют свои лицензии: движок Quake (и в PocketQuake) — GPL-2.0-or-later, остальное в PocketQuake — MIT, Chocolate Doom (и doomgeneric) — GPL-2.0-or-later, VexRiscv — MIT, opl3_fpga — LGPL-3.0.
