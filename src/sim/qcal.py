#!/usr/bin/env python3
# Quake Ф0: Verilator cycles (tb_soc F lines) against qmodel.py on the same frames of the iss run.
# usage: qcal.py <iss run.txt> <tb_soc out.txt> [skip first N Verilator frames: cold caches, default 3]
import sys
sys.argv += [] if len(sys.argv) > 3 else ['3']
iss, vl, skip = sys.argv[1], sys.argv[2], int(sys.argv[3])
BASE, MISS = 1.50, 28
STALL = {'opt': [0, 0, 1, 1, 12, 24, 2, 1, 1, 0, 1], 'pess': [1, 0, 4, 4, 18, 30, 5, 3, 3, 1, 3]}
F, FF, V = {}, {}, {}
for l in open(iss):
    p = l.strip().split(',')
    if l.startswith('F,'): F[int(p[1])] = [int(x) for x in p[2:11]]
    elif l.startswith('FF,'): FF[int(p[1])] = [int(x) for x in p[2:]]
for l in open(vl):
    if l.startswith('F,'):
        p = l.strip().split(',')
        V[int(p[1])] = int(p[2])
keys = sorted(k for k in V if k in F)[skip:]
vc = sum(V[k] for k in keys)
ins = sum(F[k][0] for k in keys)
base = sum(BASE * F[k][0] + MISS * sum(F[k][1:5]) for k in keys)
print(f'frames {keys[0]}..{keys[-1]} ({len(keys)}): {ins/len(keys)/1e6:.2f} M instr, Verilator CPI {vc/ins:.3f}, '
      f'{vc/len(keys)/1e6:.2f} M cycles / frame = {79.545e6*len(keys)/vc:.1f} fps at 79.545 MHz')
for n, st in STALL.items():
    m = base + sum(sum(s * v for s, v in zip(st, FF.get(k, [0] * 11))) for k in keys)
    print(f'  model {n}: {m/len(keys)/1e6:.2f} M cycles / frame, Verilator / model = {vc/m:.3f}')
print(f'  model without FPU stalls: {base/len(keys)/1e6:.2f} M, Verilator / it = {vc/base:.3f}')
