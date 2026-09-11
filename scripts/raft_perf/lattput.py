#!/usr/bin/env python3
"""Latency-throughput (saturation) curve for raft_bench records.

Ported from ~/jetpack/data_processing/lattput.py. Kept: the plot itself —
median latency on y against achieved throughput on x, one line per
configuration, axes pinned at the origin, the 0.6 box aspect and the marker
vocabulary. Dropped: jetpack's protocol/replica-count branching (`raft` vs
`etcd` vs `copilot`, 3-rep vs 5-rep) and its hardcoded annotation indices,
neither of which has a counterpart here — this harness sweeps offered rate
within one protocol.

The x axis is what the run ACHIEVED (applied entries/sec), not what it was
asked for. That is the whole point of a saturation curve: past the knee the
offered rate keeps rising and the achieved rate does not.

    python3 scripts/raft_perf/lattput.py <records-dir> -o lattput.png

Requires matplotlib (>= 3.3, for Axes.set_box_aspect).
"""

import argparse
import os
import sys

import matplotlib
matplotlib.use("Agg")  # headless: these scripts run on the machine that swept
import matplotlib.pyplot as plt  # noqa: E402

matplotlib.rcParams["pdf.fonttype"] = 42
matplotlib.rcParams["ps.fonttype"] = 42

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import processing  # noqa: E402

MARKERS = ["-x", "-^", "-v", "-o", "-s", "-D", "-P", "-*"]


