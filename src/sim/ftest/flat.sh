#!/bin/sh
# Quake Ф3г: FpuPlugin latencies (flat.c) on tb_soc VexRiscv_FPU. flat.sh [out dir] [tb_soc]
set -e
D=$(cd "$(dirname "$0")" && pwd)
O=${1:-$D/../out/flat}
T=${2:-$D/../obj_VexRiscv_FPU/tb_soc}
mkdir -p "$O"
X=$(ls -d /c/Users/*/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin | head -1)/riscv-none-elf
$X-gcc -march=rv32imf_zicsr -mabi=ilp32f -O1 -ffreestanding -nostdlib -nostartfiles \
  -T "$D/../../fw/quake/link_q.ld" "$D/../../fw/quake/crt0_q.S" "$D/flat.c" -lgcc -o "$O/flat.elf" 2>&1 | grep -v RWX || true
$X-objcopy -O binary "$O/flat.elf" "$O/flat.bin"
"$D/../iss" "$O/flat.bin" --org 0x10000000 --dump-at 0 "$O/dump0.bin" > /dev/null 2>&1 || true
"$T" "$O/dump0.bin" --max 50 2>&1 | grep -v "^loaded\|^F,\|^QSPAN\|^END"
