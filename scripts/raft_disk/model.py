#!/usr/bin/env python3
"""The disk plan's model (docs/verus/disk-persistence-plan.md §5): F(n, K) and the
low-load and saturated estimates for the gate points, from the measured
parameters and the memory baselines (bug-fix gate, 0633e1ffc). Prints both CRC32C
variants; the plan's table is the SSE4.2 one."""
import math
s, cn, h, q = 1.0, 0.5, 1000.0, 1000.0          # us
CB = {'sse4.2': 0.80, 'software': 1.32}          # us per KiB: write 0.60 + copy 0.07 + crc
def F(kib, n, D, cb): return s + D + kib * cb + n * cn
def wait_leader(F1): return F1 * F1 / (2 * h) + F1 / 2 if F1 <= h else F1
def collect_shift(r0, F1): return q * (math.ceil((r0 + F1) / q) - math.ceil(r0 / q))
for crc, cb in CB.items():
    print(f'== CRC {crc} ({cb} us/KiB)')
    for D in (0, 200, 1000):
        out = []
        # low load: one entry per round
        for g, kib, base, r0 in (('G1', 4, 2641, 100), ('G3', 279.5, 3362, 400), ('G5', 1024, 8489, None)):
            F1 = F(kib, 1, D, cb)
            dc = F1 if r0 is None else collect_shift(r0, F1)
            dl = wait_leader(F1) + dc
            out.append(f'{g} F1={F1:7.0f} +{dl:6.0f} -> {base+dl:7.0f} ({dl/base*100:+5.1f}%)')
        # G2, traced round: 256 x 4 KiB per round
        B = 256; T = B / 34112 * 1e6
        FB = F(1024, B, D, cb)
        for rule, nf in (('tail', 2), ('sent', 1)):
            X = B / (T + nf * FB) * 1e6
            out.append(f'G2[{rule}] F(B)={FB:5.0f} X={X:7.0f}/s ({X/34112*100-100:+5.1f}%)')
        # G4 (6 partitions, 58-entry rounds) and G6 (16-entry rounds), per entry
        for g, xm, parts, kib, Bn in (('G4', 2859.4, 6, 279.5, 58), ('G6', 186.8, 1, 1024, 16)):
            t = parts / xm * 1e6; ce = kib * cb + cn
            for rule, nf in (('tail', 2), ('sent', 1)):
                X = parts / (t + nf * ce + nf * (D + s) / Bn) * 1e6
                out.append(f'{g}[{rule}] X={X:6.0f}/s ({X/xm*100-100:+5.1f}%)')
        print(f'D={D:4d}us:'); [print('   ', o) for o in out]
