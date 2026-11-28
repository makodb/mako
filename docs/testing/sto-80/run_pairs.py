#!/usr/bin/env python3
"""Collect all predetermined paired trials, then check each median ratio."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--threads', nargs='+', type=int, required=True)
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--duration-ms', type=int, default=3000)
    parser.add_argument('--first-cpu', type=int, default=24)
    parser.add_argument('--rcu-scope', type=int, choices=(0, 1), default=1)
    parser.add_argument('--min-ratio', type=float, default=0.80)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.repeats < 1 or args.duration_ms < 1 or min(args.threads) < 1:
        parser.error('counts and durations must be positive')
    binaries = {
        'cpp': 'build_sto_reference/cpp_string_clean',
        'rust': 'build_sto_perf/cargo/release/examples/native_parity',
    }
    hashes = {name: hashlib.sha256(Path(path).read_bytes()).hexdigest()
              for name, path in binaries.items()}
    rows = []
    # Exclusive creation prevents replacing a failed or earlier qualification.
    with args.output.open('x') as output:
        for threads in args.threads:
            cpus = ','.join(str(cpu) for cpu in
                            range(args.first_cpu, args.first_cpu + threads))
            for repeat in range(args.repeats):
                order = ('cpp', 'rust') if repeat % 2 == 0 else ('rust', 'cpp')
                for writes in (0, 50):
                    for engine in order:
                        command = [
                            'taskset', '-c', cpus, binaries[engine],
                            '--threads', str(threads), '--keyspace', '100000',
                            '--ops-per-txn', '10', '--write-percent', str(writes),
                            '--warmup-ms', '700',
                            '--duration-ms', str(args.duration_ms),
                            '--seed', str(repeat + 1),
                        ]
                        if engine == 'rust':
                            command += ['--rcu-scope', str(args.rcu_scope)]
                        completed = subprocess.run(command, check=True, text=True,
                                                   capture_output=True, timeout=90)
                        reports = [line.removeprefix('BENCH_RESULT=')
                                   for line in completed.stdout.splitlines()
                                   if line.startswith('BENCH_RESULT=')]
                        if len(reports) != 1:
                            raise RuntimeError(f'expected one result: {completed.stdout}')
                        row = json.loads(reports[0])
                        row.update(engine=engine, repeat=repeat, command=command,
                                   binary_sha256=hashes[engine])
                        rows.append(row)
                        output.write(json.dumps(row) + '\n')
                        output.flush()
                        print(threads, writes, repeat, engine,
                              round(row['ops_per_sec']), flush=True)
    summary = []
    for threads in args.threads:
        for writes in (0, 50):
            medians = {
                engine: statistics.median(
                    row['ops_per_sec'] for row in rows
                    if row['threads'] == threads and row['write_percent'] == writes
                    and row['engine'] == engine)
                for engine in binaries
            }
            summary.append(dict(threads=threads, write_percent=writes,
                                median_ops_per_sec=medians,
                                ratio=medians['rust'] / medians['cpp']))
    args.output.with_suffix('.summary.json').write_text(
        json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2), flush=True)
    # Evaluate only after all trials, including all failing cells, are saved.
    return 0 if all(cell['ratio'] >= args.min_ratio for cell in summary) else 1


if __name__ == '__main__':
    raise SystemExit(main())
