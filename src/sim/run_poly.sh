#!/bin/sh
# Quake Ф7: bench of a firmware variant on the deterministic `start` hall: build (bench, F, drawer + perspective +
# QOPT, map start, +host_framerate 0.05), ISS dump at frame 30, ISS profile of 12 frames, tb_soc 12 frames.
#   [RTL=1] run_poly.sh <name> [make variables, e.g. POLY=1]  -> $OUT/<name>/{iss.txt,prof.txt,soc.txt[,rtl.txt]}, a summary
# OUT: the big files (default D:\.claude\karabas-go-core-quake\f7). Needs iss and obj_VexRiscv_FPU/tb_soc built.
set -e
D=$(cd "$(dirname "$0")" && pwd)
N=$1; shift
OUT=${OUT:-/d/.claude/karabas-go-core-quake/f7}/$N
PAK=${PAK:-/c/Users/1/Downloads/quake-sw/game/ID1/PAK0.PAK}
ARGS=${QARGS:-+con_notifytime 0 +host_framerate 0.05 +map start}
mkdir -p "$OUT"
make -C "$D/../fw" quake FLOAT=f HWSPAN=1 SCSRAM=1 QOPT=1 HWPERSP=1 QXN=d "$@" QARGS="$ARGS" > "$OUT/build.txt" 2>&1
B=$(sed -n 's/.*-o \(build-quake[^ ]*\)\/quake.elf .*/\1/p' "$OUT/build.txt" | tail -1)
[ -n "$B" ] || B=$(ls -td "$D"/../fw/build-quake-f-span-scs-o-p*-d | head -1 | xargs basename)
ELF="$D/../fw/$B/quake.elf"
"$D/iss" "$D/../fw/$B/quake.bin" --org 0x10000000 --wad "$PAK" --dump-at ${DUMPAT:-30} "$OUT/d.bin" > "$OUT/iss.txt" 2>&1 || true
"$D/iss" x --resume "$OUT/d.bin" --frames 12 --prof "$OUT/prof_s.txt" > "$OUT/iss12.txt" 2>&1 || true
python3 "$D/prof.py" "$ELF" "$OUT/prof_s.txt" 30 > "$OUT/prof.txt"
rm -f "$OUT/prof_s.txt"
"$D/obj_VexRiscv_FPU/tb_soc" "$OUT/d.bin" --frames 12 > "$OUT/soc.txt" 2>&1 || true
awk -F, -v n="$N" '/^F,/{c+=$3; k++} /^F,/ && k>0 {} END{if(k) printf "%s: %d frames, %.3f M cycles / frame, %.2f fps at 79.545 MHz\n", n, k, c/k/1e6, 79.545e6*k/c}' "$OUT/soc.txt"
awk -F, '/^F,/{c+=$3; k++} END{if(k) printf "ISS: %.3f M instructions / frame\n", c/k/1e6}' "$OUT/iss12.txt"
grep -E "^(QSPAN|QPOLY)" "$OUT/soc.txt" || true
head -12 "$OUT/prof.txt"
# RTL=1: also obj_rtl_q/tb_rtl on the dump (12 frames), its fps and the frame hashes against tb_soc
if [ -n "$RTL" ]; then
	"$D/obj_rtl_q/tb_rtl" "$OUT/d.bin" --frames 12 > "$OUT/rtl.txt" 2>&1 || true
	awk -F, -v n="$N" '/^F,/{c+=$3; k++} END{if(k) printf "%s RTL: %.3f M cycles / frame, %.2f fps at 79.545 MHz\n", n, c/k/1e6, 79.545e6*k/c}' "$OUT/rtl.txt"
	grep -E "^QSPAN" "$OUT/rtl.txt" || true
	if [ "$(grep '^F,' "$OUT/rtl.txt" | cut -d, -f4)" = "$(grep '^F,' "$OUT/soc.txt" | cut -d, -f4)" ]; then echo "frames: RTL = tb_soc"; else echo "frames: RTL != tb_soc"; fi
fi
