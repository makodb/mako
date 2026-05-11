#!/usr/bin/env python3
"""Plot full ReplayPool sensitivity matrix results."""

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


COLORS = {
    0: "#D55E00",
    1: "#E69F00",
    2: "#56B4E9",
    4: "#0072B2",
    8: "#009E73",
    11: "#CC79A7",
}
MARKERS = {0: "X", 1: "o", 2: "s", 4: "^", 8: "D", 11: "v"}
BATCH_SIZE = 400.0


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


def load(path):
    rows = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            throughput = num(row, "throughput_ops_sec")
            replay_progress = num(row, "replay_batches_per_sec") * BATCH_SIZE
            rows.append(
                {
                    "worker_threads": int(num(row, "worker_threads")),
                    "replay_threads": int(num(row, "replay_threads")),
                    "throughput": throughput,
                    "replay_progress": replay_progress,
                    "replay_ratio": replay_progress / throughput if throughput > 0 else 0.0,
                    "role_apply_peak": num(row, "role_apply_peak"),
                    "role_replay_mean": num(row, "role_replay_mean"),
                }
            )
    rows.sort(key=lambda r: (r["replay_threads"], r["worker_threads"]))
    return rows


def kops(value, _pos):
    return f"{value / 1000:.0f}"


def pct(value, _pos):
    return f"{value * 100:.0f}%"


def save(fig, outdir, stem):
    outdir.mkdir(parents=True, exist_ok=True)
    png = outdir / f"{stem}.png"
    pdf = outdir / f"{stem}.pdf"
    fig.savefig(png, dpi=220, bbox_inches="tight")
    fig.savefig(pdf, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {png}")
    print(f"wrote {pdf}")


def shared_legend(fig, ax, ncol, y=0.0):
    handles, labels = ax.get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        frameon=False,
        loc="lower center",
        ncol=ncol,
        bbox_to_anchor=(0.5, y),
    )


def values_for(rows, replay_threads, key):
    selected = [r for r in rows if r["replay_threads"] == replay_threads]
    selected.sort(key=lambda r: r["worker_threads"])
    return [r["worker_threads"] for r in selected], [r[key] for r in selected]


def plot_lines(rows, outdir):
    replay_sizes = sorted({r["replay_threads"] for r in rows})
    fig, axes = plt.subplots(1, 3, figsize=(13.2, 3.8))
    specs = [
        ("throughput", "(a) Leader-side throughput", "Throughput (K txns/s)", FuncFormatter(kops)),
        ("replay_progress", "(b) Follower replay progress", "Throughput-equivalent (K txns/s)", FuncFormatter(kops)),
        ("replay_ratio", "(c) Replay progress / leader throughput", "Replay completeness", FuncFormatter(pct)),
    ]
    for ax, (key, title, ylabel, formatter) in zip(axes, specs):
        for replay_threads in replay_sizes:
            xs, ys = values_for(rows, replay_threads, key)
            ax.plot(
                xs,
                ys,
                color=COLORS.get(replay_threads, "#333333"),
                marker=MARKERS.get(replay_threads, "o"),
                linewidth=1.9,
                markersize=4.8,
                label=f"{replay_threads} replay threads",
            )
        if key == "replay_ratio":
            ax.axhline(1.0, color="#555555", linestyle="--", linewidth=1.0)
        ax.set_title(title)
        ax.set_xlabel("Mako worker threads")
        ax.set_ylabel(ylabel)
        ax.yaxis.set_major_formatter(formatter)
        ax.set_xticks(sorted({r["worker_threads"] for r in rows}))
        ax.set_ylim(bottom=0)
    shared_legend(fig, axes[0], ncol=3, y=-0.02)
    fig.suptitle("ReplayPool sensitivity across worker counts", fontweight="bold", y=1.04)
    fig.tight_layout(rect=(0, 0.13, 1, 1))
    save(fig, outdir, "fig08_replay_pool_matrix")


def plot_heatmap(rows, outdir):
    workers = sorted({r["worker_threads"] for r in rows})
    replay_sizes = sorted({r["replay_threads"] for r in rows})
    lookup = {(r["worker_threads"], r["replay_threads"]): r for r in rows}
    matrix = []
    for replay_threads in replay_sizes:
        matrix.append([lookup[(worker, replay_threads)]["replay_ratio"] for worker in workers])

    fig, ax = plt.subplots(figsize=(7.4, 4.6))
    im = ax.imshow(matrix, aspect="auto", origin="lower", vmin=0.0, vmax=1.0, cmap="viridis")
    ax.set_xticks(range(len(workers)))
    ax.set_xticklabels(workers)
    ax.set_yticks(range(len(replay_sizes)))
    ax.set_yticklabels(replay_sizes)
    ax.set_xlabel("Mako worker threads")
    ax.set_ylabel("ReplayPool threads")
    ax.set_title("Follower replay completeness")
    cbar = fig.colorbar(im, ax=ax)
    cbar.set_label("Replay progress / leader throughput")
    cbar.ax.yaxis.set_major_formatter(FuncFormatter(pct))
    for y, replay_threads in enumerate(replay_sizes):
        for x, worker in enumerate(workers):
            value = lookup[(worker, replay_threads)]["replay_ratio"]
            color = "white" if value < 0.55 else "black"
            ax.text(x, y, f"{value * 100:.0f}", ha="center", va="center", color=color, fontsize=7)
    fig.tight_layout()
    save(fig, outdir, "fig08_replay_pool_matrix_heatmap")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", required=True)
    parser.add_argument("--outdir", default="doc/thesis/figures/graphs")
    args = parser.parse_args()

    configure_style()
    rows = load(Path(args.csv))
    outdir = Path(args.outdir)
    plot_lines(rows, outdir)
    plot_heatmap(rows, outdir)


if __name__ == "__main__":
    main()
