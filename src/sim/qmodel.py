#!/usr/bin/env python3
# Quake Ф0 cycle model over one iss run (per-frame F / FF lines): the Doom model of model.py
#   cycles = BASE*instr + MISS*(I$ + D$ line fills)        (BASE 1.50, MISS 28: Verilator Ф0.4)
# plus, for an F build, the stall of the FPU per operation kind (VexRiscv FpuPlugin: pipelined add / mul /
# fma, iterative div (FpuDiv: 14 steps) / sqrt (FpuSqrt: 26 steps); results to the integer registers and fcvt pass the FPU pipeline too).
# Two sets: "opt" (independent ops, most latency hidden) and "pess" (every op waits for its result).
# Not calibrated: Verilator with the FPU core is the next check.
# usage: qmodel.py <run.txt> [skip frames] [MHz list]
import sys
fn = sys.argv[1]
SKIP = int(sys.argv[2]) if len(sys.argv) > 2 else 5
MHZ = [float(m) for m in sys.argv[3].split(',')] if len(sys.argv) > 3 else [75, 79.545, 100]
BASE, MISS = 1.50, 28
# kinds (iss.cpp fc_name order): flw fsw fadd fmul fdiv fsqrt fmadd fcvt fcmp fmv fminmax
STALL = {
    'opt':  [0, 0, 1, 1, 12, 24, 2, 1, 1, 0, 1],
    'pess': [1, 0, 4, 4, 18, 30, 5, 3, 3, 1, 3],
}
F, FF = {}, {}
for l in open(fn):
    if l.startswith('F,'):
        p = l.strip().split(',')
        F[int(p[1])] = [int(x) for x in p[2:11]]
    elif l.startswith('FF,'):
        p = l.strip().split(',')
        FF[int(p[1])] = [int(x) for x in p[2:]]
keys = sorted(F)[SKIP:]
if not keys:
    sys.exit('no frames')
n = len(keys)
ins = sum(F[k][0] for k in keys) / n
miss = sum(sum(F[k][1:5]) for k in keys) / n
print(f'{fn}: {n} frames (after {SKIP}), {ins/1e6:.2f} M instr / frame, {1000*miss/ins:.1f} misses / kI')
if FF:
    tot = [sum(FF.get(k, [0] * 11)[j] for k in keys) / n for j in range(11)]
    names = ['flw', 'fsw', 'fadd', 'fmul', 'fdiv', 'fsqrt', 'fmadd', 'fcvt', 'fcmp', 'fmv', 'fminmax']
    print('FP / frame: ' + ', '.join(f'{nm} {v/1e3:.1f}k' for nm, v in zip(names, tot)) +
          f'  (FP {100*sum(tot)/ins:.1f} % of instr)')
for name, st in (STALL.items() if FF else [('int', None)]):
    per = []
    for k in keys:
        c = BASE * F[k][0] + MISS * sum(F[k][1:5])
        if st:
            c += sum(s * v for s, v in zip(st, FF.get(k, [0] * 11)))
        per.append(c)
    per.sort()
    mean, p95 = sum(per) / n, per[n * 95 // 100]
    print(f'{name:>5}: {mean/1e6:7.2f} M cycles / frame  ' +
          '  '.join(f'{m:g} MHz {m*1e6/mean:5.1f} / {m*1e6/p95:4.1f} fps' for m in MHZ) + '  (mean / p95)')
