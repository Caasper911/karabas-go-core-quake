# Ф0 / Ф1 sims: make -> iss (fast ISS), obj_$(CPU)/tb_soc (Verilator CPU on memory models, CPU=VexRiscv|VexRiscv_Lite),
# obj_rtl/tb_rtl (the SoC RTL on pin models of the SDRAM / SRAM)
CPU ?= VexRiscv
OBJ := obj_$(CPU)
ifneq ($(filter MINGW% MSYS%,$(shell uname -s)),)
# msys make passes no TMP / LOCALAPPDATA / USERPROFILE to Windows programs: the gcc fell back to C:/Windows
# for its temporary files, the python of the Python install manager installed a runtime into the current dir
export TMP := $(shell cygpath -w /tmp)
export TEMP := $(TMP)
export LOCALAPPDATA := $(shell cygpath -w -F 0x1c)
export USERPROFILE := $(shell cygpath -w -F 0x28)
endif

all: iss $(OBJ)/tb_soc obj_rtl/tb_rtl

iss: iss.cpp qspan_model.h
	g++ -O2 -march=native -o $@ $<

$(OBJ)/tb_soc: tb_soc.cpp qspan_model.h ../rtl/vexriscv/$(CPU).v
	verilator --cc --exe --build -j 8 -O3 -Wno-fatal -Wno-lint -Wno-style \
	  --top-module VexRiscv --prefix Vcpu -Mdir $(OBJ) -o tb_soc \
	  -CFLAGS "-O2" -MAKEFLAGS "OPT_FAST=-O3 OPT_SLOW=-O1" ../rtl/vexriscv/$(CPU).v tb_soc.cpp >$(OBJ).log 2>&1 || { tail -20 $(OBJ).log; exit 1; }

# Ф1: the SoC RTL (doom_soc) from a dump: obj_rtl/tb_rtl
RTL := ../rtl/doom_soc.v ../rtl/doom_sdram.v ../rtl/doom_sram.v ../rtl/doom_draw.v ../rtl/vexriscv/VexRiscv.v
obj_rtl/tb_rtl: tb_rtl.cpp $(RTL)
	verilator --cc --exe --build -j 8 -O3 -Wno-fatal -Wno-lint -Wno-style \
	  --top-module doom_soc -GSDRAM_INIT_CLOCKS=100 -GRESET_VECTOR=32\'h11fff000 -Mdir obj_rtl -o tb_rtl \
	  -CFLAGS "-O2" -MAKEFLAGS "OPT_FAST=-O3 OPT_SLOW=-O1" $(RTL) tb_rtl.cpp >obj_rtl.log 2>&1 || { tail -20 obj_rtl.log; exit 1; }

# Quake Ф1: the SoC of Quake (VexRiscv_FPU, no drawer, 875 / 11 MHz) from a dump: obj_rtl_q/tb_rtl; check its
# frame hashes against obj_VexRiscv_FPU/tb_soc on the same dump (make CPU=VexRiscv_FPU obj_VexRiscv_FPU/tb_soc)
# Ф3: QSPAN=1 (default) with quake_span (WITH_QSPAN); QSPAN=0 the SoC of Ф1
QSPAN ?= 1
# Ф6: CPU_V = another CPU file (cache variants), QOBJ = its object dir (e.g. QOBJ=obj_rtl_q_i2d2)
CPU_V ?= ../rtl/vexriscv/VexRiscv_FPU.v
QOBJ ?= obj_rtl_q
RTL_Q := ../rtl/doom_soc.v ../rtl/doom_sdram.v ../rtl/doom_sram.v ../rtl/doom_draw.v ../rtl/quake_span.v $(CPU_V)
$(QOBJ)/tb_rtl: tb_rtl.cpp $(RTL_Q)
	verilator --cc --exe --build -j 8 -O3 -Wno-fatal -Wno-lint -Wno-style \
	  --top-module doom_soc -GSDRAM_INIT_CLOCKS=100 -GRESET_VECTOR=32\'h11fff000 -GWITH_DRAW=0 -GWITH_QSPAN=$(QSPAN) \
	  -GCLK_NUM=10\'d875 -GCLK_DEN=10\'d11 -Mdir $(QOBJ) -o tb_rtl \
	  -CFLAGS "-O2 -DKG_QUAKE" -MAKEFLAGS "OPT_FAST=-O3 OPT_SLOW=-O1" $(RTL_Q) tb_rtl.cpp >$(QOBJ).log 2>&1 || { tail -20 $(QOBJ).log; exit 1; }

clean:
	rm -rf iss obj_* obj_*.log
.PHONY: all clean
