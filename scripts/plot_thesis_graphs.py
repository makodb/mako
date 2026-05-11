#!/usr/bin/env python3
"""Generate thesis-ready evaluation graphs from organized result CSVs."""

import argparse
import csv
import math
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


BACKENDS = {
    "single_pool": {
        "label": "Single-Raft + ReplayPool",
        "short": "Single-Raft",
        "color": "#0072B2",
        "marker": "o",
    },
    "single_no_pool": {
        "label": "Single-Raft, no ReplayPool",
        "short": "No ReplayPool",
        "color": "#D55E00",
        "marker": "X",
    },
    "multi_raft": {
        "label": "Multi-Raft",
        "short": "Multi-Raft",
        "color": "#009E73",
        "marker": "s",
    },
    "paxos": {
        "label": "Paxos",
        "short": "Paxos",
        "color": "#CC79A7",
        "marker": "^",
    },
}

DISKS = {
    "no_disk": {"label": "No disk", "color": "#4D4D4D", "marker": "o"},
    "nvme": {"label": "NVMe model", "color": "#009E73", "marker": "s"},
    "cloudssd": {"label": "Cloud-SSD model", "color": "#D55E00", "marker": "^"},
}

DURATION_SECONDS = 30.0


def configure_style():
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.serif": ["DejaVu Serif", "Times New Roman", "Times"],
            "font.size": 10,
            "axes.titlesize": 11,
            "axes.labelsize": 10,
            "legend.fontsize": 9,
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


def parse_float(value, default=0.0):
    if value is None:
        return default
    value = str(value).strip()
    if value == "" or value == "N/A":
        return default
    try:
        return float(value)
    except ValueError:
        return default


def load_csv(path):
    rows = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            rows.append(row)
    return rows


def load_by_threads(path):
    rows = {}
    for row in load_csv(path):
        threads = int(parse_float(row.get("threads")))
        rows[threads] = row
    return [rows[t] for t in sorted(rows)]


def col(rows, name, scale=1.0):
    return [parse_float(row.get(name)) * scale for row in rows]


def threads(rows):
    return [int(parse_float(row.get("threads"))) for row in rows]


def kops_formatter(value, _pos):
    return f"{value / 1000:.0f}"


def mops_formatter(value, _pos):
    return f"{value / 1_000_000:.1f}"


def gb_formatter(value, _pos):
    return f"{value / 1_000_000_000:.1f}"


def pct_formatter(value, _pos):
    return f"{value:.0f}%"


def save(fig, outdir, stem):
    png = outdir / f"{stem}.png"
    pdf = outdir / f"{stem}.pdf"
    fig.savefig(png, dpi=220, bbox_inches="tight")
    fig.savefig(pdf, bbox_inches="tight")
    plt.close(fig)
    return png, pdf


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


def line(ax, xs, ys, key, label=None, color=None, marker=None, linestyle="-"):
    style = BACKENDS.get(key, DISKS.get(key, {}))
    ax.plot(
        xs,
        ys,
        label=label or style.get("label", key),
        color=color or style.get("color", "#333333"),
        marker=marker or style.get("marker", "o"),
        linewidth=2.1,
        markersize=5.5,
        linestyle=linestyle,
    )


def annotate_last(ax, xs, ys, text, color):
    if not xs or not ys:
        return
    ax.annotate(
        text,
        xy=(xs[-1], ys[-1]),
        xytext=(6, 0),
        textcoords="offset points",
        ha="left",
        va="center",
        color=color,
        fontsize=8.5,
    )


def plot_no_disk_scalability(data, outdir):
    fig, axes = plt.subplots(1, 2, figsize=(10.8, 4.2), sharey=True)
    for ax, metric, title in [
        (axes[0], "throughput_ops_sec", "Leader-side reported throughput"),
        (axes[1], "replay_batch_p1", "Follower replay progress"),
    ]:
        for key in ("single_no_pool", "single_pool", "multi_raft", "paxos"):
            rows = data[key]
            xs = threads(rows)
            if metric == "replay_batch_p1":
                ys = [parse_float(row.get(metric)) * 400.0 / DURATION_SECONDS for row in rows]
            else:
                ys = col(rows, metric)
            line(ax, xs, ys, key)
        ax.set_xlabel("Mako worker threads")
        ax.set_title(title)
        ax.yaxis.set_major_formatter(FuncFormatter(kops_formatter))
        ax.set_xticks(threads(data["single_pool"]))
        ax.set_ylim(bottom=0)
    axes[0].set_ylabel("Throughput (K txns/s)")
    shared_legend(fig, axes[0], ncol=4, y=-0.01)
    fig.suptitle("No-disk scalability and replay progress", fontweight="bold", y=1.03)
    fig.tight_layout(rect=(0, 0.10, 1, 1))
    return save(fig, outdir, "fig06_no_disk_scalability")