def plot_lattput(series, meta, ax, title="", ylim_mode="auto", log_axes="auto"):
    """Draw one line per configuration. Returns the drawn labels, in order.

    Two ordering rules matter here, and both were got wrong first:

    * The polyline follows the SWEEP, in offered-rate order — not achieved
      throughput. Past the knee the achieved rate stops rising and starts
      wandering, so sorting by it scrambles exactly the points the plot exists
      to show: a rate array that probes 14900, 17900, 23800 and 35700 against a
      ceiling of ~11900 lands four points within noise of each other on x with
      steadily rising latency, and a throughput-sorted polyline draws that as a
      sawtooth in which latency falls as throughput grows.
    * The unthrottled point (offered_rate == 0) is NOT part of the line. Its
      latency is not a property of Raft but of the in-flight bound the run was
      given — Little's law, bound/throughput — so connecting it folds the curve
      back on itself and swamps the y axis. It is drawn as a labelled star.
    """
    labels = []
    throttled_max_lat = 0.0
    offscale = []
    series_ceilings = []
    for i, key in enumerate(sorted(series)):
        rows = series[key]
        if not rows:
            continue
        throttled = sorted([r for r in rows if r[0] != 0], key=lambda r: r[0])
        unthrottled = [r for r in rows if r[0] == 0]
        marker = MARKERS[i % len(MARKERS)]

        if throttled:
            ax.plot([r[2] for r in throttled], [r[1] for r in throttled], marker,
                    label=key, linewidth=3, ms=8, markeredgewidth=3)
            labels.append(key)
            throttled_max_lat = max(throttled_max_lat, max(r[1] for r in throttled))
            series_ceilings.append(max(r[2] for r in throttled))

        for r in unthrottled:
            # Label the star itself when there is no line for this key, or the
            # figure comes out with no legend at all and three identical red
            # stars nobody can tell apart (a --phase knee directory is exactly
            # that shape).
            star_label = "_nolegend_" if throttled else "%s (unthrottled)" % key
            ax.plot(r[2], r[1], "r*", label=star_label, ms=14, markeredgewidth=3)
            if not throttled:
                labels.append(key)
                series_ceilings.append(r[2])
            offscale.append((key, r[2], r[1]))

    ax.set_xlabel("Throughput (applied entries/s)")
    ax.set_ylabel("Median enqueue-to-apply latency (ms)")

    # A rate-phase directory mixes payload sizes whose CEILINGS differ by more
    # than two orders of magnitude (11900/s at 4 KiB against 65/s at 1 MiB).
    # On shared linear axes the small-entry curve is flattened onto the x axis
    # and the large-entry curve occupies half a percent of the width, which
    # reads as "the 4 KiB configuration has no latency at any rate". Switch to
    # log axes when the SERIES are that far apart — not when a single series
    # sweeps a wide range of offered rates, which is normal and reads better
    # linear.
    wide = (len(series_ceilings) > 1 and
            max(series_ceilings) > 20.0 * max(min(series_ceilings), 1e-9))
    use_log = (log_axes == "on") or (log_axes == "auto" and wide)
    if use_log:
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("Throughput (applied entries/s, log scale)")
        ax.set_ylabel("Median enqueue-to-apply latency (ms, log scale)")
    else:
        ax.set_xlim(left=0)
        ax.set_ylim(bottom=0)
        if ylim_mode == "auto" and throttled_max_lat > 0:
            top = throttled_max_lat * 1.6
            ax.set_ylim(0, top)
            for key, x, y in offscale:
                if y > top:
                    ax.annotate("%s unthrottled: %.0f ms" % (key, y), xy=(x, top),
                                xytext=(x, top * 0.86), ha="right", fontsize=7,
                                color="red",
                                arrowprops=dict(arrowstyle="->", color="red", lw=1))

    if labels:
        ax.legend(frameon=False, fontsize="small")
    ax.set_box_aspect(0.6)
    ax.grid(True, which="both", linewidth=0.4, alpha=0.4)

    # Say how many trials back the thinnest point, so a one-run smoke sweep
    # does not read like a three-run measurement.
    min_trials = None
    for rows_meta in meta.values():
        for m in rows_meta:
            if min_trials is None or m["trials"] < min_trials:
                min_trials = m["trials"]
    if title:
        ax.set_title(title, fontsize=13)
    if min_trials is not None and min_trials < 3:
        ax.text(0.5, -0.22,
                "min %d trial%s per point — a single run is not a measurement" % (
                    min_trials, "" if min_trials == 1 else "s"),
                transform=ax.transAxes, ha="center", fontsize=9, color="#a33")
    return labels


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="directory of raft_bench JSON records")
    ap.add_argument("-o", "--output", default="lattput.png", help="output image path")
    ap.add_argument("--x-field", default=processing.X_FIELD,
                    help="record key that identifies a sweep point (default: %(default)s)")
    ap.add_argument("--title", default="Raft enqueue-to-apply latency vs throughput")
    ap.add_argument("--log-y", action="store_true",
                    help="force log axes (the default already switches to them when the "
                         "throughput span across series exceeds 20x)")
    ap.add_argument("--log-axes", choices=("auto", "on", "off"), default="auto",
                    help="log-scale both axes: 'auto' (default) when the throughput span "
                         "across series exceeds 20x, else linear")
    ap.add_argument("--ylim", choices=("auto", "full"), default="auto",
                    help="'auto' (default) scales the latency axis to the throttled points "
                         "and annotates the unthrottled star if it falls off the top; "
                         "'full' lets matplotlib fit everything, which a queued "
                         "unthrottled point will dominate")
    args = ap.parse_args(argv)

    try:
        series, meta = processing.load_series(args.input, x_field=args.x_field)
    except processing.RecordError as exc:
        sys.stderr.write("lattput: %s\n" % exc)
        return 1

    plt.rcParams["font.size"] = 12
    fig, ax = plt.subplots(figsize=(8, 5.5))
    labels = plot_lattput(series, meta, ax, title=args.title,
                          ylim_mode="full" if args.log_y else args.ylim,
                          log_axes="on" if args.log_y else args.log_axes)
    if not labels:
        sys.stderr.write("lattput: nothing to plot\n")
        return 1

    out_dir = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(out_dir, exist_ok=True)
    fig.savefig(args.output, bbox_inches="tight", dpi=150)
    print("lattput: wrote %s (%d series)" % (args.output, len(labels)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
