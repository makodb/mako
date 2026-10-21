#!/usr/bin/env python3
"""merge_latency_csv.py — produce the summary.md for an Akkio etcd experiment.

Reads per-variant directories under ``<RDIR>/log/<variant>/`` and emits a
markdown report with:

  - A rich per-host table for each variant. Latency percentiles come from
    two sources:

      * The ``.res`` file's ``All-efficient-attempts statistics`` line
        covers the mid-10s window's count, p50, p90, p99, and mean. These
        are what deptran itself reports.

      * The per-host ``<label>-<zooN>.csv`` gives the raw per-request
        ``End2End-Latency`` samples (same mid-10s window). From these we
        derive additional percentiles (min, p75, p95, p99.9, max, stddev)
        so the tail is visible without re-running with a different SLO.

    We also log mid-10s throughput (parsed from the ``Mid throughput is``
    line) and core-17 CPU utilisation (parsed from the 1 Hz
    ``<label>-<zooN>-cpustat.txt`` samples).

  - A cross-host aggregate table for each variant: percentiles over the
    union of per-request samples. That matters because averaging
    per-host percentiles is mathematically wrong — the percentile of a
    union is not the mean of component percentiles.

  - A final headline table stacking all variants' cross-host numbers for
    easy comparison.

Usage:
    python3 merge_latency_csv.py <RDIR>            # stdout
    python3 merge_latency_csv.py <RDIR> > out.md   # redirect
"""

from __future__ import annotations

import csv
import math
import re
import statistics
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ZOO_NAMES = ["zoo1", "zoo2", "zoo3", "zoo4", "zoo5"]
VARIANTS_DEFAULT = ["V1-raw", "V2-batch", "V3-lease", "V4-jetpack-etcd"]

# Percentile tuples used in the rich per-host table and the cross-host table.
PCT_HOST: List[Tuple[str, float]] = [
    ("min", 0.0),
    ("p50", 50.0),
    ("p75", 75.0),
    ("p90", 90.0),
    ("p95", 95.0),
    ("p99", 99.0),
    ("p99.9", 99.9),
    ("max", 100.0),
]
PCT_AGG: List[Tuple[str, float]] = [
    ("p50", 50.0),
    ("p75", 75.0),
    ("p90", 90.0),
    ("p95", 95.0),
    ("p99", 99.0),
    ("p99.9", 99.9),
]

STATS_RE = re.compile(
    r"All-efficient-attempts\s+statistics\s+"
    r"count\s+(?P<count>\d+)\s+"
    r"0pct\s+(?P<p0>-?\d+\.?\d*)\s+"
    r"50pct\s+(?P<p50>-?\d+\.?\d*)\s+"
    r"90pct\s+(?P<p90>-?\d+\.?\d*)\s+"
    r"99pct\s+(?P<p99>-?\d+\.?\d*)\s+"
    r"ave\s+(?P<ave>-?\d+\.?\d*)"
)
MID_TPUT_RE = re.compile(r"Mid throughput is\s+(?P<tp>-?\d+\.?\d*)")


# ---------- .res + CPU parsing helpers ----------

def parse_res_stats(res_path: Path) -> Optional[Dict[str, float]]:
    """Return deptran's own mid-10s stats + mid throughput for a .res file."""
    out: Dict[str, float] = {}
    if not res_path.is_file():
        return None
    with res_path.open("r", errors="replace") as f:
        for line in f:
            if not out and "All-efficient-attempts" in line and "statistics" in line:
                m = STATS_RE.search(line)
                if m:
                    out.update({k: float(v) for k, v in m.groupdict().items()})
            elif "Mid throughput is" in line:
                m = MID_TPUT_RE.search(line)
                if m:
                    out["mid_tput"] = float(m.group("tp"))
    return out if out else None


def parse_cpu_core(cpustat_path: Path, core: int) -> Optional[float]:
    """Return avg % busy for a specific core over the capture window, or None."""
    if not cpustat_path.is_file():
        return None
    prev_total = prev_idle = None
    usages: List[float] = []
    core_prefix = f"cpu{core} "
    with cpustat_path.open("r", errors="replace") as f:
        for line in f:
            if not line.startswith(core_prefix):
                continue
            parts = line.split()
            if len(parts) < 8:
                continue
            try:
                vals = list(map(int, parts[1:9]))
            except ValueError:
                continue
            user, nice, system, idle, iowait, irq, softirq, steal = vals
            total = sum(vals)
            idle_time = idle + iowait
            if prev_total is not None:
                dt = total - prev_total
                di = idle_time - prev_idle
                if dt > 0:
                    usages.append(100.0 * (1.0 - di / dt))
            prev_total = total
            prev_idle = idle_time
    if not usages:
        return None
    return sum(usages) / len(usages)


