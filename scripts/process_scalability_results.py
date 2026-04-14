#!/usr/bin/env python3
"""
Process scalability sweep results from all three backends and produce
comparison tables for the thesis.

Usage:
  python3 scripts/process_scalability_results.py \
    --paxos results/benchmarks/paxos/scalability_latest/results.csv \
    --raft-multi results/benchmarks/raft-multi/scalability_latest/results.csv \
    --raft-single results/benchmarks/raft-single/scalability_latest/results.csv \
    [--output results/thesis_tables.txt]

Any subset of backends can be provided (e.g., just --paxos for Act 1).
"""

import argparse
import csv
import math
import sys
from collections import defaultdict


def load_csv(path):
    """Load results.csv and group by thread count."""
    data = defaultdict(list)  # thread_count -> list of row dicts
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            trd = int(row['threads'])
            data[trd].append(row)
    return data


def compute_stats(values):
    """Compute mean, stdev, CV from a list of floats."""
    n = len(values)
    if n == 0:
        return None, None, None
    mean = sum(values) / n
    if n > 1:
        variance = sum((x - mean) ** 2 for x in values) / (n - 1)
        stdev = math.sqrt(variance)
    else:
        stdev = 0.0
    cv = (stdev / mean * 100) if mean > 0 else 0.0
    return mean, stdev, cv


def extract_throughputs(data, thread_count):
    """Extract valid throughput values for a thread count."""
    values = []
    for row in data.get(thread_count, []):
        tp = row.get('throughput_ops_sec', '')
        if tp:
            try:
                values.append(float(tp))
            except ValueError:
                pass
    return values


def extract_cpu(data, thread_count):
    """Extract valid avg CPU% values for a thread count."""
    values = []
    for row in data.get(thread_count, []):
        cpu = row.get('avg_cpu_pct', '')
        if cpu:
            try:
                values.append(float(cpu))
            except ValueError:
                pass
    return values


def extract_active_threads(data, thread_count):
    """Extract active thread counts."""
    values = []
    for row in data.get(thread_count, []):
        at = row.get('active_threads', '')
        if at:
            try:
                values.append(float(at))
            except ValueError:
                pass
    return values


def get_all_thread_counts(*datasets):
    """Get sorted union of all thread counts across datasets."""
    counts = set()
    for data in datasets:
        if data:
            counts.update(data.keys())
    return sorted(counts)


def fmt(val, width=10, decimals=0):
    """Format a number or return 'N/A'."""
    if val is None:
        return f"{'N/A':>{width}}"
    if decimals == 0:
        return f"{val:>{width},.0f}"
    return f"{val:>{width},.{decimals}f}"


def fmt_pct(val, width=8):
    """Format a percentage or return 'N/A'."""
    if val is None:
        return f"{'N/A':>{width}}"
    return f"{val:>{width}.1f}%"


def print_table(header, rows, col_widths=None):
    """Print a formatted ASCII table."""
    if col_widths is None:
        col_widths = [max(len(str(row[i])) for row in [header] + rows) + 2
                      for i in range(len(header))]

    def fmt_row(row):
        return "| " + " | ".join(
            f"{str(row[i]):>{col_widths[i]}}" if i < len(row) else f"{'':>{col_widths[i]}}"
            for i in range(len(header))
        ) + " |"

    sep = "+" + "+".join("-" * (w + 2) for w in col_widths) + "+"

    print(sep)
    print(fmt_row(header))
    print(sep)
    for row in rows:
        print(fmt_row(row))
    print(sep)


