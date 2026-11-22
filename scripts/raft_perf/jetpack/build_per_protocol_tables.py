#!/usr/bin/env python3
"""Build per-protocol detail tables for max_throughput_core17_<date>.md.

Parses the <proto>-adaptive.csv produced by scripts/run_adaptive_sweep.sh
(one per protocol under results/<date>-<proto>-core17/) and emits a Markdown
table per protocol with every N attempted (baseline + probes + bisection),
bolding the peak tput. For CURP it also picks up the hand-run N=55/60/65/70
points from the .res files since the adaptive bisection misses them.

Usage:
  python3 scripts/build_per_protocol_tables.py \
      [--date 2026-04-20] [--results-root /path/to/results]

Paste the output into the "Full per-N detail per protocol" section of the
dated max-throughput doc.
"""

import argparse
import os
import csv
import re
import sys


def _default_results_root():
    # scripts/ → repo root → results/
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(os.path.dirname(here), "results")


_ap = argparse.ArgumentParser(description=__doc__)
_ap.add_argument("--date", default="2026-04-20",
                 help="date prefix for result dirs (default: 2026-04-20)")
_ap.add_argument("--results-root", default=_default_results_root(),
                 help="root of results/ (default: <repo>/results)")
_args = _ap.parse_args()

RESULTS_ROOT = _args.results_root
DATE = _args.date

# Order matches the Peak table in the doc (ranked by peak throughput).
PROTOCOLS = [
    ("naive_epaxos",       "naive-epaxos"),
    ("epaxos",             "epaxos"),
    ("naive_raft",         "naive-raft"),
    ("swiftpaxos",         "swiftpaxos"),
    ("raft",               "raft"),
    ("jp-raft-adaptive",   "jp-raft-adaptive"),
    ("jp-raft-fp100",      "jp-raft-fp100"),
    ("etcd",               "etcd"),
    ("CURP",               "curp"),
]

RE_STATS = re.compile(r"All-efficient-attempts\s+statistics")
RE_MID = re.compile(r"Mid throughput is\s+([\d.]+)")
RE_CPU = re.compile(r"server average\s*:\s*([\d.]+)")

def parse_res(path):
    """Return dict with tput, cpu, p50, p90, p99 for a single .res file."""
    tp = cpu = p50 = p90 = p99 = None
    try:
        with open(path) as f:
            for line in f:
                m = RE_MID.search(line)
                if m:
                    tp = float(m.group(1))
                m = RE_CPU.search(line)
                if m:
                    cpu = float(m.group(1))
                if RE_STATS.search(line):
                    toks = line.split()
                    for i, t in enumerate(toks):
                        if t == "50pct" and i+1 < len(toks):
                            p50 = float(toks[i+1])
                        elif t == "90pct" and i+1 < len(toks):
                            p90 = float(toks[i+1])
                        elif t == "99pct" and i+1 < len(toks):
                            p99 = float(toks[i+1])
    except FileNotFoundError:
        pass
    return {"tput": tp, "cpu": cpu, "p50": p50, "p90": p90, "p99": p99}

def collect_manual_rows(dir_name, prefix, extra_Ns):
    """For each N in extra_Ns that has all 5 zoo.res files, synthesize a
    CSV-style row. Used for manually-attempted N points not in the adaptive
    CSV (e.g. CURP N=55/60/65/70)."""
    rows = []
    base = os.path.join(RESULTS_ROOT, f"{DATE}-{dir_name}-core17")
    for N in extra_Ns:
        zoo = [parse_res(os.path.join(base, f"{prefix}-N{N}c500-zoo{i}.res"))
               for i in (1, 2, 3, 4, 5)]
        if not all(z["tput"] is not None for z in zoo):
            continue
        tput_total = sum(z["tput"] for z in zoo)
        avg_cpu = sum((z["cpu"] or 0) for z in zoo) / 5
        # Use zoo2 and zoo3 for p50/p90/p99; the CSV column schema keeps
        # zoo2 as the authoritative latency host in this cluster layout.
        rows.append({
            "N": str(N),
            "tput": f"{tput_total:.1f}",
            "zoo2_p50": f"{zoo[1]['p50']:.2f}" if zoo[1]['p50'] is not None else "-1.00",
            "zoo2_p90": f"{zoo[1]['p90']:.2f}" if zoo[1]['p90'] is not None else "-1.00",
            "zoo2_p99": f"{zoo[1]['p99']:.2f}" if zoo[1]['p99'] is not None else "-1.00",
            "zoo3_p50": f"{zoo[2]['p50']:.2f}" if zoo[2]['p50'] is not None else "-1.00",
            "zoo1_cpu": f"{zoo[0]['cpu']:.3f}" if zoo[0]['cpu'] is not None else "0",
            "zoo2_cpu": f"{zoo[1]['cpu']:.3f}" if zoo[1]['cpu'] is not None else "0",
            "zoo3_cpu": f"{zoo[2]['cpu']:.3f}" if zoo[2]['cpu'] is not None else "0",
            "zoo4_cpu": f"{zoo[3]['cpu']:.3f}" if zoo[3]['cpu'] is not None else "0",
            "zoo5_cpu": f"{zoo[4]['cpu']:.3f}" if zoo[4]['cpu'] is not None else "0",
            "avg_cpu":  f"{avg_cpu:.3f}",
            "stopped": "manual",
        })
    return rows

