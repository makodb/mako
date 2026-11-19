#!/usr/bin/env python3
"""
Latency CDF figures for camera-ready folders.

Mirrors the OSDI notebook's cell-14 CDF style (cumulative fraction vs latency,
sample source = CSV column 5 = All-efficient-attempts), but on the camera-ready
folder layout: a 1x5 row (Raft / Copilot / Mencius / MongoDB / etcd; ZooKeeper
dropped) plus a 5-line compare (Raft vanilla, Raft+Jetpack adaptive, CURP,
Swift Paxos, EPaxos), each protocol drawn at one fixed near-saturation
concurrency.

Output (in <folder>/figs/):
  latency_cdf_grid.{pdf,png}
  latency_cdf_raft_curp_swiftpaxos_epaxos.{pdf,png}
  (merged folder additionally:) ..._dc{0..9}_{NAME}.{pdf,png}

Per data point: requires all 10 .csv AND all 10 .res files to exist (cluster
completeness gate). Cluster CDF concatenates samples from 10 hosts; per-DC
CDF uses just that host's samples.
"""

import argparse
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

NHOSTS = 10
DC_NAMES = ["CA", "OR", "MUM", "FF", "STO", "LDN", "HK", "SGP", "IRL", "PAR"]

# Near-saturation, pre-collapse concurrency per protocol root.
# Raft/Copilot/Mencius/MongoDB inherit OSDI's fixed_conc_number choices
# (Raft uses the OSDI hard-coded 150 override). etcd/ZK/CURP/Swift/EPaxos
# are picked from the camera-ready throughput-latency curves.
# mencius default conc swapped 16 → 10 (2026-05-10 user directive: drop
# the c=16 figures entirely; c=10 is the camera-ready setting).
FIXED_CONC = {
    # 2026-05-09: aligned to the workload-axis anchors so CDF and
    # workload-axis figures share concs per protocol. Mencius stays at 16
    # because c=50 is past its saturation point (no clean 10/10 cells).
    "raft":              50,    # was 150 (OSDI near-saturation override)
    "copilot":           50,
    "mencius":           10,
    "mongodb":           50,    # 2025-06-26 dataset (restored 2026-05-10). At
                                 # c=50 every mode is clean: vanilla 286ms,
                                 # 0% 288ms, 100% 172ms, 101% 168ms (.res ave).
    "etcd":              50,
    "zookeeper":        200,
    "curp":             150,
    "swiftpaxos":       200,
    "epaxos_corrected": 200,
}

# (label, color, linestyle) — same palette as the throughput-latency plot
PANEL_CONFIGS = [
    ("vanilla",  "#437c17", "-"),
    ("0%",       "black",   ":"),
    ("adaptive", "#B22222", "--"),
    ("100%",     "orange",  "-."),
]

GRID_PROTOCOLS = [
    ("Raft",      "raft"),
    ("etcd",      "etcd"),
    ("MongoDB",   "mongodb"),
    ("Copilot",   "copilot"),
    ("Mencius",   "mencius"),
]

COMPARE_LINES = [
    # (label, color, linestyle, proto-prefix, mode, FIXED_CONC root)
    ("Raft (vanilla)",            "#437c17", "-",  "none_raft",              0,   "raft"),
    ("Raft + Jetpack (adaptive)", "#B22222", "--", "rule_raft",            101,   "raft"),
    ("CURP",                      "#1f77b4", "-",  "none_curp",            200,   "curp"),
    ("Swift Paxos",               "#ff7f0e", "-",  "none_swiftpaxos",        0,   "swiftpaxos"),
    ("EPaxos",                    "#8c564b", "-",  "none_epaxos_corrected",  0,   "epaxos_corrected"),
]

# Single concurrency used for ALL lines on the compare CDF figure (overrides
# the per-protocol FIXED_CONC table — set 2026-05-07 by user). This makes the
# 5 protocols directly comparable at the same offered load instead of each
# at its own near-saturation point.
COMPARE_FIXED_CONC = 150

X_AXIS_MAX_MS = 1000