# ---------- CSV latency parsing ----------

def load_csv_e2e(csv_path: Path) -> List[float]:
    out: List[float] = []
    if not csv_path.is_file():
        return out
    with csv_path.open("r", errors="replace") as f:
        reader = csv.DictReader(f)
        for row in reader:
            v = (row.get("End2End-Latency") or "").strip()
            if not v:
                continue
            try:
                x = float(v)
            except ValueError:
                continue
            if x > 0:
                out.append(x)
    return out


def percentile(sorted_vals: List[float], q: float) -> float:
    if not sorted_vals:
        return float("nan")
    if q <= 0:
        return sorted_vals[0]
    if q >= 100:
        return sorted_vals[-1]
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    k = (len(sorted_vals) - 1) * (q / 100.0)
    lo = int(k)
    hi = min(lo + 1, len(sorted_vals) - 1)
    frac = k - lo
    return sorted_vals[lo] * (1 - frac) + sorted_vals[hi] * frac


def stddev_fast(vals: List[float], mean: float) -> float:
    if len(vals) < 2:
        return 0.0
    # Two-pass variance; avoids numerical issues of Welford for large N.
    s = 0.0
    for v in vals:
        d = v - mean
        s += d * d
    return math.sqrt(s / (len(vals) - 1))


# ---------- rendering ----------

def render_row(cells: List[str]) -> str:
    return "| " + " | ".join(cells) + " |"


def fmt(x: float, nd: int = 2) -> str:
    if x != x:  # nan
        return "-"
    if math.isinf(x):
        return "-"
    return f"{x:.{nd}f}"


def find_variants(log_dir: Path) -> List[str]:
    names = [p.name for p in sorted(log_dir.iterdir()) if p.is_dir()]
    if not names:
        return VARIANTS_DEFAULT
    ranked = [v for v in VARIANTS_DEFAULT if v in names]
    tail = [v for v in names if v not in ranked]
    return ranked + sorted(tail)


# ---------- main ----------

def summarize_variant(variant: str, vdir: Path) -> List[Tuple[str, Dict[str, float]]]:
    """Print per-host + cross-host for one variant. Return headline row data."""
    print(f"## {variant}\n")

    # 1) Per-host table (.res mid-10s stats + csv-derived extra percentiles
    #    + mid throughput + core-17 cpu).
    print("### Per-host (mid-10s window)\n")
    header = [
        "host", "samples", "tput (req/s)",
        "min (ms)", "p50", "p75", "p90", "p95", "p99", "p99.9", "max",
        "avg (ms)", "stddev (ms)",
        "core-17 cpu (%)",
    ]
    print(render_row(header))
    print(render_row(["---"] * len(header)))

    for zoo in ZOO_NAMES:
        res_files = list(vdir.glob(f"*-{zoo}.res"))
        csv_files = list(vdir.glob(f"*-{zoo}.csv"))
        cpu_files = list(vdir.glob(f"*-{zoo}-cpustat.txt"))

        res_stats = parse_res_stats(res_files[0]) if res_files else None
        csv_samples = sorted(
            s for f in csv_files for s in load_csv_e2e(f)
        )
        core17 = parse_cpu_core(cpu_files[0], 17) if cpu_files else None

        samples = len(csv_samples)
        if res_stats and res_stats.get("count", 0) > 0:
            count = int(res_stats["count"])
        else:
            count = samples
        tput = res_stats.get("mid_tput") if res_stats else None

        if csv_samples:
            pcts = {lbl: percentile(csv_samples, q) for lbl, q in PCT_HOST}
            mean = sum(csv_samples) / len(csv_samples)
            stddev = stddev_fast(csv_samples, mean)
        elif res_stats and res_stats.get("count", 0) > 0:
            # Fall back to deptran's own percentiles if CSV is absent.
            pcts = {
                "min": res_stats["p0"],
                "p50": res_stats["p50"],
                "p75": float("nan"),
                "p90": res_stats["p90"],
                "p95": float("nan"),
                "p99": res_stats["p99"],
                "p99.9": float("nan"),
                "max": float("nan"),
            }
            mean = res_stats["ave"]
            stddev = float("nan")
        else:
            pcts = {lbl: float("nan") for lbl, _ in PCT_HOST}
            mean = float("nan")
            stddev = float("nan")

        row: List[str] = [
            zoo,
            str(count) if count else "-",
            fmt(tput, 1) if tput is not None else "-",
            fmt(pcts["min"]),
            fmt(pcts["p50"]),
            fmt(pcts["p75"]),
            fmt(pcts["p90"]),
            fmt(pcts["p95"]),
            fmt(pcts["p99"]),
            fmt(pcts["p99.9"]),
            fmt(pcts["max"]),
            fmt(mean),
            fmt(stddev),
            fmt(core17, 1) if core17 is not None else "-",
        ]
        print(render_row(row))
    print()

    # 2) Cross-host aggregate over the union of CSV samples.
    print("### Cross-host aggregate (union of per-request samples)\n")
    all_samples: List[float] = []
    per_host_counts: Dict[str, int] = {}
    for zoo in ZOO_NAMES:
        per_host = 0
        for csv_path in sorted(vdir.glob(f"*-{zoo}.csv")):
            samples = load_csv_e2e(csv_path)
            per_host += len(samples)
            all_samples.extend(samples)
        per_host_counts[zoo] = per_host

    headline_rows: List[Tuple[str, Dict[str, float]]] = []
    if not all_samples:
        print("_no CSV samples available_\n")
    else:
        all_samples.sort()
        mean = sum(all_samples) / len(all_samples)
        stddev = stddev_fast(all_samples, mean)
        header2 = [
            "total samples",
            "min (ms)", "p50", "p75", "p90", "p95", "p99", "p99.9", "max",
            "avg (ms)", "stddev (ms)",
        ]
        print(render_row(header2))
        print(render_row(["---"] * len(header2)))
        row2: List[str] = [
            str(len(all_samples)),
            fmt(all_samples[0]),
            *[fmt(percentile(all_samples, q)) for _, q in [
                ("p50", 50.0), ("p75", 75.0), ("p90", 90.0),
                ("p95", 95.0), ("p99", 99.0), ("p99.9", 99.9),
            ]],
            fmt(all_samples[-1]),
            fmt(mean),
            fmt(stddev),
        ]
        print(render_row(row2))
        print()
        # Per-host sample counts underneath for context.
        print("Per-host sample counts:")
        ph_parts = [f"{zoo}={per_host_counts[zoo]}" for zoo in ZOO_NAMES]
        print("- " + ", ".join(ph_parts))
        print()

        headline_rows.append((variant, {
            "count": float(len(all_samples)),
            "p50": percentile(all_samples, 50),
            "p90": percentile(all_samples, 90),
            "p99": percentile(all_samples, 99),
            "p99.9": percentile(all_samples, 99.9),
            "avg": mean,
        }))
    return headline_rows


