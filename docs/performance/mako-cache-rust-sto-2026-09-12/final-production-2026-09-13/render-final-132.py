#!/usr/bin/env python3
"""Render the audited final medians as a standalone retained report."""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
d = json.loads((HERE / 'final-results-summary.json').read_text())
b = json.loads((HERE / 'final-source-build.json').read_text())
assert d['complete']
lines = ['# Final production-source paired benchmarks', '',
         'Completed 2026-09-13 on zoo-002. All 60 foreground cells and all 72 main',
         'sample/recovery cells passed. No private optimization arm is included.',
         'These bounded measurements do not establish performance acceptance or a',
         'default-engine cutover. C ABI remains the default.', '',
         'The final source includes record-local OCC versions, bounded atomic values,',
         'pooled wrapper logs, restricted-write HLC prefetch, the bounded read-only',
         'outcome fence, and the composite native/core health diagnostic. It excludes',
         'the rejected borrowed-value and direct-mapped resolved-key cache experiments.', '',
         'Each table reports the median of three fresh processes per engine. Percent',
         'differences divide the STO median by the C ABI median. The JSON also retains',
         'minimum and maximum observations; three repetitions are not a confidence interval.', '',
         '## Foreground one-put results', '',
         'Each worker uses 256 disjoint 8-byte keys and 128-byte values. Warmup is',
         '65,536 commits and measurement is 262,144 commits per worker. CRC is off.',
         'The ACK queue holds all admitted writes, 524,288 records per worker.',
         'Value/count verification is outside worker timing; the unapplied ACK tail',
         'is discarded afterward. These ACK numbers are not drained storage rates.', '',
         'Workers are individually pinned to physical CPUs 0 through W-1, writeback',
         'to CPU 32, and helpers to CPUs 33 through 63. All 60 PMU samples have',
         f'minimum running ratio {d["foreground_min_pmu_running_ratio"]:.8f}.', '']
fg = {(r['mode'], r['workers'], r['arm']): r['metrics'] for r in d['foreground']}
for mode, title in [('engine', 'Engine facade'), ('cache-ack', 'Cache foreground ACK')]:
    lines += [f'### {title}', '', '| Workers | C ABI Mtx/s | STO Mtx/s | STO difference | C ABI cycles/commit | STO cycles/commit |',
              '| ---: | ---: | ---: | ---: | ---: | ---: |']
    for workers in (1, 4, 8, 16, 32):
        c = fg[mode, workers, 'c-abi']; s = fg[mode, workers, 'sto']
        ct = c['transactions_per_second']['median']; st = s['transactions_per_second']['median']
        lines.append(f'| {workers} | {ct/1e6:.3f} | {st/1e6:.3f} | {(st/ct-1)*100:+.1f}% | {c["cycles_per_commit"]["median"]:.0f} | {s["cycles_per_commit"]["median"]:.0f} |')
    lines.append('')
lines += ['## Main cache workloads with drain and fresh recovery', '',
          'The unchanged main-v2 protocol uses CRC32C, Concurrent foreground mode and',
          'per-worker engine ordering. Each queue has 262,144 divided by transaction',
          'size record slots. This is not the earlier sustained-soak policy of 1,024',
          'records per worker. RocksDB WAL is enabled and sync is false.', '',
          'Read measurement/warmup counts are 262,144/65,536 commits per worker.',
          'Write and RMW counts are 65,536/16,384 divided by transaction size.',
          'Seed and warmup writes drain before measurement. Measured workers are',
          'fresh threads, so attachment and cold scratch costs remain in this interval,',
          'along with selected-key allocation, latency collection and joins.', '',
          'Workers and unpinned child helpers share CPUs 0 through W-1; writeback alone',
          'is pinned to CPU 32. The launcher stays on CPUs 33 through 63. Every cell',
          'fully drained, then passed exact per-key value and checksum checks in a',
          'fresh recovery process. There were 144 successful sample/recovery phases.', '',
          '| Workload | Keys/tx | Contention | Workers | C ABI ktx/s | STO ktx/s | STO difference |',
          '| --- | ---: | --- | ---: | ---: | ---: | ---: |']
main = {(r['workload'], r['tx'], r['contention'], r['workers'], r['arm']): r['metrics'] for r in d['main']}
configs = list(dict.fromkeys(key[:-1] for key in main))
for config in configs:
    ct = main[*config, 'c-abi']['transactions_per_second']['median']
    st = main[*config, 'sto']['transactions_per_second']['median']
    lines.append(f'| {config[0]} | {config[1]} | {config[2]} | {config[3]} | {ct/1e3:.1f} | {st/1e3:.1f} | {(st/ct-1)*100:+.1f}% |')
lines += ['', f'Low-contention measured conflicts across both engines: {d["main_low_contention_conflicts"]}.', '']
for arm in ('c-abi', 'sto'):
    metrics = main['rmw', 4, 'high', 4, arm]
    lines.append(f'High-contention RMW {arm}: median {metrics["conflicts"]["median"]:,.0f} conflicts for 65,536 measured commits; median logical p99 {metrics["logical_p99_ns"]["median"]/1000:.1f} microseconds.')
lines += ['', '## Provenance and limits', '',
          'Rust 1.95.0, commit 59807616e1fa2540724bfbac14d7976d7e4a3860,',
          'non-LTO release, opt-level 3, one codegen unit, panic abort, znver1,',
          'frame pointers enabled, debuginfo 1. Both builds used the unchanged native',
          'production archives, with no fake ABI or test hooks.', '',
          'Boost remained disabled and all 64 governors stayed schedutil. The final60',
          'and main72 ran sequentially under the same cooperative performance lock.',
          'The supervisor checked all four binary hashes and loaded-library hashes',
          'before and after both stages. Main additionally checks its binary before',
          'and after every sample and recovery phase.', '',
          f'All {d["source_files_unchanged"]} source files and {d["native_inputs_unchanged"]} native/link inputs match the final build snapshot.',
          'The source archive and complete native build manifest are retained here.',
          'Native fingerprint: 9a8e66f56c12c3a6cc0a554735084cb61db66625c571eebde17fc3dcfaa070ea.', '',
          '| Engine | Binary | SHA256 |', '| --- | --- | --- |']
for arm, targets in b['binaries'].items():
    for target, info in targets.items():
        lines.append(f'| {arm} | {target} | {info["sha256"]} |')
lines += ['', 'Immutable executables and original raw results remain on zoo-002 under',
          '`/var/tmp/mako-engine-perf-20260912.iz63eX`, with local build copies under',
          '`/var/tmp/mako-engine-perf-20260912.trsFuD`. The retained directory includes',
          'both protocols, every cell result, stdout/stderr, machine snapshots and',
          'main process-attempt memory samples. `final-results-summary.json` contains',
          'the complete medians/ranges and the audit input hashes.', '',
          'No new sustained GC soak was run on this final source. Earlier 960-second',
          'soak evidence belongs to its recorded older source and remains separate.',
          'The sizeable engine, write and RMW gaps remain. No acceptance budget was',
          'supplied, and this run does not justify changing the default engine.', '']
(HERE / 'final-production-report.md').write_text('\n'.join(lines))
