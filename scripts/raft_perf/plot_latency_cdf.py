#!/usr/bin/env python3
"""Enqueue-to-apply latency CDF, one curve per configuration.

Ported from ~/jetpack/data_processing/plot_raft.py — specifically its
`plot_cdf`, the only part of that file with a counterpart here. Kept: latency
on x, cumulative fraction on y pinned to [0, 1], linewidth 3, the 0.6 box
aspect, and the TrueType-embedding rcParams. Dropped: the seven-way
"slowness" experiment matrix and the four-panel figure it belonged to; this
harness injects no faults (that is out of scope per
the raft_bench harness plan (removed; see git history) section 12).

A CDF describes ONE run, not a whole curve, so the script has to choose which
point of each configuration to show. By default it takes the
highest-throughput THROTTLED point — the saturated end, where the tail is
interesting, without the unthrottled point whose distribution is set by the
run's in-flight bound rather than by Raft. --at-rate picks a specific offered
rate instead; --at-rate 0 asks for the unthrottled one explicitly, and the
legend then says so.

    python3 scripts/raft_perf/plot_latency_cdf.py <records-dir> -o cdf.png
    python3 scripts/raft_perf/plot_latency_cdf.py <records-dir> --at-rate 5000

Requires matplotlib (>= 3.3, for Axes.set_box_aspect).
"""

import argparse
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

matplotlib.rcParams["pdf.fonttype"] = 42
matplotlib.rcParams["ps.fonttype"] = 42

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import processing  # noqa: E402


def pick_row(rows, at_rate):
    """Choose the row whose CDF to draw. Returns (row, why) or (None, why).

    The default deliberately skips the unthrottled point. Its distribution is
    set by --max-outstanding, not by Raft — at an in-flight bound of 4096 and
    12000 entries/s the whole CDF sits around 340 ms — so defaulting to it
    would put a CDF centred on a third of a second beside a saturation curve
    topping out at 8 ms, from the same directory, with nothing saying why.
    Ask for it explicitly with --at-rate 0.
    """
    if not rows:
        return None, "no rows"
    if at_rate is None:
        throttled = [r for r in rows if r[0] != 0]
        if throttled:
            best = max(throttled, key=lambda r: r[2])
            return best, "offered %d/s, achieved %.0f/s" % (best[0], best[2])
        best = max(rows, key=lambda r: r[2])
        return best, "UNTHROTTLED, %.0f/s — shape set by the in-flight bound" % best[2]
    for r in rows:
        if r[0] == at_rate:
            if at_rate == 0:
                return r, "UNTHROTTLED, %.0f/s — shape set by the in-flight bound" % r[2]
            return r, "offered %d/s, achieved %.0f/s" % (at_rate, r[2])
    return None, "no point at offered rate %s" % at_rate


def plot_cdf(series, ax, at_rate=None, x_max=None):
    drawn = 0
    for key in sorted(series):
        row, why = pick_row(series[key], at_rate)
        if row is None:
            sys.stderr.write("plot_latency_cdf: skipping %s (%s)\n" % (key, why))
            continue
        cdf = row[4]
        if not cdf:
            sys.stderr.write("plot_latency_cdf: skipping %s (empty CDF)\n" % key)
            continue
        # Sort by fraction, not by sorting the two axes independently: the
        # pairing is what makes it a CDF.
        points = sorted(cdf.items())
        fracs = [p for p, _ in points]
        lats = [l for _, l in points]
        ax.plot(lats, fracs, linewidth=3, label="%s — %s" % (key, why))
        drawn += 1

    ax.set_xlabel("Enqueue-to-apply latency (ms)")
    ax.set_ylabel("CDF")
    ax.set_ylim([0, 1])
    ax.set_xlim(left=0)
    if x_max is not None:
        ax.set_xlim([0, x_max])
    ax.set_box_aspect(0.6)
    ax.grid(True, linewidth=0.4, alpha=0.4)
    if drawn:
        ax.legend(frameon=False, fontsize="small", loc="lower right")
    return drawn


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="directory of raft_bench JSON records")
    ap.add_argument("-o", "--output", default="latency_cdf.png", help="output image path")
    ap.add_argument("--at-rate", type=int, default=None,
                    help="offered rate whose CDF to draw (0 = unthrottled); default is "
                         "each configuration's highest-throughput THROTTLED point")
    ap.add_argument("--x-field", default=processing.X_FIELD,
                    help="record key that identifies a sweep point (default: %(default)s)")
    ap.add_argument("--x-max", type=float, default=None, help="clamp the latency axis (ms)")
    ap.add_argument("--log-x", action="store_true", help="log-scale the latency axis")
    ap.add_argument("--title", default="Raft enqueue-to-apply latency CDF")
    args = ap.parse_args(argv)

    try:
        series, _meta = processing.load_series(args.input, x_field=args.x_field)
    except processing.RecordError as exc:
        sys.stderr.write("plot_latency_cdf: %s\n" % exc)
        return 1

    plt.rcParams["font.size"] = 12
    fig, ax = plt.subplots(figsize=(8, 5.5))
    drawn = plot_cdf(series, ax, at_rate=args.at_rate, x_max=args.x_max)
    if args.log_x:
        ax.set_xscale("log")
        ax.set_xlim(left=None)
    if not drawn:
        sys.stderr.write("plot_latency_cdf: nothing to plot\n")
        return 1
    ax.set_title(args.title, fontsize=13)

    out_dir = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(out_dir, exist_ok=True)
    fig.savefig(args.output, bbox_inches="tight", dpi=150)
    print("plot_latency_cdf: wrote %s (%d series)" % (args.output, drawn))
    return 0


if __name__ == "__main__":
    sys.exit(main())