def load_csv(dir_name, prefix):
    path = os.path.join(RESULTS_ROOT, f"{DATE}-{dir_name}-core17", f"{prefix}-adaptive.csv")
    if not os.path.exists(path):
        return None
    rows = []
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            rows.append(row)
    # Sort by N ascending, but keep bisection order stable.
    # Actually sort by search order as recorded in CSV (already in order).
    return rows

def fmt(v, precision=2):
    try:
        fv = float(v)
        if fv == 0 and precision > 0:
            return "—"
        return f"{fv:.{precision}f}"
    except Exception:
        return str(v)

def fmt_cpu(v):
    try:
        fv = float(v)
        if fv < 0.01:
            return "—"
        return f"{fv:.1f}"
    except Exception:
        return str(v)

def fmt_tput(v):
    try:
        fv = float(v)
        if fv == 0:
            return "**0**"
        return f"{fv:,.0f}".replace(",", " ")
    except Exception:
        return str(v)

def build_protocol_table(label, rows):
    out = []
    out.append(f"### {label}")
    out.append("")
    if not rows:
        out.append(f"_No adaptive CSV found for `{label}`._")
        out.append("")
        return "\n".join(out)
    out.append("| N | tput | zoo2 p50 | zoo2 p90 | zoo2 p99 | zoo3 p50 | zoo1 cpu | zoo2 cpu | zoo3 cpu | zoo4 cpu | zoo5 cpu | avg cpu | stopped |")
    out.append("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---|")
    # Peak: max tput among unsaturated points (ok / bisect-ok / baseline).
    # Matches the headline Peak per protocol table (which uses the last
    # unsaturated bisect-ok, not the highest throughput at a saturated N).
    unsat = [r for r in rows if r["stopped"] in ("ok", "bisect-ok", "baseline", "manual")]
    # Among manual rows, only treat as "unsat" those with p99 under the
    # sensible saturation envelope (p99 < 2 × WAN_RTT × 100 ms is a loose
    # bound; tighter here: treat p99 > 1000 ms as saturated regardless of
    # the `manual` label). Keeps the peak bolded at a realistic point.
    def _sane(r):
        try:
            return float(r["zoo2_p99"]) < 1000.0
        except Exception:
            return True
    unsat = [r for r in unsat if r["stopped"] != "manual" or _sane(r)]
    peak_tput = max((float(r["tput"]) for r in unsat), default=0)
    for r in rows:
        tput_cell = fmt_tput(r["tput"])
        is_peak = (r in unsat) and abs(float(r["tput"]) - peak_tput) < 0.1 and peak_tput > 0
        if is_peak:
            tput_cell = f"**{tput_cell}**"
        out.append("| {n} | {tput} | {p2_50} | {p2_90} | {p2_99} | {p3_50} | {c1} | {c2} | {c3} | {c4} | {c5} | {ca} | {st} |".format(
            n=r["N"],
            tput=tput_cell,
            p2_50=fmt(r["zoo2_p50"]),
            p2_90=fmt(r["zoo2_p90"]),
            p2_99=fmt(r["zoo2_p99"]),
            p3_50=fmt(r["zoo3_p50"]),
            c1=fmt_cpu(r["zoo1_cpu"]),
            c2=fmt_cpu(r["zoo2_cpu"]),
            c3=fmt_cpu(r["zoo3_cpu"]),
            c4=fmt_cpu(r["zoo4_cpu"]),
            c5=fmt_cpu(r["zoo5_cpu"]),
            ca=fmt_cpu(r["avg_cpu"]),
            st=r["stopped"],
        ))
    out.append("")
    return "\n".join(out)

chunks = []
chunks.append("## Full per-N detail per protocol")
chunks.append("")
chunks.append("Every N attempted during the adaptive sweep (baseline + 50/100 probes + upward extension + bisection), in search order. **Bold** tput = peak. Throughput is cluster-wide `cmd/s` from the mid-10 s window; p50/p90/p99 are client-reported, in ms; per-host cpu is the core-17 mean %. `—` in CPU columns means missing sample (the sweep occasionally reports `0` when `/proc/stat` sampling fails on a host).")
chunks.append("")
chunks.append("Column semantics match the Peak per protocol table above. `stopped` is the adaptive-sweep verdict: `baseline` = N=1 seed, `ok` = probe under 2× baseline, `stop` = probe over 2× baseline (triggered search termination), `bisect-ok`/`bisect-stop` = bisection step outcome.")
chunks.append("")
# Manual N points run outside the adaptive sweep, to be merged per protocol.
# For CURP: N=55/60/65/70 were run manually (see CURP notes in the doc);
# the adaptive sweep's bisection collapsed at N=100 so the 11 973 @ N=60
# peak is only visible via these manual runs.
MANUAL_EXTRA = {
    "CURP": [55, 60, 65, 70],
}

def sort_rows_by_N(rows):
    """Sort by numeric N, keeping order stable within same N."""
    return sorted(rows, key=lambda r: int(r["N"]))

for label, dir_name in PROTOCOLS:
    # prefix convention: most use the same name, curp uses "curp".
    prefix = dir_name
    if label == "naive_epaxos":
        prefix = "naive_epaxos"
    elif label == "naive_raft":
        prefix = "naive_raft"
    rows = load_csv(dir_name, prefix) or []
    extras = collect_manual_rows(dir_name, prefix, MANUAL_EXTRA.get(label, []))
    # Avoid duplicates: drop any extra whose N already appears in the CSV.
    existing_Ns = {r["N"] for r in rows}
    rows.extend(r for r in extras if r["N"] not in existing_Ns)
    rows = sort_rows_by_N(rows)
    chunks.append(build_protocol_table(label, rows))

print("\n".join(chunks))