def plot_worker_cpu(data, outdir):
    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    for key in ("single_no_pool", "single_pool", "multi_raft", "paxos"):
        rows = data[key]
        xs = threads(rows)
        ys = col(rows, "role_worker_mean")
        line(ax, xs, ys, key)
    ax.axhline(100, color="#777777", linewidth=1.0, linestyle="--")
    ax.text(1.05, 103, "one full core per worker", color="#555555", fontsize=8.5)
    ax.set_xlabel("Mako worker threads")
    ax.set_ylabel("Mean worker CPU per active worker")
    ax.set_title("Mako worker CPU utilization")
    ax.yaxis.set_major_formatter(FuncFormatter(pct_formatter))
    ax.set_xticks(threads(data["single_pool"]))
    ax.set_ylim(bottom=0)
    shared_legend(fig, ax, ncol=4, y=-0.02)
    fig.tight_layout(rect=(0, 0.13, 1, 1))
    return save(fig, outdir, "fig07_worker_cpu_utilization")


def load_replay_raw(summary_rows):
    raw = {}
    for row in summary_rows:
        replay_threads = int(parse_float(row.get("replay_threads")))
        sweep_dir = Path(row.get("sweep_dir", ""))
        csv_path = sweep_dir / "results.csv"
        if csv_path.exists():
            raw_rows = load_by_threads(csv_path)
            raw[replay_threads] = raw_rows[-1] if raw_rows else {}
    return raw


def plot_replay_pool_sensitivity(summary_rows, outdir):
    raw = load_replay_raw(summary_rows)
    xs = [int(parse_float(row.get("replay_threads"))) for row in summary_rows]
    throughput = [parse_float(row.get("throughput_ops_sec")) for row in summary_rows]
    batches = [parse_float(row.get("batches_per_sec")) for row in summary_rows]
    replay_cpu = [parse_float(raw.get(x, {}).get("role_replay_mean")) for x in xs]
    apply_cpu = [parse_float(raw.get(x, {}).get("role_apply_peak")) for x in xs]

    fig, axes = plt.subplots(1, 3, figsize=(12.2, 3.7))
    ax = axes[0]
    ax.plot(xs, throughput, color=BACKENDS["single_pool"]["color"], marker="o", linewidth=2.1)
    ax.set_xlabel("ReplayPool threads")
    ax.set_ylabel("Throughput (K txns/s)")
    ax.set_title("Throughput at t=11")
    ax.yaxis.set_major_formatter(FuncFormatter(kops_formatter))
    ax.set_xticks(xs)
    ax.set_ylim(bottom=0)

    ax = axes[1]
    ax.plot(xs, batches, color="#009E73", marker="s", linewidth=2.1)
    ax.set_xlabel("ReplayPool threads")
    ax.set_ylabel("Replay batches/s")
    ax.set_title("Follower replay progress")
    ax.set_xticks(xs)
    ax.set_ylim(bottom=0)

    ax = axes[2]
    ax.plot(xs, replay_cpu, color="#CC79A7", marker="^", linewidth=2.0, label="Replay CPU")
    ax.plot(xs, apply_cpu, color="#D55E00", marker="X", linewidth=2.0, label="Apply peak CPU")
    ax.set_xlabel("ReplayPool threads")
    ax.set_ylabel("CPU (%)")
    ax.set_title("CPU placement")
    ax.set_xticks(xs)
    ax.set_ylim(bottom=0)
    shared_legend(fig, ax, ncol=2, y=-0.02)

    fig.suptitle("ReplayPool sensitivity", fontweight="bold", y=1.02)
    fig.tight_layout(rect=(0, 0.12, 1, 1))
    return save(fig, outdir, "fig08_replay_pool_sensitivity")


def plot_disk_throughput(disk_data, outdir):
    fig, axes = plt.subplots(1, 3, figsize=(10.8, 3.7), sharey=True)
    backend_order = [
        ("single_raft", "(a) Single-Raft + ReplayPool"),
        ("multi_raft", "(b) Multi-Raft"),
        ("paxos", "(c) Paxos"),
    ]
    for ax, (backend_key, title) in zip(axes, backend_order):
        for disk_key in ("no_disk", "nvme", "cloudssd"):
            rows = disk_data[disk_key][backend_key]
            xs = threads(rows)
            ys = col(rows, "throughput_ops_sec")
            line(ax, xs, ys, disk_key)
        ax.set_title(title)
        ax.set_xlabel("Worker threads")
        ax.set_xticks(threads(disk_data["no_disk"][backend_key]))
        ax.set_ylim(bottom=0)
        ax.yaxis.set_major_formatter(FuncFormatter(kops_formatter))
    axes[0].set_ylabel("Throughput (K txns/s)")
    shared_legend(fig, axes[0], ncol=3, y=-0.02)
    fig.suptitle("Throughput under simulated persistence", fontweight="bold", y=1.03)
    fig.tight_layout(rect=(0, 0.12, 1, 1))
    return save(fig, outdir, "fig09_disk_persistence_throughput")