# ------------------------------------------------------------------------
# Sample loading
# ------------------------------------------------------------------------
_SAMPLE_CACHE = {}  # (log_dir, proto, conc, mode) -> [arr0..9] or None


def _read_csv_col5(path):
    try:
        df = pd.read_csv(
            path, usecols=[5], header=0, engine="c",
            low_memory=False, dtype=str,
        )
    except (FileNotFoundError, pd.errors.EmptyDataError):
        return None
    if df.empty:
        return np.array([], dtype=float)
    df.columns = ["ae"]
    arr = pd.to_numeric(df["ae"], errors="coerce").dropna().to_numpy(dtype=float)
    return arr


def get_per_host_samples(log_dir, proto, conc, mode):
    """Return list of length 10 (one np.array per host) or None if any of the
    20 csv/res files is missing or any csv fails to load."""
    key = (log_dir, proto, conc, mode)
    if key in _SAMPLE_CACHE:
        return _SAMPLE_CACHE[key]
    arrays = []
    for i in range(NHOSTS):
        prefix = f"{proto}-60c1s5r10p-rw_1000000-concurrent_{conc}-{mode}-YCSB_A-server{i}"
        csv_p = os.path.join(log_dir, prefix + ".csv")
        res_p = os.path.join(log_dir, prefix + ".res")
        if not (os.path.isfile(csv_p) and os.path.isfile(res_p)):
            _SAMPLE_CACHE[key] = None
            return None
        arr = _read_csv_col5(csv_p)
        if arr is None:
            _SAMPLE_CACHE[key] = None
            return None
        arrays.append(arr)
    _SAMPLE_CACHE[key] = arrays
    return arrays


def collect_samples(log_dir, proto, conc, mode, dc=None):
    arrays = get_per_host_samples(log_dir, proto, conc, mode)
    if arrays is None:
        return None
    if dc is None:
        return np.concatenate(arrays) if arrays else np.array([], dtype=float)
    return arrays[dc]


def cdf_xy(samples):
    if samples is None or len(samples) == 0:
        return np.array([]), np.array([])
    s = np.sort(samples)
    n = len(s)
    y = np.arange(n) / n   # 0, 1/n, …, (n-1)/n  (matches OSDI cell 14)
    return s, y


# ------------------------------------------------------------------------
# Plotting
# ------------------------------------------------------------------------
# Per OSDI submission cell 14 (CDF figure): tick label size 18, fonts 22 / 22 / 22 / 18.
plt.rcParams.update({
    "text.usetex": False,
    "font.family": "serif",
    "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
    "mathtext.fontset": "stix",
    "xtick.labelsize": 18,
    "ytick.labelsize": 18,
})

# OSDI cell 14 specifics for the CDF panels.
TITLE_FONT_SIZE  = 22
XLABEL_FONT_SIZE = 22
YLABEL_FONT_SIZE = 22
LEGEND_FONT_SIZE = 18
LINE_WIDTH       = 3   # bumped for CDF readability per cell 14
MARKER_SIZE      = 10
CDF_XLIM         = (-20, 620)
CDF_XTICKS       = [0, 200, 400, 600]


def savefig_all(fig, pdf_path, **kwargs):
    fig.savefig(pdf_path, **kwargs)
    base, _ = os.path.splitext(pdf_path)
    fig.savefig(base + ".png", dpi=200, **kwargs)


# Per-panel line suppressions: (proto_root, panel_label) pairs to NOT draw.
# Mirrors gen_tput_p90_figures.py's SKIP_LINES so any variant suppressed
# in the throughput-latency grid is also suppressed in the CDF panel.
SKIP_LINES = {
    # Cleared 2026-05-09 — mongodb-0% line is clean now (June 2025 c=50 data).
}


