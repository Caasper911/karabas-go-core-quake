#!/bin/bash
# Quake Ф0: XST area / speed estimate of a CPU core alone (xc6slx25-2-ftg256, options of the boards):
#   xst_cpu.sh <verilog file> <name> [top]   -> src/measure/<name>/ (xst.syr), prints the summary lines
#   (top: VexRiscv by default; Quake Ф5: quake_span)
set -e
D=$(cd "$(dirname "$0")" && pwd)
V=$(cygpath -m "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")")
N=$2
X=/c/Xilinx/14.7/ISE_DS
mkdir -p "$D/$N/tmp" && cd "$D/$N"
echo "verilog work \"$V\"" > cpu.prj
cat > cpu.xst <<XST
set -tmpdir "tmp"
set -xsthdpdir "xst"
run
-ifn cpu.prj
-ofn cpu
-ofmt NGC
-p xc6slx25-2-ftg256
-top ${3:-VexRiscv}
-opt_mode Speed
-opt_level 2
-iobuf NO
-keep_hierarchy No
-ram_extract Yes
-use_dsp48 Auto
-shreg_extract YES
XST
T=$(cygpath -m "$PWD/tmp")
export TEMP="$T" TMP="$T" XILINXD_LICENSE_FILE="$(cygpath -m $X)/.xinstall/xilinx_ise.lic" XILINX="$X/ISE"
PATH="$X/ISE/bin/nt64:$X/ISE/lib/nt64:$X/common/bin/nt64:$X/common/lib/nt64:$PATH" xst -intstyle silent -ifn cpu.xst -ofn cpu.syr > /dev/null
grep -E "Number of Slice LUTs:|Number of Slice Registers:|Number of Block RAM/FIFO:|Number of DSP48A1s:|Maximum Frequency:|RAMB16BWER|RAMB8BWER" cpu.syr