def main():
    parser = argparse.ArgumentParser(description="Process Mako scalability sweep results")
    parser.add_argument('--paxos', help='Path to Paxos results.csv')
    parser.add_argument('--raft-multi', help='Path to Multi-Raft results.csv')
    parser.add_argument('--raft-single', help='Path to Single-Raft results.csv')
    parser.add_argument('--output', '-o', help='Output file (default: stdout)')
    args = parser.parse_args()

    if not any([args.paxos, args.raft_multi, args.raft_single]):
        parser.error("At least one backend CSV required")

    # Load data
    paxos_data = load_csv(args.paxos) if args.paxos else None
    multi_data = load_csv(args.raft_multi) if args.raft_multi else None
    single_data = load_csv(args.raft_single) if args.raft_single else None

    datasets = [d for d in [paxos_data, multi_data, single_data] if d]
    thread_counts = get_all_thread_counts(*datasets)

    # Redirect output if needed
    out = open(args.output, 'w') if args.output else sys.stdout
    orig_stdout = sys.stdout
    sys.stdout = out

    print("=" * 80)
    print("  MAKO SCALABILITY RESULTS — CROSS-BACKEND COMPARISON")
    print("=" * 80)
    print()

    # ================================================================
    # Table 1: Throughput Scalability
    # ================================================================
    print("TABLE 1: Throughput Scalability (ops/sec)")
    print("-" * 80)

    header = ["Threads"]
    has_paxos = paxos_data is not None
    has_multi = multi_data is not None
    has_single = single_data is not None

    if has_paxos:
        header += ["Paxos", "+/-"]
    if has_multi:
        header += ["Multi-Raft", "+/-"]
    if has_single:
        header += ["Single-Raft", "+/-"]
    if has_paxos and has_multi:
        header.append("Multi/Paxos")
    if has_multi and has_single:
        header.append("Single/Multi")

    rows = []
    # Store means for ratio computation
    paxos_means = {}
    multi_means = {}
    single_means = {}

    for trd in thread_counts:
        row = [str(trd)]

        if has_paxos:
            vals = extract_throughputs(paxos_data, trd)
            mean, stdev, _ = compute_stats(vals)
            paxos_means[trd] = mean
            row.append(fmt(mean))
            row.append(fmt(stdev))
        if has_multi:
            vals = extract_throughputs(multi_data, trd)
            mean, stdev, _ = compute_stats(vals)
            multi_means[trd] = mean
            row.append(fmt(mean))
            row.append(fmt(stdev))
        if has_single:
            vals = extract_throughputs(single_data, trd)
            mean, stdev, _ = compute_stats(vals)
            single_means[trd] = mean
            row.append(fmt(mean))
            row.append(fmt(stdev))
        if has_paxos and has_multi:
            pm = paxos_means.get(trd)
            mm = multi_means.get(trd)
            if pm and mm and pm > 0:
                row.append(f"{mm/pm:.2f}")
            else:
                row.append("N/A")
        if has_multi and has_single:
            mm = multi_means.get(trd)
            sm = single_means.get(trd)
            if mm and sm and mm > 0:
                row.append(f"{sm/mm:.2f}")
            else:
                row.append("N/A")

        rows.append(row)

    print_table(header, rows)
    print()

    # ================================================================
    # Table 2: Scaling Efficiency
    # ================================================================
    print("TABLE 2: Scaling Efficiency (actual / ideal_linear * 100%)")
    print("-" * 80)

    header2 = ["Threads"]
    if has_paxos:
        header2.append("Paxos Eff%")
    if has_multi:
        header2.append("Multi-Raft Eff%")
    if has_single:
        header2.append("Single-Raft Eff%")

    # Get baseline (1-thread or smallest thread count)
    base_trd = thread_counts[0] if thread_counts else 1
    paxos_base = paxos_means.get(base_trd)
    multi_base = multi_means.get(base_trd)
    single_base = single_means.get(base_trd)

    rows2 = []
    for trd in thread_counts:
        row = [str(trd)]
        scale_factor = trd / base_trd  # handles non-1 base

        if has_paxos:
            pm = paxos_means.get(trd)
            if pm and paxos_base and paxos_base > 0:
                eff = (pm / (paxos_base * scale_factor)) * 100
                row.append(f"{eff:.0f}%")
            else:
                row.append("N/A")
        if has_multi:
            mm = multi_means.get(trd)
            if mm and multi_base and multi_base > 0:
                eff = (mm / (multi_base * scale_factor)) * 100
                row.append(f"{eff:.0f}%")
            else:
                row.append("N/A")
        if has_single:
            sm = single_means.get(trd)
            if sm and single_base and single_base > 0:
                eff = (sm / (single_base * scale_factor)) * 100
                row.append(f"{eff:.0f}%")
            else:
                row.append("N/A")

        rows2.append(row)

    print_table(header2, rows2)
    print()

    # ================================================================
    # Table 3: CPU Usage
    # ================================================================
    print("TABLE 3: CPU Usage (100% = 1 core)")
    print("-" * 80)

    header3 = ["Threads"]
    if has_paxos:
        header3.append("Paxos CPU%")
    if has_multi:
        header3.append("Multi-Raft CPU%")
    if has_single:
        header3.append("Single-Raft CPU%")
    if has_paxos and has_single:
        header3.append("Single/Paxos CPU")

    rows3 = []
    for trd in thread_counts:
        row = [str(trd)]
        paxos_cpu = None
        single_cpu = None

        if has_paxos:
            vals = extract_cpu(paxos_data, trd)
            mean, _, _ = compute_stats(vals)
            paxos_cpu = mean
            row.append(f"{mean:.0f}%" if mean else "N/A")
        if has_multi:
            vals = extract_cpu(multi_data, trd)
            mean, _, _ = compute_stats(vals)
            row.append(f"{mean:.0f}%" if mean else "N/A")
        if has_single:
            vals = extract_cpu(single_data, trd)
            mean, _, _ = compute_stats(vals)
            single_cpu = mean
            row.append(f"{mean:.0f}%" if mean else "N/A")
        if has_paxos and has_single:
            if paxos_cpu and single_cpu and paxos_cpu > 0:
                row.append(f"{single_cpu/paxos_cpu:.2f}")
            else:
                row.append("N/A")

        rows3.append(row)

    print_table(header3, rows3)
    print()

    # ================================================================
    # Table 4: Reproducibility (CV%)
    # ================================================================
    print("TABLE 4: Reproducibility (Coefficient of Variation)")
    print("-" * 80)

    header4 = ["Threads"]
    if has_paxos:
        header4.append("Paxos CV%")
    if has_multi:
        header4.append("Multi-Raft CV%")
    if has_single:
        header4.append("Single-Raft CV%")

    rows4 = []
    for trd in thread_counts:
        row = [str(trd)]
        if has_paxos:
            vals = extract_throughputs(paxos_data, trd)
            _, _, cv = compute_stats(vals)
            row.append(f"{cv:.1f}%" if cv is not None else "N/A")
        if has_multi:
            vals = extract_throughputs(multi_data, trd)
            _, _, cv = compute_stats(vals)
            row.append(f"{cv:.1f}%" if cv is not None else "N/A")
        if has_single:
            vals = extract_throughputs(single_data, trd)
            _, _, cv = compute_stats(vals)
            row.append(f"{cv:.1f}%" if cv is not None else "N/A")
        rows4.append(row)

    print_table(header4, rows4)
    print()

    # ================================================================
    # Summary interpretation
    # ================================================================
    print("=" * 80)
    print("  INTERPRETATION GUIDE")
    print("=" * 80)
    print()
    print("  Throughput ratios (Multi/Paxos, Single/Multi):")
    print("    ~1.0   = Expected (replication is not the bottleneck)")
    print("    < 0.9  = Investigate (unexpected regression)")
    print("    > 1.1  = Investigate (unexpected improvement)")
    print()
    print("  Scaling efficiency:")
    print("    > 90%  = Near-linear scaling (excellent)")
    print("    70-90% = Sub-linear but acceptable")
    print("    < 70%  = Bottleneck (investigate contention)")
    print()
    print("  CV% (reproducibility):")
    print("    < 5%   = Thesis-ready")
    print("    5-10%  = Acceptable")
    print("    > 10%  = Investigate variance")
    print()

    if args.output:
        sys.stdout = orig_stdout
        out.close()
        print(f"Results saved to: {args.output}")


if __name__ == '__main__':
    main()