def panel_curves(log_dir, proto_root, dc=None):
    """Return list of (label, color, linestyle, x, y) for the 3 variants we
    show on this figure: vanilla, 0%, adaptive. The 100% line is intentionally
    dropped here per the 2026-05-09 rule that only the throughput-latency
    grid shows the 100% line; CDFs and workload-axis grids show only
    {vanilla, 0%, adaptive}.

    Entries matching SKIP_LINES are returned with empty samples (drop the line
    from the panel without disturbing the legend slot)."""
    conc = FIXED_CONC[proto_root]
    sources = [
        ("vanilla",  f"none_{proto_root}", 0),
        ("0%",       f"rule_{proto_root}", 0),
        ("adaptive", f"rule_{proto_root}", 101),
    ]
    cfg = {c[0]: c for c in PANEL_CONFIGS}
    out = []
    for label, proto, mode in sources:
        if (proto_root, label) in SKIP_LINES:
            out.append((label, cfg[label][1], cfg[label][2], np.array([]), np.array([])))
            continue
        samples = collect_samples(log_dir, proto, conc, mode, dc=dc)
        x, y = cdf_xy(samples)
        out.append((label, cfg[label][1], cfg[label][2], x, y))
    return out, conc


def draw_grid(log_dir, out_path, dc=None, dc_label=None, protocols=None):
    """OSDI cell 14 layout: 1xN figsize=(6N, 3), shared y, x-axis 0-600 ms with
    ticks at [0, 200, 400, 600], inner legend on first panel at lower-right,
    y-tick labels hidden on panels 2..N (shared scaling), wspace=0.05."""
    if protocols is None:
        protocols = GRID_PROTOCOLS
    n = len(protocols)
    fig, axes = plt.subplots(1, n, figsize=(6 * n, 3), sharey=True)
    if n == 1:
        axes = [axes]
    else:
        axes = axes.flatten()
    counts = []
    for ax, (title, root) in zip(axes, protocols):
        curves, conc = panel_curves(log_dir, root, dc=dc)
        n_lines = 0
        for label, color, ls, x, y in curves:
            if len(x) == 0:
                continue
            ax.plot(x, y, label=label, color=color, linestyle=ls, lw=LINE_WIDTH)
            n_lines += 1
        ax.set_title(f"{title} (c={conc})", fontsize=TITLE_FONT_SIZE)
        ax.set_xlabel("Latency (ms)", fontsize=XLABEL_FONT_SIZE)
        ax.grid(True, linestyle="--", alpha=0.5)
        ax.set_xlim(*CDF_XLIM)
        ax.set_xticks(CDF_XTICKS)
        ax.set_ylim(0, 1.0)
        counts.append((title, conc, n_lines))

    # OSDI cell 14: y-label only on the leftmost panel; hide y-tick labels on
    # axes[1:] but keep their scaling.
    axes[0].set_ylabel("Cumulative Fraction", fontsize=YLABEL_FONT_SIZE)
    for ax in axes[1:]:
        ax.tick_params(labelleft=False)

    # OSDI cell 14: inner legend on first panel at lower-right.
    if axes[0].get_legend_handles_labels()[0]:
        axes[0].legend(loc="lower right", fontsize=LEGEND_FONT_SIZE)
    if dc_label:
        fig.suptitle(dc_label, fontsize=20, y=1.03)
    fig.subplots_adjust(wspace=0.05)

    savefig_all(fig, out_path, bbox_inches="tight")
    plt.close(fig)
    return counts


def draw_compare(log_dir, out_path, dc=None, dc_label=None, conc=None):
    fig, ax = plt.subplots(figsize=(10, 6.5))
    counts = []
    if conc is None:
        conc = COMPARE_FIXED_CONC  # all 5 protocols at the same conc (set 2026-05-07)
    for label, color, ls, proto, mode, _root in COMPARE_LINES:
        samples = collect_samples(log_dir, proto, conc, mode, dc=dc)
        x, y = cdf_xy(samples)
        counts.append((label, conc, len(x)))
        if len(x) == 0:
            continue
        ax.plot(x, y, label=label, color=color, linestyle=ls, lw=2.5)
    ax.set_xlabel("Latency (ms)", fontsize=20)
    ax.set_ylabel("Cumulative Fraction", fontsize=20)
    ax.grid(True, linestyle="--", alpha=0.5)
    ax.set_xlim(0, X_AXIS_MAX_MS)
    ax.set_ylim(0, 1.0)
    title_bits = []
    if dc_label:
        title_bits.append(dc_label)
    title_bits.append(f"c={conc}")
    ax.set_title(" — ".join(title_bits), fontsize=18)
    ax.legend(fontsize=12, loc="lower right", framealpha=0.95)
    fig.tight_layout()
    savefig_all(fig, out_path, bbox_inches="tight")
    plt.close(fig)
    return counts