def plot_fakedisk_work(disk_data, outdir):
    fig, axes = plt.subplots(1, 3, figsize=(12.6, 3.8))
    backend_order = [("single_raft", "single_pool"), ("multi_raft", "multi_raft"), ("paxos", "paxos")]
    right_panel_values = []
    for backend_key, style_key in backend_order:
        rows = disk_data["cloudssd"][backend_key]
        xs = threads(rows)
        throughput = col(rows, "throughput_ops_sec")
        bytes_y = col(rows, "fake_cluster_total_bytes")
        bytes_per_txn = []
        for row in rows:
            committed = parse_float(row.get("throughput_ops_sec")) * DURATION_SECONDS
            total_bytes = parse_float(row.get("fake_cluster_total_bytes"))
            bytes_per_txn.append(total_bytes / committed if committed > 0 else 0.0)
        right_panel_values.extend(bytes_per_txn)
        line(axes[0], xs, throughput, style_key, label=BACKENDS[style_key]["short"])
        line(axes[1], xs, bytes_y, style_key, label=BACKENDS[style_key]["short"])
        line(axes[2], xs, bytes_per_txn, style_key, label=BACKENDS[style_key]["short"])
    axes[0].set_xlabel("Worker threads")
    axes[0].set_ylabel("Throughput (K txns/s)")
    axes[0].set_title("(a) Cloud-SSD throughput")
    axes[0].yaxis.set_major_formatter(FuncFormatter(kops_formatter))
    axes[0].set_xticks(threads(disk_data["cloudssd"]["single_raft"]))
    axes[0].set_ylim(bottom=0)

    axes[1].set_xlabel("Worker threads")
    axes[1].set_ylabel("Cluster simulated-disk bytes (GB)")
    axes[1].set_title("(b) Measured persisted bytes")
    axes[1].yaxis.set_major_formatter(FuncFormatter(gb_formatter))
    axes[1].set_xticks(threads(disk_data["cloudssd"]["single_raft"]))
    axes[1].set_ylim(bottom=0)

    axes[2].set_xlabel("Worker threads")
    axes[2].set_ylabel("Simulated-disk bytes per committed txn")
    axes[2].set_title("(c) Normalized storage work")
    axes[2].set_xticks(threads(disk_data["cloudssd"]["single_raft"]))
    if right_panel_values:
        low = min(right_panel_values)
        high = max(right_panel_values)
        pad = max((high - low) * 1.2, 8.0)
        axes[2].set_ylim(low - pad, high + pad)
    shared_legend(fig, axes[0], ncol=3, y=-0.02)
    fig.suptitle("Cloud-SSD flattening and simulated-disk proof", fontweight="bold", y=1.03)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    return save(fig, outdir, "fig10_fakedisk_proof_cloudssd")


def plot_bytes_per_txn(disk_data, outdir):
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    backend_order = [("single_raft", "single_pool"), ("multi_raft", "multi_raft"), ("paxos", "paxos")]
    all_values = []
    for backend_key, style_key in backend_order:
        rows = disk_data["cloudssd"][backend_key]
        xs = threads(rows)
        bytes_per_txn = []
        for row in rows:
            committed = parse_float(row.get("throughput_ops_sec")) * DURATION_SECONDS
            total_bytes = parse_float(row.get("fake_cluster_total_bytes"))
            bytes_per_txn.append(total_bytes / committed if committed > 0 else 0.0)
        all_values.extend(bytes_per_txn)
        line(ax, xs, bytes_per_txn, style_key, label=BACKENDS[style_key]["short"])
    ax.set_xlabel("Worker threads")
    ax.set_ylabel("Simulated-disk bytes per committed txn")
    ax.set_title("Normalized persistence work, Cloud-SSD model")
    ax.set_xticks(threads(disk_data["cloudssd"]["single_raft"]))
    if all_values:
        low = min(all_values)
        high = max(all_values)
        pad = max((high - low) * 1.2, 8.0)
        ax.set_ylim(low - pad, high + pad)
    shared_legend(fig, ax, ncol=3, y=-0.02)
    fig.tight_layout(rect=(0, 0.12, 1, 1))
    return save(fig, outdir, "fig11_cloudssd_bytes_per_txn")


def mean_stdev(rows, column):
    values = [parse_float(row.get(column)) for row in rows]
    mean = statistics.mean(values) if values else 0.0
    stdev = statistics.stdev(values) if len(values) > 1 else 0.0
    cv = stdev / mean * 100.0 if mean else 0.0
    return mean, stdev, cv