def main(argv: List[str]) -> int:
    if len(argv) < 2:
        print("usage: merge_latency_csv.py <RDIR>", file=sys.stderr)
        return 2
    rdir = Path(argv[1]).resolve()
    log_dir = rdir / "log"
    if not log_dir.is_dir():
        print(f"# {rdir.name}\n\nno log/ directory found at {log_dir}")
        return 1

    print(f"# {rdir.name} — latency summary\n")
    print(f"Result directory: `{rdir}`  \n"
          f"Generated by `scripts/merge_latency_csv.py`.\n")
    print("All latency values are milliseconds over the **mid 10 s** window of "
          "each 30 s run. "
          "Per-host percentiles min, p50, p75, p90, p95, p99, p99.9, max, avg, "
          "stddev are computed from the raw `End2End-Latency` column in each "
          "host's `.csv`. "
          "Per-host `tput (req/s)` is the deptran \"Mid throughput is …\" "
          "value (count over the mid 10 s divided by that window length, "
          "already mid-10s-scoped). "
          "`core-17 cpu (%)` is the mean busy-fraction of CPU17 sampled at "
          "1 Hz across the CPU monitor window. "
          "Cross-host numbers are percentiles of the union of all host CSV "
          "samples (so they correctly reflect the distribution across all "
          "100 client sites, not an average of per-host percentiles).\n")

    variants = find_variants(log_dir)
    headline: List[Tuple[str, Dict[str, float]]] = []
    for variant in variants:
        vdir = log_dir / variant
        if not vdir.is_dir():
            continue
        headline.extend(summarize_variant(variant, vdir))

    if headline:
        print("## Headline (cross-host aggregate)\n")
        hdr = ["variant", "samples",
               "p50 (ms)", "p90 (ms)", "p99 (ms)", "p99.9 (ms)", "avg (ms)"]
        print(render_row(hdr))
        print(render_row(["---"] * len(hdr)))
        for variant, stats in headline:
            print(render_row([
                variant,
                f"{int(stats['count'])}",
                fmt(stats["p50"]),
                fmt(stats["p90"]),
                fmt(stats["p99"]),
                fmt(stats["p99.9"]),
                fmt(stats["avg"]),
            ]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
