# src/fw/quake — платформа Quake

Здесь будет (Ф0, `docs/PLAN.md`):

- `kg_qg.c` — функции `QG_*` quakegeneric поверх платформы ядра (`../kg_main.c`, `../syscalls.c`): кадр 320x200 в
  SRAM 0x3f0000, палитра в MMIO, таймер, ввод (клавиатура, мышь, геймпад), pak из SDRAM как RAM-файл;
- копии файлов `upstream/quakegeneric/source`, которые придётся менять (метки `karabas:`);
- цели `quake` (стенд ISS) и `quake-hw` (ROM платы, Ф1) в `../Makefile`.

Сами исходники Quake берутся из `upstream/quakegeneric/source` без копирования.
