# (Windows: msys make has HOME=/home/<user>, so the xPack is also looked for in /c/Users/<user>/opt)
# Ф0 bench firmware: doomgeneric for rv32 (xPack riscv-none-elf-gcc), run by ../sim/iss.
#   make [FB=direct|copy] [LAYOUT=sdram|sram] [ARCH=rv32im] [OPT=-O2] [MB=6: zone MiB] [ARGS="-timedemo demo1"]
# FB direct: Doom draws into the SRAM frame (fbdirect.py); copy: upstream, 64 KB memcpy per frame.
# LAYOUT sdram: program + zone in SDRAM; sram: program + zone in the 4 MB SRAM (zone must be small).
# The doom objects are built once per ARCH / OPT in build-$(ARCH)$(OPT)/; LAYOUT / ARGS only relink.
XPACK   ?= $(firstword $(wildcard $(HOME)/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin /c/Users/*/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin) $(HOME)/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin)
CROSS   := $(XPACK)/riscv-none-elf-
ifneq ($(filter MINGW% MSYS%,$(shell uname -s)),)
# msys make passes no TMP / LOCALAPPDATA / USERPROFILE to Windows programs: the gcc fell back to C:/Windows
# for its temporary files, the python of the Python install manager installed a runtime into the current dir
export TMP := $(shell cygpath -w /tmp)
export TEMP := $(TMP)
export LOCALAPPDATA := $(shell cygpath -w -F 0x1c)
export USERPROFILE := $(shell cygpath -w -F 0x28)
endif
PYTHON ?= python3
ARCH    ?= rv32im
OPT     ?= -O2
LAYOUT  ?= sdram
FB      ?= direct
MB      ?= 6
ARGS    ?= -timedemo demo1
HWARGS  ?=
NAME    := doom-$(FB)-$(LAYOUT)-$(MB)

DG      := ../../upstream/doomgeneric/doomgeneric
B       := build-$(ARCH)$(subst -,,$(OPT))
# upstream Makefile SRC_DOOM without the xlib platform and w_file_stdc (w_file_kg.c instead)
DOOM    := dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale \
           f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio \
           m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy \
           p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch \
           p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump \
           st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone i_input i_video \
           doomgeneric
OBJ     := $(addprefix $(B)/,$(addsuffix .o,$(filter-out i_video,$(DOOM)))) $(B)/w_file_kg.o $(B)/i_video_$(FB).o
ABI     := -march=$(ARCH) -mabi=ilp32
DEFS    := -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200
CFLAGS  := $(ABI) $(OPT) -g -ffunction-sections -fdata-sections -w $(DEFS) -I$(DG) -I.
LDFLAGS := $(ABI) -nostartfiles --specs=nano.specs -u _printf_float -Wl,--gc-sections

ifeq ($(LAYOUT),sram)
ORG := 0x00000000
LEN := 0x3f0000
else
ORG := 0x10000000
LEN := 0x0c00000
endif

vpath %.c $(DG)

all: $(B)/$(NAME).bin

$(B):
	mkdir -p $@
$(B)/%.o: %.c | $(B)
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
$(B)/i_video_copy.o: $(DG)/i_video.c | $(B)
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
$(B)/i_video_direct.c: $(DG)/i_video.c fbdirect.py | $(B)
	$(PYTHON) fbdirect.py $< $@
$(B)/i_video_direct.o: $(B)/i_video_direct.c mmio.h
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
$(B)/w_file_kg.o: w_file_kg.c mmio.h | $(B)
	$(CROSS)gcc $(CFLAGS) -I$(DG) -c $< -o $@

OPTS := $(B)/opts-$(NAME)-$(subst $(eval) ,_,$(ARGS))
$(OPTS): | $(B)
	rm -f $(B)/opts-$(NAME)-*; touch $@
$(B)/link-$(LAYOUT).ld: link.ld.in Makefile | $(B)
	sed -e 's/@ORG@/$(ORG)/' -e 's/@LEN@/$(LEN)/' -e 's/@REGION@/$(LAYOUT)/' $< > $@
$(B)/$(NAME).elf: $(OBJ) kg_main.c syscalls.c crt0.S mmio.h $(B)/link-$(LAYOUT).ld $(OPTS)
	$(CROSS)gcc $(CFLAGS) '-DMB="$(MB)"' '-DARGS="$(ARGS)"' -c kg_main.c -o $(B)/kg_main.o
	$(CROSS)gcc $(CFLAGS) -c syscalls.c -o $(B)/syscalls.o
	$(CROSS)gcc $(ABI) -c crt0.S -o $(B)/crt0.o
	$(CROSS)gcc $(LDFLAGS) -T $(B)/link-$(LAYOUT).ld -Wl,-Map,$(B)/$(NAME).map -o $@ \
	  $(B)/crt0.o $(OBJ) $(B)/kg_main.o $(B)/syscalls.o -lm
	$(CROSS)size $@
$(B)/$(NAME).bin: $(B)/$(NAME).elf
	$(CROSS)objcopy -O binary $< $@

# the board firmware (roms/doom.bin of the .kg): kg_hw.c + crt0_hw.S, LAYOUT sdram, FB direct, and
# the sound (Ф2: FEATURE_SOUND, i_kgsound.c / i_kgmusic.c, the OPL music of chocolate-doom 2.3.0 with
# opl_kg.c and compat/): its own build dir build-hw-$(ARCH)$(OPT)/
CHOC    := ../../upstream/chocolate-doom
BH      := build-hw-$(ARCH)$(subst -,,$(OPT))
HWDEFS  := $(DEFS) -DFEATURE_SOUND
HWCFLAGS := -march=$(ARCH)_zicsr -mabi=ilp32 $(OPT) -std=gnu11 -g -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -ffunction-sections -fdata-sections -w $(HWDEFS) -I. -I$(DG) -Icompat -I$(CHOC)/opl -I$(CHOC)/src
HWOBJ   := $(addprefix $(BH)/,$(addsuffix .o,$(filter-out i_video,$(DOOM)))) $(BH)/w_file_kg.o $(BH)/i_video_direct.o \
           $(BH)/opl.o $(BH)/opl_queue.o $(BH)/i_oplmusic.o $(BH)/midifile.o $(BH)/mus2mid.o $(BH)/sdl_compat.o \
           $(BH)/kg_irq.o $(BH)/opl_kg.o $(BH)/i_kgsound.o $(BH)/i_kgmusic.o $(BH)/trap.o $(BH)/kg_save.o
$(BH):
	mkdir -p $@
$(BH)/%.o: $(DG)/%.c | $(BH)
	$(CROSS)gcc $(HWCFLAGS) -c $< -o $@
$(BH)/%.o: $(CHOC)/opl/%.c | $(BH)
	$(CROSS)gcc $(HWCFLAGS) -c $< -o $@
# copies in the build dir: their quoted includes then find the doomgeneric headers first (the
# chocolate-doom ones of the same names differ), midifile.h from $(CHOC)/src
$(BH)/choc_%.c: $(CHOC)/src/%.c | $(BH)
	cp $< $@
$(BH)/i_oplmusic.o $(BH)/midifile.o: $(BH)/%.o: $(BH)/choc_%.c compat/kg_choc_sound.h compat/SDL.h
	$(CROSS)gcc $(HWCFLAGS) -include compat/kg_choc_sound.h -include compat/SDL.h -c $< -o $@
$(BH)/sdl_compat.o: compat/sdl_compat.c compat/SDL.h | $(BH)
	$(CROSS)gcc $(HWCFLAGS) -c $< -o $@
$(BH)/i_video_direct.o: $(B)/i_video_direct.c mmio.h | $(BH)
	$(CROSS)gcc $(HWCFLAGS) -c $< -o $@
$(BH)/w_file_kg.o $(BH)/kg_irq.o $(BH)/opl_kg.o $(BH)/i_kgsound.o $(BH)/i_kgmusic.o $(BH)/kg_save.o: $(BH)/%.o: %.c mmio.h kg_irq.h kg_save.h | $(BH)
	$(CROSS)gcc $(HWCFLAGS) -c $< -o $@
$(BH)/trap.o: trap.S | $(BH)
	$(CROSS)gcc -march=$(ARCH)_zicsr -mabi=ilp32 -c $< -o $@

hw: $(BH)/doom-hw.bin
.PHONY: $(BH)/doom-hw.elf
$(BH)/doom-hw.elf: $(HWOBJ) kg_hw.c font8.h syscalls.c crt0_hw.S mmio.h $(B)/link-$(LAYOUT).ld
	$(CROSS)gcc $(HWCFLAGS) '-DHWARGS="$(HWARGS)"' -c kg_hw.c -o $(BH)/kg_hw.o
	$(CROSS)gcc $(HWCFLAGS) -c syscalls.c -o $(BH)/syscalls.o
	$(CROSS)gcc $(ABI) -c crt0_hw.S -o $(BH)/crt0_hw.o
	$(CROSS)gcc $(LDFLAGS) -Wl,--wrap=realloc -T $(B)/link-$(LAYOUT).ld -Wl,-Map,$(BH)/doom-hw.map -o $@ \
	  $(BH)/crt0_hw.o $(HWOBJ) $(BH)/kg_hw.o $(BH)/syscalls.o -lm
	$(CROSS)size $@
$(BH)/doom-hw.bin: $(BH)/doom-hw.elf
	$(CROSS)objcopy -O binary $< $@

# Heretic (chocolate-doom 2.3.0 src/heretic + the common src/ files), the Ф0 bench build:
#   make htic [ARCH=..] [OPT=..] [MB=..] [ARGS=..] -> build-htic-$(ARCH)$(OPT)/heretic-$(MB).bin (LAYOUT sdram)
# The platform is htic/kg_htic.c (main, timer, video into the SRAM frame); htic/kg_htic.h, force-included,
# replaces src/doomfeatures.h (no multiplayer / dehacked / WAD merge).
HT      := build-htic-$(ARCH)$(subst -,,$(OPT))
CHOC_COMMON := d_event d_iwad d_loop d_mode i_sound i_system m_argv m_bbox m_cheat m_config m_controls \
           m_fixed m_misc memio sha1 tables v_video w_checksum w_file w_main w_wad z_zone
HERETIC := am_map ct_chat d_main d_net f_finale g_game info in_lude m_random mn_menu p_ceilng p_doors p_enemy \
           p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch \
           p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_things sb_bar sounds s_sound
HTOBJ   := $(addprefix $(HT)/,$(addsuffix .o,$(CHOC_COMMON) $(addprefix h_,$(HERETIC)))) $(HT)/w_file_kg.o
HTCFLAGS := $(ABI) $(OPT) -std=gnu11 -g -ffunction-sections -fdata-sections -w -D_DEFAULT_SOURCE \
           -include htic/kg_htic.h -Ihtic -I. -Icompat -I$(CHOC)/src
$(HT):
	mkdir -p $@
$(HT)/%.o: $(CHOC)/src/%.c htic/kg_htic.h | $(HT)
	$(CROSS)gcc $(HTCFLAGS) -c $< -o $@
$(HT)/h_%.o: $(CHOC)/src/heretic/%.c htic/kg_htic.h | $(HT)
	$(CROSS)gcc $(HTCFLAGS) -c $< -o $@
# src/i_sound.c binds the variables of the sound modules always, the table of m_config.c has them
# only with FEATURE_SOUND
$(HT)/m_config.o: HTCFLAGS += -DFEATURE_SOUND
$(HT)/w_file_kg.o: w_file_kg.c mmio.h | $(HT)
	$(CROSS)gcc $(HTCFLAGS) -c $< -o $@
HTOPTS := $(HT)/opts-$(MB)-$(subst $(eval) ,_,$(ARGS))
$(HTOPTS): | $(HT)
	rm -f $(HT)/opts-*; touch $@
htic: $(HT)/heretic-$(MB).bin
$(HT)/heretic-$(MB).elf: $(HTOBJ) htic/kg_htic.c syscalls.c crt0.S mmio.h $(B)/link-sdram.ld $(HTOPTS)
	$(CROSS)gcc $(HTCFLAGS) '-DMB="$(MB)"' '-DARGS="$(ARGS)"' -c htic/kg_htic.c -o $(HT)/kg_htic.o
	$(CROSS)gcc $(HTCFLAGS) -c syscalls.c -o $(HT)/syscalls.o
	$(CROSS)gcc $(ABI) -c crt0.S -o $(HT)/crt0.o
	$(CROSS)gcc $(LDFLAGS) -T $(B)/link-sdram.ld -Wl,-Map,$(HT)/heretic-$(MB).map -o $@ \
	  $(HT)/crt0.o $(HTOBJ) $(HT)/kg_htic.o $(HT)/syscalls.o -lm
	$(CROSS)size $@
$(HT)/heretic-$(MB).bin: $(HT)/heretic-$(MB).elf
	$(CROSS)objcopy -O binary $< $@

# Ф1: the board firmware of Heretic (roms/heretic.bin of the .kg): htic/kg_htic_hw.c + kg_htic.c (KG_HW),
# the sound (FEATURE_SOUND: i_sound.c of chocolate with i_kgsound.c / i_kgmusic.c as its SDL modules,
# i_oplmusic.c + opl_kg.c), the saves (kg_save.c, KG_HERETIC), LAYOUT sdram: build-htic-hw-$(ARCH)$(OPT)/
HH      := build-htic-hw-$(ARCH)$(subst -,,$(OPT))
HHCFLAGS := -march=$(ARCH)_zicsr -mabi=ilp32 $(OPT) -std=gnu11 -g -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -ffunction-sections -fdata-sections -w -D_DEFAULT_SOURCE \
           -DFEATURE_SOUND -DKG_HW -DKG_HERETIC -include htic/kg_htic.h -Ihtic -I. -Icompat -I$(CHOC)/src -I$(CHOC)/opl
HHOBJ   := $(addprefix $(HH)/,$(addsuffix .o,$(CHOC_COMMON) i_oplmusic midifile mus2mid opl opl_queue \
           $(addprefix h_,$(HERETIC)))) \
           $(addprefix $(HH)/,w_file_kg.o kg_irq.o opl_kg.o i_kgsound.o i_kgmusic.o kg_save.o sdl_compat.o trap.o \
           kg_htic.o)
$(HH):
	mkdir -p $@
$(HH)/%.o: $(CHOC)/src/%.c htic/kg_htic.h | $(HH)
	$(CROSS)gcc $(HHCFLAGS) -c $< -o $@
$(HH)/%.o: $(CHOC)/opl/%.c | $(HH)
	$(CROSS)gcc $(HHCFLAGS) -c $< -o $@
$(HH)/h_%.o: $(CHOC)/src/heretic/%.c htic/kg_htic.h | $(HH)
	$(CROSS)gcc $(HHCFLAGS) -c $< -o $@
$(HH)/%.o: %.c mmio.h kg_irq.h kg_save.h | $(HH)
	$(CROSS)gcc $(HHCFLAGS) -c $< -o $@
$(HH)/sdl_compat.o: compat/sdl_compat.c compat/SDL.h | $(HH)
	$(CROSS)gcc $(HHCFLAGS) -c $< -o $@
$(HH)/kg_htic.o: htic/kg_htic.c mmio.h | $(HH)
	$(CROSS)gcc $(HHCFLAGS) -c $< -o $@
$(HH)/trap.o: trap.S | $(HH)
	$(CROSS)gcc -march=$(ARCH)_zicsr -mabi=ilp32 -c $< -o $@

htic-hw: $(HH)/heretic-hw.bin
.PHONY: $(HH)/heretic-hw.elf
$(HH)/heretic-hw.elf: $(HHOBJ) htic/kg_htic_hw.c font8.h syscalls.c crt0_hw.S mmio.h $(B)/link-sdram.ld
	$(CROSS)gcc $(HHCFLAGS) '-DHWARGS="$(HWARGS)"' -c htic/kg_htic_hw.c -o $(HH)/kg_htic_hw.o
	$(CROSS)gcc $(HHCFLAGS) -c syscalls.c -o $(HH)/syscalls.o
	$(CROSS)gcc $(ABI) -c crt0_hw.S -o $(HH)/crt0_hw.o
	$(CROSS)gcc $(LDFLAGS) -Wl,--wrap=realloc -T $(B)/link-sdram.ld -Wl,-Map,$(HH)/heretic-hw.map -o $@ \
	  $(HH)/crt0_hw.o $(HHOBJ) $(HH)/kg_htic_hw.o $(HH)/syscalls.o -lm
	$(CROSS)size $@
$(HH)/heretic-hw.bin: $(HH)/heretic-hw.elf
	$(CROSS)objcopy -O binary $< $@

# Hexen (chocolate-doom 2.3.0 src/hexen + the common src/ files), the Ф0 bench build on the platform of Heretic:
#   make hexen [ARCH=..] [OPT=..] [MB=..] [ARGS=..] -> build-hexen-$(ARCH)$(OPT)/hexen-$(MB).bin (LAYOUT sdram)
# htic/kg_htic.c with KG_GAME "hexen" + hexen/kg_hexen_stubs.c (startup screen 640x480, CD music) +
# hexen/kg_hexen_bench.c (zone peak; ARGS="-warp 1 -kgbench": the saves of a hub);
# -fcommon: src/hexen defines some globals in two files (demoextend, ...).
# HXDRAW kg (default): hexen/r_draw.c, the faster column / span drawers; up: the upstream r_draw.c;
# hw: hexen/r_draw.c with KG_HWDRAW (Ф1в: the drawer of the FPGA, the ISS models it).
HX      := build-hexen-$(ARCH)$(subst -,,$(OPT))
HXDRAW  ?= kg
HEXEN   := $(notdir $(basename $(wildcard $(CHOC)/src/hexen/*.c)))
HXOBJ   := $(addprefix $(HX)/,$(addsuffix .o,$(CHOC_COMMON) $(addprefix x_,$(filter-out $(if $(filter kg hw,$(HXDRAW)),r_draw r_segs r_plane r_things),$(HEXEN))))) $(HX)/w_file_kg.o \
           $(HX)/kg_hexen_stubs.o $(HX)/kg_hexen_bench.o $(if $(filter kg,$(HXDRAW)),$(HX)/kg_r_draw.o) \
           $(if $(filter hw,$(HXDRAW)),$(HX)/kg_r_draw_hw.o)
HXHWDRAW := $(if $(filter hw,$(HXDRAW)),-DKG_HWDRAW)
# Ф1г: memcpy / memset by words (kg_mem.c) before libc
HXOBJ   += $(HX)/kg_mem.o $(HX)/kg_lz.o
# Ф1г: hexen/r_segs.c (R_RenderSegLoop in locals) with the kg / hw drawers
HXOBJ   += $(if $(filter kg hw,$(HXDRAW)),$(HX)/kg_r_segs.o $(HX)/kg_r_plane$(if $(filter hw,$(HXDRAW)),_hw).o $(HX)/kg_r_things$(if $(filter hw,$(HXDRAW)),_hw).o)
HXCFLAGS := $(ABI) $(OPT) -std=gnu11 -g -fcommon -ffunction-sections -fdata-sections -w -D_DEFAULT_SOURCE \
           '-DKG_GAME="hexen"' -include htic/kg_htic.h -Ihtic -I. -Icompat -I$(CHOC)/src
$(HX):
	mkdir -p $@
$(HX)/%.o: $(CHOC)/src/%.c htic/kg_htic.h | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -c $< -o $@
$(HX)/x_%.o: $(CHOC)/src/hexen/%.c htic/kg_htic.h | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -I$(CHOC)/src/hexen -c $< -o $@
$(HX)/m_config.o: HXCFLAGS += -DFEATURE_SOUND
$(HX)/w_file_kg.o: w_file_kg.c mmio.h | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -c $< -o $@
$(HX)/kg_hexen_%.o: hexen/kg_hexen_%.c | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -I$(CHOC)/src/hexen -c $< -o $@
$(HX)/kg_r_segs.o: hexen/r_segs.c hexen/kg_hexen_r.h htic/kg_htic.h | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -I$(CHOC)/src/hexen -c $< -o $@
$(HX)/kg_r_plane.o: hexen/r_plane.c hexen/kg_hexen_r.h htic/kg_htic.h | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -I$(CHOC)/src/hexen -c $< -o $@
$(HX)/kg_r_plane_hw.o: hexen/r_plane.c hexen/kg_hexen_r.h htic/kg_htic.h | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -DKG_HWDRAW -I$(CHOC)/src/hexen -c $< -o $@
$(HX)/kg_r_things.o: hexen/r_things.c hexen/kg_hexen_r.h htic/kg_htic.h | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -I$(CHOC)/src/hexen -c $< -o $@
$(HX)/kg_r_things_hw.o: hexen/r_things.c hexen/kg_hexen_r.h htic/kg_htic.h | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -DKG_HWDRAW -I$(CHOC)/src/hexen -c $< -o $@
$(HX)/kg_lz.o: kg_lz.c kg_lz.h | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -c $< -o $@
$(HX)/kg_mem.o: kg_mem.c | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns -c $< -o $@
$(HX)/kg_r_draw.o: hexen/r_draw.c htic/kg_htic.h | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -I$(CHOC)/src/hexen -c $< -o $@
$(HX)/kg_r_draw_hw.o: hexen/r_draw.c htic/kg_htic.h mmio.h | $(HX)
	$(CROSS)gcc $(HXCFLAGS) -DKG_HWDRAW -I$(CHOC)/src/hexen -c $< -o $@
HXOPTS := $(HX)/opts-$(HXDRAW)-$(MB)-$(subst $(eval) ,_,$(ARGS))
$(HXOPTS): | $(HX)
	rm -f $(HX)/opts-*; touch $@
hexen: $(HX)/hexen-$(MB).bin
$(HX)/hexen-$(MB).elf: $(HXOBJ) htic/kg_htic.c syscalls.c crt0.S mmio.h $(B)/link-sdram.ld $(HXOPTS)
	$(CROSS)gcc $(HXCFLAGS) $(HXHWDRAW) '-DMB="$(MB)"' '-DARGS="$(ARGS)"' -c htic/kg_htic.c -o $(HX)/kg_htic.o
	$(CROSS)gcc $(HXCFLAGS) -c syscalls.c -o $(HX)/syscalls.o
	$(CROSS)gcc $(ABI) -c crt0.S -o $(HX)/crt0.o
	$(CROSS)gcc $(LDFLAGS) -T $(B)/link-sdram.ld -Wl,-Map,$(HX)/hexen-$(MB).map -o $@ \
	  $(HX)/crt0.o $(HXOBJ) $(HX)/kg_htic.o $(HX)/syscalls.o -lm
	$(CROSS)size $@
$(HX)/hexen-$(MB).bin: $(HX)/hexen-$(MB).elf
	$(CROSS)objcopy -O binary $< $@

# Ф1: the board firmware of Hexen (roms/hexen.bin of the .kg): the board platform of Heretic
# (htic/kg_htic_hw.c + kg_htic.c with KG_HW) for KG_HEXEN / KG_GAME "hexen"; KG_HERETIC too (the
# chocolate sound API of i_kgsound.c / i_kgmusic.c); kg_save.c keeps hexen.cfg / chocolate-hexen.cfg
# (Ф2: kg_save_fs.c, the savegames packed in the save file); hexen/r_draw.c as HXDRAW=kg: build-hexen-hw-$(ARCH)$(OPT)/
XH      := build-hexen-hw-$(ARCH)$(subst -,,$(OPT))
XHCFLAGS := -march=$(ARCH)_zicsr -mabi=ilp32 $(OPT) -std=gnu11 -g -fcommon -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -ffunction-sections -fdata-sections -w \
           -D_DEFAULT_SOURCE -DFEATURE_SOUND -DKG_HW -DKG_HERETIC -DKG_HEXEN -DKG_HWDRAW '-DKG_GAME="hexen"' \
           -include htic/kg_htic.h -Ihtic -I. -Icompat -I$(CHOC)/src -I$(CHOC)/opl
XHOBJ   := $(addprefix $(XH)/,$(addsuffix .o,$(CHOC_COMMON) i_oplmusic midifile mus2mid opl opl_queue \
           $(addprefix x_,$(filter-out r_draw r_segs r_plane r_things,$(HEXEN))))) \
           $(addprefix $(XH)/,w_file_kg.o kg_irq.o opl_kg.o i_kgsound.o i_kgmusic.o kg_save_fs.o kg_lz.o sdl_compat.o trap.o \
           kg_htic.o kg_r_draw.o kg_hexen_stubs.o)
XHOBJ   += $(XH)/kg_hexen_hw.o $(XH)/kg_mem.o $(XH)/kg_r_segs.o $(XH)/kg_r_plane.o $(XH)/kg_r_things.o
$(XH):
	mkdir -p $@
$(XH)/%.o: $(CHOC)/src/%.c htic/kg_htic.h | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -c $< -o $@
$(XH)/%.o: $(CHOC)/opl/%.c | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -c $< -o $@
$(XH)/x_%.o: $(CHOC)/src/hexen/%.c htic/kg_htic.h | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -I$(CHOC)/src/hexen -c $< -o $@
$(XH)/%.o: %.c mmio.h kg_irq.h kg_save.h | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -c $< -o $@
$(XH)/sdl_compat.o: compat/sdl_compat.c compat/SDL.h | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -c $< -o $@
$(XH)/kg_htic.o: htic/kg_htic.c mmio.h | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -c $< -o $@
$(XH)/kg_r_draw.o: hexen/r_draw.c htic/kg_htic.h | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -I$(CHOC)/src/hexen -c $< -o $@
$(XH)/kg_hexen_stubs.o: hexen/kg_hexen_stubs.c | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -c $< -o $@
$(XH)/kg_r_segs.o: hexen/r_segs.c hexen/kg_hexen_r.h htic/kg_htic.h | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -I$(CHOC)/src/hexen -c $< -o $@
$(XH)/kg_r_plane.o: hexen/r_plane.c hexen/kg_hexen_r.h htic/kg_htic.h | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -I$(CHOC)/src/hexen -c $< -o $@
$(XH)/kg_r_things.o: hexen/r_things.c hexen/kg_hexen_r.h htic/kg_htic.h | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -I$(CHOC)/src/hexen -c $< -o $@
$(XH)/kg_hexen_hw.o: hexen/kg_hexen_hw.c | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -I$(CHOC)/src/hexen -c $< -o $@
$(XH)/kg_mem.o: kg_mem.c | $(XH)
	$(CROSS)gcc $(XHCFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns -c $< -o $@
$(XH)/trap.o: trap.S | $(XH)
	$(CROSS)gcc -march=$(ARCH)_zicsr -mabi=ilp32 -c $< -o $@

hexen-hw: $(XH)/hexen-hw.bin
.PHONY: $(XH)/hexen-hw.elf
$(XH)/hexen-hw.elf: $(XHOBJ) htic/kg_htic_hw.c font8.h syscalls.c crt0_hw.S mmio.h $(B)/link-sdram.ld
	$(CROSS)gcc $(XHCFLAGS) '-DHWARGS="$(HWARGS)"' -c htic/kg_htic_hw.c -o $(XH)/kg_htic_hw.o
	$(CROSS)gcc $(XHCFLAGS) -c syscalls.c -o $(XH)/syscalls.o
	$(CROSS)gcc $(ABI) -c crt0_hw.S -o $(XH)/crt0_hw.o
	$(CROSS)gcc $(LDFLAGS) -Wl,--wrap=realloc -T $(B)/link-sdram.ld -Wl,-Map,$(XH)/hexen-hw.map -o $@ \
	  $(XH)/crt0_hw.o $(XHOBJ) $(XH)/kg_htic_hw.o $(XH)/syscalls.o -lm
	$(CROSS)size $@
$(XH)/hexen-hw.bin: $(XH)/hexen-hw.elf
	$(CROSS)objcopy -O binary $< $@

# Quake Ф0 bench (docs/PLAN.md): the engine of PocketQuake (upstream/pocketquake/src/firmware: quake/ +
# its libc/) with its FPGA accelerators off and our platform (quake/: sys_kg, vid_kg, snd_kg, kg_plat,
# sram_fill_accel.h, crt0_q.S, link_q.ld). The submodule stays untouched: the tree is copied to
# $(QB)/src, every "#define HW_<name> 1" of its headers and sources (r_surf.c has one) becomes 0 (the software paths), our files are
# laid over it (setjmp.S: its fs0..fs11 save only with F). FLOAT=soft: rv32im + soft-float (the CPU of the core now); FLOAT=f: rv32imf (VexRiscv
# FpuPlugin, variant Б). The pak: ../sim/iss --wad id1/pak0.pak (at 0x10c00000).
#   make quake [FLOAT=soft|f] [QARGS="+timedemo demo1"]  -> build-quake-$(FLOAT)/quake.bin
# Ф1, the board firmware (roms/quake.bin of the .kg): KG_HW, rv32imf, in_kg.c + kg_qhw.c (input, the wait
# for the pak, the log screen, FPS); the libc save region (0x13C00000, beyond our SDRAM) moved to SRAM 0x200000:
#   make quake-hw [QHWARGS=..]  -> build-quake-f-hw/quake.bin
QSRC    := ../../upstream/pocketquake/src/firmware
FLOAT   ?= soft
QARGS   ?= +timedemo demo1
QHWARGS ?= +_snd_mixahead 0.15
HW      ?=
# Ф3: HWSPAN=1 -> the span drawer (quake_span) does the texture and z spans (src/fw/quake/d_scan.c KG_HWSPAN)
HWSPAN  ?=
# Ф3а: SCSRAM=1 -> the surface cache in the SRAM 0x020000..0x1fffff (the drawer reads the texels there)
SCSRAM  ?=
# Ф3д: QOPT=1 -> KG_QOPT (shorter FPU chains: ceil by fcvt rup, the last span steps by a reciprocal table,
# integer izi in D_DrawZSpans)
QOPT    ?=
# Ф3е: HWPERSP=1 -> KG_HWPERSP: the perspective of D_DrawSpans8 in the drawer (needs HWSPAN=1 SCSRAM=1)
HWPERSP ?=
# Ф7: POLY=1 -> KG_POLY: the polygon driver (r_drawpolys; D_DrawPoly in quake/d_edge.c), the faces z-tested by the
# rasterizer of the drawer, no edge / span sorting (needs HWSPAN=1 SCSRAM=1 HWPERSP=1)
POLY    ?=
# Ф3в: QX = extra compiler / LTO code generation flags (the -mtune / scheduling tries), QXN = its name (build dir)
QX      ?=
QXN     ?=
QB      := build-quake-$(FLOAT)$(if $(HW),-hw)$(if $(HWSPAN),-span)$(if $(SCSRAM),-scs)$(if $(QOPT),-o)$(if $(HWPERSP),-p)$(if $(POLY),-poly)$(if $(QXN),-$(QXN))
QT      := $(QB)/src
ifeq ($(FLOAT),f)
QMARCH  := -march=rv32imf_zicsr -mabi=ilp32f
else
QMARCH  := -march=rv32im_zicsr -mabi=ilp32
endif
# their QUAKE_SRCS without the Pocket platform (sys / vid / in / cd / snd_pocket, audio_timer, net_link)
QENG    := chase cl_demo cl_input cl_main cl_parse cl_tent cmd common console crc cvar d_edge d_fill d_init \
           d_modech d_part d_polyse draw d_scan d_sky d_sprite d_surf d_vars d_zpoint host host_cmd keys \
           mathlib menu model net_link net_loop net_main net_none net_vcr nonintel pr_cmds pr_edict pr_exec \
           r_aclip r_alias r_bsp r_draw r_edge r_efrag r_light r_main r_misc r_part r_sky r_sprite r_surf \
           r_vars sbar screen sv_main sv_move sv_phys sv_user view wad world zone snd_dma snd_mix snd_mem \
           printf_pocket cd_null sys_kg vid_kg snd_kg $(if $(HW),in_kg,in_null)
QLIBC   := memory string ctype stdlib qsort time math file
QOBJ    := $(addprefix $(QB)/quake/,$(addsuffix .o,$(QENG))) $(addprefix $(QB)/libc/,$(addsuffix .o,$(QLIBC))) \
           $(QB)/libc/setjmp.o $(QB)/kg_plat.o $(QB)/kg_pak.o $(if $(HW),$(QB)/kg_qhw.o $(QB)/kg_qsave.o $(QB)/kg_save_fs.o $(QB)/kg_lz.o)
QDEFS   := $(if $(HW),-DKG_HW '-DARGS="$(QHWARGS)"',-DKG_BENCH '-DARGS="$(QARGS)"') $(if $(HWSPAN),-DKG_HWSPAN) $(if $(SCSRAM),-DKG_SCSRAM) $(if $(QOPT),-DKG_QOPT) $(if $(HWPERSP),-DKG_HWPERSP) $(if $(POLY),-DKG_POLY)
# their flags: -O3 -flto, single-precision constants, fast-math for the renderer (d_*, r_*)
QCFLAGS := $(QMARCH) -O3 -flto -g -std=gnu11 -fcommon -ffreestanding -nostdlib -fno-builtin \
           -fsingle-precision-constant -ffunction-sections -fdata-sections -w \
           -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types \
           -Wno-error=int-conversion -Wno-error=return-mismatch \
           -DPOCKET_QUAKE -DPOCKET_LINK_ENABLE=0 -DKG_QUAKE $(QDEFS) -I$(QT)/quake -I$(QT)/libc -I. $(QX)
QRFLAGS := -ffast-math -fno-math-errno -fno-trapping-math
QOVER   := quake/sys_kg.c quake/vid_kg.c quake/snd_kg.c quake/in_kg.c quake/sram_fill_accel.h quake/d_scan.c quake/r_draw.c quake/d_edge.c quake/r_sky.c quake/r_alias.c quake/d_surf.c quake/kg_hwspan.h
QOVERT  := quake/kg_plat.c quake/kg_pak.c quake/kg_qhw.c quake/kg_qsave.c quake/dataslot.h kg_save_fs.c kg_lz.c
QOPTS   := $(QB)/opts-$(shell printf "%s" "$(if $(HW),$(QHWARGS),$(QARGS))" | md5sum | cut -c1-12)

$(QT)/.stamp: $(QOVER) $(QOVERT) $(wildcard $(QSRC)/quake/*.[ch] $(QSRC)/libc/*.[chS] $(QSRC)/*.h)
	rm -rf $(QT)
	mkdir -p $(QT)
	cp -r $(QSRC)/quake $(QSRC)/libc $(QSRC)/*.h $(QT)/
	sed -i -E 's/^(#define[ \t]+HW_[A-Z0-9_]+)[ \t]+1\b/\1 0/' $(QT)/quake/*.h $(QT)/quake/*.c
	sed -i -e '/f[sl]w fs0,/i #ifdef __riscv_flen' -e '/f[sl]w fs11,/a #endif' $(QT)/libc/setjmp.S
	# karabas: no 500 us busy wait per server message during the signon (their workaround, a read of the
	# Pocket cycle counter 0x40000004: cached on our CPU, it would never end on the board)
	sed -i 's|while (\*cyc - t < 50000) {}|(void)t; /* karabas: no signon delay */|' $(QT)/quake/cl_parse.c
	grep -q 'karabas: no signon delay' $(QT)/quake/cl_parse.c
	# karabas: the colormap copy in the Pocket BRAM (0x54000000) is used by d_polyse.c whatever HW_CMAP_BRAM
	# says: point it at the colormap in memory
	sed -i 's|^#define CMAP_BRAM_PTR .*|#define CMAP_BRAM_PTR ((const unsigned char *)vid.colormap) /* karabas */|' $(QT)/quake/surface_accel.h
	grep -q 'vid.colormap) /\* karabas' $(QT)/quake/surface_accel.h
	# karabas: the save / config region of their libc in our SRAM (theirs is beyond our 32 MB SDRAM)
	sed -i 's|^#define SAV_REGION_BASE  0x13C00000|#define SAV_REGION_BASE  0x00200000 /* karabas: SRAM */|' $(QT)/libc/file.c
	grep -q 'SAV_REGION_BASE  0x00200000' $(QT)/libc/file.c
	# karabas Ф7: S_ClearBuffer silences the SFX ring of doom_audio too (snd_kg.c kg_snd_clear): the looped leftovers
	# rattled during a load
	sed -i 's|^    memset(shm->buffer, clear, shm->samples \* shm->samplebits / 8);|&\n    { extern void kg_snd_clear(void); kg_snd_clear(); }   /* karabas */|' $(QT)/quake/snd_dma.c
	grep -q 'kg_snd_clear(); }   /\* karabas' $(QT)/quake/snd_dma.c
	# karabas Ф7: no stores to the PocketQuake debug VRAM (0x20000000) per Host_Frame: on our bus that is the SRAM, and
	# the store after SCR_UpdateScreen waited for the frame copy (5 % of the clocks of the start hall in tb_rtl)
	sed -i -e "s|^\t((volatile char \*)0x20000000)\[26 \* 40 + 1\] = '.';.*|\t/* karabas: no debug VRAM write */|" \
	  -e 's|volatile char \*v = (volatile char \*)0x20000000;|static char v[26 * 40 + 2]; /* karabas: not the debug VRAM */|' $(QT)/quake/host.c
	! grep -q '^	((volatile char \*)0x20000000)\[26' $(QT)/quake/host.c
	grep -q 'karabas: not the debug VRAM' $(QT)/quake/host.c
	# karabas Ф7: the polygon driver on with KG_POLY (d_init.c), the z-buffer cleared at the frame start (r_edge.c)
	sed -i 's|^\tr_drawpolys = false;|#ifdef KG_POLY\n\tr_drawpolys = true;   /* karabas Ф7 */\n#else\n\tr_drawpolys = false;\n#endif|' $(QT)/quake/d_init.c
	grep -q 'r_drawpolys = true;   /\* karabas' $(QT)/quake/d_init.c
	sed -i 's|^\tedge_p = r_edges;|#ifdef KG_POLY\n\t{ extern void kg_poly_begin (void); kg_poly_begin (); }   /* karabas Ф7 */\n#endif\n\tedge_p = r_edges;|' $(QT)/quake/r_edge.c
	grep -q 'kg_poly_begin (); }   /\* karabas' $(QT)/quake/r_edge.c
	cp $(QOVER) $(QT)/quake/
	cp $(QOVERT) $(QT)/
	touch $@
$(QOPTS):
	mkdir -p $(QB)
	rm -f $(QB)/opts-*; touch $@
$(QB)/quake/sys_kg.o: $(QOPTS)
$(QB)/%.o: $(QT)/.stamp mmio.h
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(QCFLAGS) $(if $(filter quake/d_% quake/r_%,$*),$(QRFLAGS)) -c $(QT)/$*.c -o $@
$(QB)/libc/setjmp.o: $(QT)/.stamp
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(QMARCH) -c $(QT)/libc/setjmp.S -o $@
$(QB)/crt0_q.o: quake/crt0_q.S
	@mkdir -p $(dir $@)
	$(CROSS)gcc $(QMARCH) $(if $(HW),-DKG_HW) -c $< -o $@
quake: $(QB)/quake.bin
$(QB)/quake.elf: $(QB)/crt0_q.o $(QOBJ) quake/link_q.ld
	$(CROSS)gcc $(QMARCH) -O3 -flto $(QX) -nostdlib -nostartfiles -T quake/link_q.ld -Wl,--gc-sections \
	  -Wl,-Map,$(QB)/quake.map -o $@ $(QB)/crt0_q.o $(QOBJ) -lgcc
	$(CROSS)size $@
$(QB)/quake.bin: $(QB)/quake.elf
	$(CROSS)objcopy -O binary $< $@
quake-hw:
	$(MAKE) quake FLOAT=f HW=1 HWSPAN=$(HWSPAN) SCSRAM=$(SCSRAM) QOPT=$(QOPT) HWPERSP=$(HWPERSP) POLY=$(POLY)

clean:
	rm -rf build-*
.PHONY: all clean hw htic htic-hw hexen hexen-hw quake quake-hw
