#!/usr/bin/env python3
"""Verdict for scripts/raft_paired_trial.sh output: per-pair relative delta
(B - A) / A, its median, how many pairs favour B, and the exact two-sided
sign test p-value. Pairs with a failed or missing run are dropped and counted."""
import csv, math, statistics, sys

rows = list(csv.DictReader(open(sys.argv[1])))
pairs = {}
for r in rows:
    pairs.setdefault(int(r['pair']), {})[r['tree']] = (r['throughput'], r['exit'])
deltas, dropped = [], 0
for p, d in sorted(pairs.items()):
    a, b = d.get('A'), d.get('B')
    if not a or not b or a[0] == 'NA' or b[0] == 'NA' or a[1] != '0' or b[1] != '0':
        dropped += 1; continue
    a, b = float(a[0]), float(b[0])
    deltas.append((b - a) / a)
n = len(deltas)
if n == 0:
    print('no completed pairs'); sys.exit(1)
favour_b = sum(1 for x in deltas if x > 0)
ties = sum(1 for x in deltas if x == 0)
m = n - ties
k = min(favour_b, m - favour_b)
p = min(1.0, 2 * sum(math.comb(m, i) for i in range(k + 1)) / 2 ** m) if m else 1.0
print(f'completed pairs: {n} (dropped {dropped})')
print(f'median delta (B-A)/A: {statistics.median(deltas)*100:+.2f}%')
print(f'mean delta:           {statistics.mean(deltas)*100:+.2f}%')
print(f'pairs favouring B:    {favour_b}/{m}   exact two-sided sign test p = {p:.3f}')
print('per-pair deltas:', ' '.join(f'{x*100:+.1f}' for x in deltas))
