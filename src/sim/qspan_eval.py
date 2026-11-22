#!/usr/bin/env python3
# Quake Ф3а: the model of qmodel.py ("pess", calibrated on Verilator / the board) for a run with the span
# drawer (QS lines), against a run without it. Extra CPU cost with the drawer: the uncached SRAM loads (the
# z-buffer through SRAM_UC: UC clocks each) and the MMIO stores to the drawer (IO clocks each: 5 per span
# command, 3 per z command, 2 per texture). The drawer works in parallel; a frame takes max(CPU, drawer)
# at best and CPU + drawer at worst (the CPU waits for the SRAM while the drawer is busy).
# usage: qspan_eval.py <run without> <run with the drawer> [skip] [MHz]
import sys
SKIP = int(sys.argv[3]) if len(sys.argv) > 3 else 15
MHZ = float(sys.argv[4]) if len(sys.argv) > 4 else 79.545
BASE, MISS, UC, IO = 1.50, 28, 6, 3
PESS = [1, 0, 4, 4, 18, 30, 5, 3, 3, 1, 3]

def load(fn):
    F, FF, QS = {}, {}, {}
    for l in open(fn):
        p = l.strip().split(',')
        if l.startswith('F,'): F[int(p[1])] = [int(x) for x in p[2:11]] + [int(p[12])]
        elif l.startswith('FF,'): FF[int(p[1])] = [int(x) for x in p[2:]]
        elif l.startswith('QS,'): QS[int(p[1])] = [int(x) for x in p[2:]]
    return F, FF, QS

def cpu(F, FF, QS, k):
    f = F[k]
    c = BASE * f[0] + MISS * sum(f[1:5]) + UC * f[9]
    c += sum(s * v for s, v in zip(PESS, FF.get(k, [0] * 11)))
    q = QS.get(k)
    if q:
        c += IO * (5 * q[0] + 3 * q[1])
    return c

A, B = load(sys.argv[1]), load(sys.argv[2])
keys = sorted(set(A[0]) & set(B[0]))[SKIP:]
n = len(keys)
ca = sorted(cpu(*A, k) for k in keys)
cb = [cpu(*B, k) for k in keys]
hw = [B[2].get(k, [0] * 6)[5] for k in keys]
best = sorted(max(c, h) for c, h in zip(cb, hw))
worst = sorted(c + h for c, h in zip(cb, hw))
def fps(v): return MHZ * 1e6 * n / sum(v), MHZ * 1e6 / v[n * 95 // 100]
ia = sum(A[0][k][0] for k in keys) / n; ib = sum(B[0][k][0] for k in keys) / n
q = [sum(B[2].get(k, [0] * 6)[j] for k in keys) / n for j in range(6)]
print(f'{n} frames: instr / frame {ia/1e6:.2f} -> {ib/1e6:.2f} M ({100*(ib/ia-1):+.0f} %), uncached loads / frame {sum(B[0][k][9] for k in keys)/n/1e3:.1f}k')
print(f'drawer / frame: {q[0]/1e3:.1f}k span cmds, {q[1]/1e3:.1f}k z cmds, {q[2]/1e3:.1f}k pixels, {q[3]/1e3:.1f}k z, '
      f'{q[4]/1e3:.1f}k line fills, busy {q[5]/1e6:.2f} M clocks')
print(f'without: {sum(ca)/n/1e6:.2f} M cycles, {fps(ca)[0]:.1f} / {fps(ca)[1]:.1f} fps (mean / p95) at {MHZ} MHz')
print(f'with, overlapped: {sum(best)/n/1e6:.2f} M, {fps(best)[0]:.1f} / {fps(best)[1]:.1f} fps (x{sum(ca)/sum(best):.2f})')
print(f'with, serial:     {sum(worst)/n/1e6:.2f} M, {fps(worst)[0]:.1f} / {fps(worst)[1]:.1f} fps (x{sum(ca)/sum(worst):.2f})')