# ------------------------------------------------------------------------
# Driver
# ------------------------------------------------------------------------
PER_DC_FOLDERS = {"2026-05-05-merged-cameraready"}
DEFAULT_FOLDERS = [
    "2026-04-30-camera-ready-exp0-small.usable",
    "2026-05-05-merged-cameraready",
]
RESULTS_ROOT = "/home/users/ztang/janus/results"


def run_one(result_dir):
    """Generate latency-CDF figures."""
    log_dir = os.path.join(result_dir, "log")
    figs_dir = os.path.join(result_dir, "figs")
    os.makedirs(figs_dir, exist_ok=True)
    if not os.path.isdir(log_dir):
        print(f"[SKIP] {result_dir}: no log/ dir")
        return
    folder_name = os.path.basename(result_dir.rstrip("/"))
    print(f"\n=== {folder_name} ===")

    # Cluster — emit both with all 5 protocols and a 4-proto variant without etcd
    # (set 2026-05-09 for the writeup's no-etcd alternative).
    for fname_suffix, protocols in (
        ("",        GRID_PROTOCOLS),
        ("_noetcd", [p for p in GRID_PROTOCOLS if p[1] != "etcd"]),
    ):
        grid_path = os.path.join(figs_dir, f"latency_cdf_grid{fname_suffix}.pdf")
        grid_counts = draw_grid(log_dir, grid_path, protocols=protocols)
        print(f"  [GRID{fname_suffix}] {grid_path}")
        for title, conc, n in grid_counts:
            print(f"    {title:<10} c={conc:<4}  -> {n} variants drawn")

    # Default conc 150 keeps the existing filename. Additional concs emit
    # `latency_cdf_raft_curp_swiftpaxos_epaxos_c<N>.{pdf,png}` for cross-conc
    # comparison. Only concs where ALL 5 protocols have clean 10/10 data
    # should appear here (verify before adding new entries).
    COMPARE_CONCS = [
        (None, ""),    # default = COMPARE_FIXED_CONC=150, no conc suffix in filename
        (1,    "_c1"),
        (100,  "_c100"),
    ]
    for conc, suffix in COMPARE_CONCS:
        cmp_path = os.path.join(
            figs_dir, f"latency_cdf_raft_curp_swiftpaxos_epaxos{suffix}.pdf"
        )
        cmp_counts = draw_compare(log_dir, cmp_path, conc=conc)
        print(f"  [CMP{suffix}]  {cmp_path}")
        for label, c_used, npts in cmp_counts:
            print(f"    {label:<32} c={c_used:<4}  -> {npts} samples")

    if folder_name in PER_DC_FOLDERS:
        for dc, name in enumerate(DC_NAMES):
            tag = f"dc{dc}_{name}"
            grid_p = os.path.join(figs_dir, f"latency_cdf_grid_{tag}.pdf")
            draw_grid(log_dir, grid_p, dc=dc, dc_label=f"DC{dc} ({name}) view")
            cmp_p = os.path.join(figs_dir, f"latency_cdf_raft_curp_swiftpaxos_epaxos_{tag}.pdf")
            draw_compare(log_dir, cmp_p, dc=dc, dc_label=f"DC{dc} ({name}) view")
            print(f"  [DC{dc:>2}] {tag}: grid + cmp written")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--result-dir", action="append",
                    help="Absolute or basename path under results/. Repeatable.")
    args = ap.parse_args()
    folders = args.result_dir or DEFAULT_FOLDERS
    for f in folders:
        path = f if os.path.isabs(f) else os.path.join(RESULTS_ROOT, f)
        run_one(path)


if __name__ == "__main__":
    main()
