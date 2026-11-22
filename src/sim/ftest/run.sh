#!/bin/sh
# Quake Ф0: build ftest.c (rv32imf), run it in the ISS and on the VexRiscv_FPU core (tb_soc, from the
# ISS dump at frame 0), compare the printed results.   run.sh [out dir, default /tmp/ftest]
set -e
D=$(cd "$(dirname "$0")" && pwd)
O=${1:-/tmp/ftest}
mkdir -p "$O"
X=$(ls -d /c/Users/*/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin | head -1)/riscv-none-elf
$X-gcc -march=rv32imf_zicsr -mabi=ilp32f -O1 -ffreestanding -nostdlib -nostartfiles \
  -T "$D/../../fw/quake/link_q.ld" "$D/../../fw/quake/crt0_q.S" "$D/ftest.c" -lgcc -o "$O/ftest.elf"
$X-objcopy -O binary "$O/ftest.elf" "$O/ftest.bin"
"$D/../iss" "$O/ftest.bin" --org 0x10000000 > "$O/iss.txt" 2>&1 || true
"$D/../iss" "$O/ftest.bin" --org 0x10000000 --dump-at 0 "$O/dump0.bin" > /dev/null 2>&1 || true
"$D/../obj_VexRiscv_FPU/tb_soc" "$O/dump0.bin" --max 200 > "$O/vl.txt" 2>&1 || true
grep -E '^[a-z]+ [0-9a-f]{8} ' "$O/iss.txt" > "$O/iss.res" || true
grep -E '^[a-z]+ [0-9a-f]{8} ' "$O/vl.txt" > "$O/vl.res" || true
echo "iss $(wc -l < "$O/iss.res") lines, verilator $(wc -l < "$O/vl.res") lines; differences:"
diff "$O/iss.res" "$O/vl.res" | grep '^[<>]' | head -40 || true
tail -2 "$O/vl.txt"