def plot_variance(results_root, outdir):
    entries = [
        ("Single-Raft", results_root / "04_variance/raw_runs/raw/single_raft/scalability_20260506_005116/results.csv", BACKENDS["single_pool"]["color"]),
        ("Multi-Raft", results_root / "04_variance/raw_runs/raw/multi_raft/scalability_20260506_005527/results.csv", BACKENDS["multi_raft"]["color"]),
        ("Paxos", results_root / "04_variance/raw_runs/raw/paxos/scalability_20260506_005942/results.csv", BACKENDS["paxos"]["color"]),
    ]
    labels = []
    means = []
    stdevs = []
    cvs = []
    colors = []
    for label, path, color in entries:
        rows = load_csv(path)
        mean, stdev, cv = mean_stdev(rows, "throughput_ops_sec")
        labels.append(label)
        means.append(mean)
        stdevs.append(stdev)
        cvs.append(cv)
        colors.append(color)

    fig, ax = plt.subplots(figsize=(6.7, 4.0))
    xs = list(range(len(labels)))
    ax.bar(xs, means, yerr=stdevs, color=colors, edgecolor="#222222", linewidth=0.7, capsize=4)
    ax.set_xticks(xs)
    ax.set_xticklabels(labels)
    ax.set_ylabel("Throughput (K txns/s)")
    ax.set_title("t=11 headline reruns")
    ax.yaxis.set_major_formatter(FuncFormatter(kops_formatter))
    ax.set_ylim(bottom=0)
    for x, mean, cv in zip(xs, means, cvs):
        ax.text(x, mean + max(stdevs + [1]) * 2.0, f"CV {cv:.2f}%", ha="center", va="bottom", fontsize=8.5)
    fig.tight_layout()
    saved = save(fig, outdir, "fig12_headline_variance_t11")

    table_path = outdir / "headline_variance_t11.csv"
    with open(table_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["backend", "mean_throughput_ops_sec", "stdev", "cv_pct", "runs"])
        for label, mean, stdev, cv in zip(labels, means, stdevs, cvs):
            writer.writerow([label, f"{mean:.1f}", f"{stdev:.1f}", f"{cv:.3f}", 3])
    return saved + (table_path,)


def load_all(results_root):
    no_disk = {
        "single_no_pool": load_by_threads(results_root / "01_no_disk_four_way/single_raft_no_pool.csv"),
        "single_pool": load_by_threads(results_root / "01_no_disk_four_way/single_raft_1to1_replay_pool.csv"),
        "multi_raft": load_by_threads(results_root / "01_no_disk_four_way/multi_raft.csv"),
        "paxos": load_by_threads(results_root / "01_no_disk_four_way/paxos.csv"),
    }
    replay_summary = load_csv(results_root / "02_replay_pool_sensitivity/summary.csv")
    disk_data = {}
    for disk_key in ("no_disk", "nvme", "cloudssd"):
        disk_data[disk_key] = {
            "single_raft": load_by_threads(results_root / f"03_disk_persistence/{disk_key}/single_raft/results.csv"),
            "multi_raft": load_by_threads(results_root / f"03_disk_persistence/{disk_key}/multi_raft/results.csv"),
            "paxos": load_by_threads(results_root / f"03_disk_persistence/{disk_key}/paxos/results.csv"),
        }
    return no_disk, replay_summary, disk_data


def write_index(outdir, generated):
    index = outdir / "README.md"
    with open(index, "w") as f:
        f.write("# Thesis Evaluation Graphs\n\n")
        f.write("Generated from `results/thesis_results/` by `scripts/plot_thesis_graphs.py`.\n\n")
        f.write("Each figure is emitted as both PNG and PDF. Use PDF for LaTeX when possible.\n\n")
        for path in generated:
            if isinstance(path, Path):
                f.write(f"- `{path.name}`\n")
            else:
                for item in path:
                    f.write(f"- `{Path(item).name}`\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--results-root", default="results/thesis_results")
    parser.add_argument("--outdir", default="doc/thesis/figures/graphs")
    args = parser.parse_args()

    configure_style()
    results_root = Path(args.results_root)
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    no_disk, replay_summary, disk_data = load_all(results_root)
    generated = []
    generated.extend(plot_no_disk_scalability(no_disk, outdir))
    generated.extend(plot_worker_cpu(no_disk, outdir))
    generated.extend(plot_replay_pool_sensitivity(replay_summary, outdir))
    generated.extend(plot_disk_throughput(disk_data, outdir))
    generated.extend(plot_fakedisk_work(disk_data, outdir))
    generated.extend(plot_bytes_per_txn(disk_data, outdir))
    write_index(outdir, generated)
    print(f"Generated {len(generated)} artifacts in {outdir}")


if __name__ == "__main__":
    main()
