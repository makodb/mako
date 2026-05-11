#!/usr/bin/env python3
"""Plot the thesis no-disk scalability figure from frozen CSV snapshots."""

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


BATCH_SIZE = 400.0
RUN_SECONDS = 30.0

BACKENDS = {
    "single_raft_no_pool": {
        "label": "Single-Raft, no ReplayPool",
        "color": "#D55E00",
        "marker": "X",
    },
    "single_raft_1to1_replay_pool": {
        "label": "Single-Raft + ReplayPool",
        "color": "#0072B2",
        "marker": "o",
    },
    "multi_raft": {
        "label": "Multi-Raft",
        "color": "#009E73",
        "marker": "s",
    },
    "paxos": {
        "label": "Paxos",
        "color": "#CC79A7",
        "marker": "^",
    },
}


def configure_style():
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.serif": ["DejaVu Serif", "Times New Roman", "Times"],
            "font.size": 10,
            "axes.titlesize": 11,
            "axes.labelsize": 10,
            "legend.fontsize": 8,
            "xtick.labelsize": 9,
            "ytick.labelsize": 9,
            "axes.spines.top": False,
            "axes.spines.right": False,
            "axes.grid": True,
            "grid.alpha": 0.25,
            "grid.linewidth": 0.7,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
        }
    )


def num(row, key, default=0.0):
    value = (row.get(key) or "").strip()
    if value == "" or value == "N/A":
        return default
    try:
        return float(value)
    except ValueError:
        return default


def load_rows(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    rows.sort(key=lambda row: int(num(row, "threads")))
    return rows


def load_all(snapshot_dir):
    return {
        key: load_rows(snapshot_dir / f"{key}.csv")
        for key in BACKENDS
    }


def kops(value, _pos):
    return f"{value / 1000:.0f}"


def replay_throughput(row):
    return num(row, "replay_batch_p1") * BATCH_SIZE / RUN_SECONDS


def series(rows, metric):
    xs = [int(num(row, "threads")) for row in rows]
    if metric == "leader":
        ys = [num(row, "throughput_ops_sec") for row in rows]
    elif metric == "replay":
        ys = [replay_throughput(row) for row in rows]
    elif metric == "coverage":
        ys = []
        for row in rows:
            leader = num(row, "throughput_ops_sec")
            ys.append(replay_throughput(row) / leader if leader > 0 else 0.0)
    else:
        raise ValueError(f"unknown metric: {metric}")
    return xs, ys


def line(ax, xs, ys, style):
    ax.plot(
        xs,
        ys,
        color=style["color"],
        marker=style["marker"],
        linewidth=2.0,
        markersize=4.8,
        label=style["label"],
    )


def save(fig, outdir, stem):
    outdir.mkdir(parents=True, exist_ok=True)
    png = outdir / f"{stem}.png"
    pdf = outdir / f"{stem}.pdf"
    fig.savefig(png, dpi=220, bbox_inches="tight")
    fig.savefig(pdf, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {png}")
    print(f"wrote {pdf}")


def plot(data, outdir):
    fig, axes = plt.subplots(1, 3, figsize=(13.3, 3.9))
    panels = [
        (
            "leader",
            "(a) Leader-side committed throughput",
            "Throughput (K transactions/s)",
            FuncFormatter(kops),
            tuple(BACKENDS),
        ),
        (
            "replay",
            "(b) Follower replay without Single-Raft ReplayPool",
            "Replay throughput (K transactions/s)",
            FuncFormatter(kops),
            ("single_raft_no_pool", "multi_raft", "paxos"),
        ),
        (
            "replay",
            "(c) Follower replay with ReplayPool",
            "Replay throughput (K transactions/s)",
            FuncFormatter(kops),
            ("single_raft_1to1_replay_pool", "multi_raft", "paxos"),
        ),
    ]

    for ax, (metric, title, ylabel, formatter, backend_keys) in zip(axes, panels):
        for key in backend_keys:
            style = BACKENDS[key]
            xs, ys = series(data[key], metric)
            line(ax, xs, ys, style)
        ax.set_title(title)
        ax.set_xlabel("Mako worker threads")
        ax.set_ylabel(ylabel)
        ax.yaxis.set_major_formatter(formatter)
        ax.set_xticks([int(num(row, "threads")) for row in data["single_raft_1to1_replay_pool"]])
        ax.set_ylim(bottom=0)
        if "Follower replay" in title:
            ax.set_ylim(0, 540_000)

    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        frameon=False,
        loc="lower center",
        ncol=4,
        bbox_to_anchor=(0.5, -0.015),
    )
    fig.suptitle("No-disk scalability and follower replay progress", fontweight="bold", y=1.04)
    fig.tight_layout(rect=(0, 0.13, 1, 1))
    save(fig, outdir, "fig07_no_disk_scalability")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--snapshot-dir",
        default="doc/thesis/latex/data/snapshot/01_no_disk_four_way",
        help="Directory containing the four no-disk CSV snapshots.",
    )
    parser.add_argument(
        "--outdir",
        default="doc/thesis/latex/figures/graphs",
        help="Directory for the generated PNG and PDF.",
    )
    args = parser.parse_args()

    configure_style()
    data = load_all(Path(args.snapshot_dir))
    plot(data, Path(args.outdir))


if __name__ == "__main__":
    main()
