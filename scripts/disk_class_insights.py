#!/usr/bin/env python3
"""Print a Markdown insights summary for one throttled-disk sweep dir.

Reads the four per-backend results.csv files inside the sweep dir and
emits a Markdown table comparing raw + honest throughput, persistence
latency, and per-role CPU at t=1, t=6, t=11. If a no-disk baseline dir
is given (or the default exists), also emits the % slowdown vs that
baseline for the headline t=11 honest throughput.

Usage:
  python3 disk_class_insights.py <sweep_dir> [<nodisk_baseline_dir>]
"""

import csv
import os
import sys


BACKENDS = [
    ("single_raft_no_pool_disk", "Single-Raft  no pool"),
    ("single_raft_pool_disk",    "Single-Raft  + pool"),
    ("multi_raft_disk",          "Multi-Raft"),
    ("paxos_disk",               "Paxos"),
]

# In the no-disk sweep dir produced by overnight_four_way.sh, the four
# per-backend CSV layouts use slightly different paths.
NODISK_LAYOUT = {
    "single_raft_no_pool_disk": "single_raft_no_pool/results.csv",
    "single_raft_pool_disk":    "single_raft_pool.csv",
    "multi_raft_disk":          "multi_raft/results.csv",
    "paxos_disk":               "paxos/results.csv",
}


def load(path):
    rows = {}
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for row in csv.DictReader(f):
            try:
                t = int(row["threads"])
            except (KeyError, ValueError, TypeError):
                continue
            rows[t] = row
    return rows


def num(row, key):
    if row is None:
        return None
    v = row.get(key, "")
    if v in ("", "N/A", None):
        return None
    try:
        return float(v)
    except ValueError:
        return None


def honest(row):
    p1 = num(row, "replay_batch_p1")
    return p1 * 400.0 / 30.0 if p1 is not None else None


def fmt_int(v):
    return f"{int(v):,}" if v is not None else "—"


def fmt_pct(delta):
    if delta is None:
        return "—"
    sign = "+" if delta >= 0 else ""
    return f"{sign}{delta:.1f}%"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    sweep_dir = sys.argv[1].rstrip("/")
    label = os.path.basename(sweep_dir)

    nodisk_base = (sys.argv[2].rstrip("/") if len(sys.argv) >= 3
                   else "results/benchmarks/nodisk_sweep_4backends_20260426_004933")

    backend_data = {}
    for be_dir, _ in BACKENDS:
        backend_data[be_dir] = load(os.path.join(sweep_dir, be_dir, "results.csv"))

    nodisk_data = {}
    if os.path.isdir(nodisk_base):
        for be_dir, _ in BACKENDS:
            nodisk_data[be_dir] = load(os.path.join(nodisk_base, NODISK_LAYOUT[be_dir]))

    print(f"# Insights — {label}")
    print()
    print(f"Sweep dir: `{sweep_dir}`  ")
    if nodisk_data:
        print(f"No-disk baseline: `{nodisk_base}`  ")
    print()

    for t in (1, 6, 11):
        print(f"## t={t}")
        print()
        print("| Backend | Raw ops/s | Honest ops/s | vs no-disk honest | "
              "Worker mean CPU% | Apply peak CPU% | Replay mean CPU% |")
        print("|---|---:|---:|---:|---:|---:|---:|")
        for be_dir, be_label in BACKENDS:
            row = backend_data[be_dir].get(t)
            raw = num(row, "throughput_ops_sec")
            hon = honest(row)
            wm  = num(row, "role_worker_mean")
            ap  = num(row, "role_apply_peak")
            rm  = num(row, "role_replay_mean")

            base_row = nodisk_data.get(be_dir, {}).get(t)
            base_hon = honest(base_row)
            delta_pct = None
            if hon is not None and base_hon and base_hon > 0:
                delta_pct = (hon - base_hon) / base_hon * 100.0

            print(f"| {be_label} | {fmt_int(raw)} | {fmt_int(hon)} | "
                  f"{fmt_pct(delta_pct)} | "
                  f"{wm if wm is not None else '—'} | "
                  f"{ap if ap is not None else '—'} | "
                  f"{rm if rm is not None else '—'} |")
        print()


if __name__ == "__main__":
    main()
