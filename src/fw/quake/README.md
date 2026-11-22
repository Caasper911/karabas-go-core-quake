# src/fw/quake — платформа Quake

Движок — `upstream/pocketquake/src/firmware/quake` (PocketQuake, GPL-2) без копирования, ускорители ПЛИС
PocketQuake выключены (`HW_*` = 0). Здесь будет (Ф0, `docs/PLAN.md`):

- `sys_kg.c`, `vid_kg.c`, `snd_kg.c`, `in_kg.c` (вместо `*_pocket.c`) поверх платформы ядра (`../kg_main.c`,
  `../syscalls.c`): кадр 320x200 в SRAM 0x3f0000, палитра в MMIO, таймер, ввод (клавиатура, мышь, геймпад),
  `dataslot_read` → pak (в ISS — в памяти, на плате — секторы образа `F` через MCU);
- копии файлов движка, которые придётся менять (метки `karabas:`), и заголовки с `HW_*` = 0;
- цели `quake` (стенд ISS, `FLOAT=soft|f`) и `quake-hw` (ROM платы, Ф1) в `../Makefile`.
