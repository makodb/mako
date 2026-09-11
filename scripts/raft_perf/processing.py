#!/usr/bin/env python3
"""Turn a directory of raft_bench JSON records into plot-ready series.

Ported from ~/jetpack/data_processing/processing.py. What was kept is the
OUTPUT shape — a list of six-element rows per series — so that the two plot
scripts port with almost no change:

    row = [x, p50_ms, throughput, p99_ms, cdf_dict, throughput_spread]
    #      0   1        2           3       4         5

What was replaced is the input side. Jetpack regex-parsed and YAML-scraped
experiment logs; raft_bench writes one flat JSON object per run carrying every
parameter and every metric (decision D3 in docs/plans/raft-perf-harness.txt).
A missing key here is an error, not a zero: a silently-zeroed row produces a
plot that looks fine and is wrong.

Latencies in the records are MICROSECONDS. Everything this module returns is
in MILLISECONDS, matching the axis labels the ported plots use.

Standalone use:

    python3 scripts/raft_perf/processing.py <records-dir>
    python3 scripts/raft_perf/processing.py <records-dir> --json
"""

import argparse
import json
import os
import sys

# Every key raft_bench writes that this module or the plots depend on. A
# record lacking any of them is rejected by name rather than defaulted.
REQUIRED_KEYS = (
    "commit",
    "date",
    "host",
    "build_flavour",
    "raft_test_coro",
    "config",
    "partitions",
    "replicas",
    "group_mode",
    "payload_bytes",
    "batch",
    "offered_rate",
    "duration_sec",
    "warmup_sec",
    "measured_window_sec",
    "applied_per_sec",
    "offered_total",
    "applied_total",
    "offered_in_window",
    "applied_in_window",
    "offer_rejected",
    "offer_stalled_sec",
    "latency_mean_us",
    "latency_p50_us",
    "latency_p90_us",
    "latency_p99_us",
    "latency_p999_us",
    "peak_outstanding",
    "max_outstanding",
    "log_level",
    "label",
    "latency_cdf_us",
)

# The fields that NAME a curve for a human. These are what the series key is
# rendered from.
DISPLAY_FIELDS = ("partitions", "group_mode", "payload_bytes", "batch", "label")

# The fields that must MATCH for two records to be repetitions of each other.
# This is deliberately wider than DISPLAY_FIELDS: every one of these changes
# the number, so medianing across a difference in any of them would delete a
# real measurement and present the survivors as trials. `commit` in particular
# — re-running a sweep into the same output directory after a code change
# produces identical filenames, and without this the before and after would be
# silently averaged. `log_level` too: the apply path logs one line per applied
# entry, so a record taken at INFO is not comparable with one at WARN.
COMPARABILITY_FIELDS = (
    "partitions",
    "group_mode",
    "payload_bytes",
    "batch",
    "label",
    "offered_rate",
    "commit",
    "host",
    "build_flavour",
    "raft_test_coro",
    "replicas",
    "duration_sec",
    "warmup_sec",
    "max_outstanding",
    "log_level",
)

X_FIELD = "offered_rate"

# Anything grouped on must also be validated, or a record missing it slips past
# load_record and dies later as a bare KeyError that names no file — and that
# KeyError is not a RecordError, so the plot scripts' handler does not catch it
# either. Tie the two lists together here so they cannot drift apart again.
_ungated = [f for f in COMPARABILITY_FIELDS if f not in REQUIRED_KEYS]
assert not _ungated, (
    "COMPARABILITY_FIELDS not covered by REQUIRED_KEYS: %s" % ", ".join(_ungated))

# Numeric fields the series rows are built from. Coercing them here, by name,
# lets a bad value name its own file instead of surfacing as an anonymous
# TypeError six hundred records into a sweep.
NUMERIC_FIELDS = (
    "applied_per_sec",
    "offer_stalled_sec",
    "latency_p50_us",
    "latency_p99_us",
    "measured_window_sec",
    "applied_in_window",
    "offered_in_window",
)


class RecordError(Exception):
    """A record is missing a key, or carries one the plots cannot use."""


def load_record(path):
    """Read and validate one record. Raises RecordError with the file named."""
    try:
        with open(path, "r") as f:
            rec = json.load(f)
    except ValueError as exc:
        raise RecordError("%s: not valid JSON: %s" % (path, exc))
    if not isinstance(rec, dict):
        raise RecordError("%s: top level is %s, expected an object" % (path, type(rec).__name__))
    missing = [k for k in REQUIRED_KEYS if k not in rec]
    if missing:
        raise RecordError(
            "%s: missing required key(s): %s\n"
            "This record was not written by a current raft_bench; re-run the point "
            "rather than plotting a partial one." % (path, ", ".join(missing))
        )
    if not isinstance(rec["latency_cdf_us"], dict):
        raise RecordError("%s: latency_cdf_us is not an object" % path)
    # Coerce here so a null or a string in a numeric field names the file that
    # carries it, instead of surfacing hundreds of records later as an
    # anonymous TypeError from inside a float().
    for key in NUMERIC_FIELDS:
        try:
            rec[key] = float(rec[key])
        except (TypeError, ValueError):
            raise RecordError("%s: key %s is %r, expected a number" % (path, key, rec[key]))
    for key in ("partitions", "payload_bytes", "batch", "offered_rate"):
        try:
            rec[key] = int(rec[key])
        except (TypeError, ValueError):
            raise RecordError("%s: key %s is %r, expected an integer" % (path, key, rec[key]))
    if not isinstance(rec["label"], str):
        raise RecordError("%s: key label is %r, expected a string" % (path, rec["label"]))
    rec["_path"] = path
    return rec


def load_records(root, skip_rejected=True):
    """Load every *.json under `root`, recursively, sorted by path.

    Two kinds of record are dropped by default, both reported by name:

      * offer_rejected > 0 — the leader lost leadership mid-run, so the
        measured window is shorter than it claims;
      * applied_in_window == 0 or measured_window_sec <= 0 — nothing was
        measured at all. Such a record still carries a full set of zeroed
        latency percentiles and a zeroed CDF, so keeping it would anchor a
        saturation curve on a false point at (0 entries/s, 0 ms).
    """
    paths = []
    if os.path.isfile(root):
        paths = [root]
    else:
        for dirpath, _dirnames, filenames in os.walk(root):
            for name in sorted(filenames):
                if name.endswith(".json"):
                    paths.append(os.path.join(dirpath, name))
    paths.sort()

    records = []
    dropped_leadership = []
    dropped_empty = []
    for path in paths:
        rec = load_record(path)
        if skip_rejected and rec["offer_rejected"] > 0:
            dropped_leadership.append(path)
            continue
        if skip_rejected and (rec["applied_in_window"] <= 0 or
                              rec["measured_window_sec"] <= 0.0):
            dropped_empty.append(path)
            continue
        records.append(rec)
    if dropped_leadership:
        sys.stderr.write(
            "processing: dropped %d record(s) whose leader lost leadership mid-run:\n  %s\n"
            % (len(dropped_leadership), "\n  ".join(dropped_leadership))
        )
    if dropped_empty:
        sys.stderr.write(
            "processing: dropped %d record(s) that applied nothing in their measured "
            "window:\n  %s\n"
            % (len(dropped_empty), "\n  ".join(dropped_empty))
        )
    # A point whose offer threads spent most of the window blocked on the
    # driver's in-flight bound either sits past saturation — legitimate — or
    # had that bound leak because Raft accepted entries it then dropped. The
    # record cannot tell the two apart, so neither can this; it can only make
    # sure nobody reads such a point as an unremarkable one.
    stalled = [
        r["_path"] for r in records
        if r["measured_window_sec"] > 0.0 and
        r["offer_stalled_sec"] > r["measured_window_sec"] * max(r["partitions"], 1) * 0.5
    ]
    if stalled:
        sys.stderr.write(
            "processing: %d record(s) spent most of their window blocked on the "
            "in-flight bound. Past saturation this is expected; otherwise the bound "
            "leaked on a leadership flap and the point is dead:\n  %s\n"
            % (len(stalled), "\n  ".join(stalled))
        )
    if not records:
        raise RecordError("no usable records found under %s" % root)
    return records


def config_key(rec, x_field=X_FIELD):
    """A short, stable, human-readable name for one curve.

    The field being swept is deliberately EXCLUDED: when the x axis is
    offered_rate the payload size names the curve, and when the x axis is
    payload_bytes it must not, or every point lands on a curve of its own.

    This is a LABEL, not the grouping key — see comparability_key. Two records
    can share a label and still be incomparable (different commit, say), which
    build_series reports rather than silently averaging.
    """
    rendered = {
        "partitions": lambda: "p%d" % rec["partitions"],
        "group_mode": lambda: rec["group_mode"],
        "payload_bytes": lambda: "%dB" % rec["payload_bytes"],
        "batch": lambda: "b%d" % rec["batch"],
        "label": lambda: rec["label"],
    }
    parts = []
    for field in DISPLAY_FIELDS:
        if field == x_field:
            continue
        text = rendered[field]()
        if text:
            parts.append(text)
    return "/".join(parts) if parts else "all"


def comparability_key(rec, x_field=X_FIELD):
    """The tuple two records must share to be repetitions of each other.

    Everything in COMPARABILITY_FIELDS except the swept axis. Grouping on this
    rather than on the display label is what stops a before-change and an
    after-change sweep — which produce identical filenames in sibling
    directories — from being medianed into one point that reports `trials 3`.
    """
    return tuple((f, rec[f]) for f in COMPARABILITY_FIELDS if f != x_field)


def median(values):
    """Median value and the index it came from.

    Ported from jetpack's processing.median: it returns the index as well so
    the reported p50/p99/CDF all come from the SAME trial as the reported
    throughput, rather than mixing a median throughput with a mean latency.
    Unlike jetpack's version this one does not rewrite zeros in place — a zero
    throughput here means a real failed run, and load_records has already
    dropped the runs we know to be invalid.
    """
    if not values:
        return 0.0, 0
    chosen = sorted(values)[len(values) // 2]
    for i, v in enumerate(values):
        if v == chosen:
            return chosen, i
    return chosen, 0


def spread(values):
    """Population standard deviation, as jetpack's `var` computed it."""
    if not values:
        return 0.0
    mean = sum(values) / len(values)
    dev = [(x - mean) ** 2 for x in values]
    return (sum(dev) / len(values)) ** 0.5


def cdf_ms(rec):
    """Record's latency CDF as {fraction: milliseconds}, fractions in (0, 1]."""
    out = {}
    for pct, us in rec["latency_cdf_us"].items():
        try:
            frac = float(pct) / 100.0
        except (TypeError, ValueError):
            raise RecordError("%s: non-numeric CDF percentile key %r" % (rec["_path"], pct))
        out[frac] = float(us) / 1000.0
    return out


def build_series(records, x_field=X_FIELD):
    """Group records into curves and collapse repetitions to one row each.

    Returns (series, meta). `series` maps a display key to a list of rows
    sorted by the x field, with the same six-element layout jetpack's plots
    index:

        row[0] = x (the swept variable, by default offered_rate)
        row[1] = median-trial p50 latency, ms
        row[2] = median applied throughput, entries/sec
        row[3] = median-trial p99 latency, ms
        row[4] = median-trial CDF, {fraction: ms}
        row[5] = throughput spread across repetitions (population stddev)

    Grouping is on comparability_key, NOT on the display key. Where two
    incomparable groups would render to the same display key — a sweep re-run
    after a code change, most often — the key is disambiguated and the
    difference is reported on stderr, because silently averaging across it
    would delete a real measurement.
    """
    grouped = {}
    for rec in records:
        if x_field not in rec:
            raise RecordError("%s: no such key to sweep along: %s" % (rec["_path"], x_field))
        grouped.setdefault(comparability_key(rec, x_field), []).append(rec)

    # Map display key -> list of comparability keys that render to it.
    by_display = {}
    for ckey in grouped:
        rec = grouped[ckey][0]
        by_display.setdefault(config_key(rec, x_field), []).append(ckey)

    series = {}
    meta = {}
    for display in sorted(by_display):
        ckeys = by_display[display]
        if len(ckeys) > 1:
            differing = sorted({
                f for f, _ in ckeys[0]
                if len({dict(c)[f] for c in ckeys}) > 1
            })
            sys.stderr.write(
                "processing: %d groups of records share the label '%s' but differ in "
                "%s; they are NOT repetitions and are plotted as separate series.\n"
                % (len(ckeys), display, ", ".join(differing))
            )
        for ckey in sorted(ckeys, key=lambda c: repr(c)):
            recs = grouped[ckey]
            name = display
            if len(ckeys) > 1:
                # Disambiguate by whatever actually differs, so the legend says
                # which is which rather than "series 1" and "series 2".
                extra = "+".join(
                    "%s=%s" % (f, dict(ckey)[f])
                    for f in sorted({
                        f for f, _ in ckeys[0]
                        if len({dict(c)[f] for c in ckeys}) > 1
                    })
                )
                name = "%s [%s]" % (display, extra)
            by_x = {}
            for rec in recs:
                by_x.setdefault(rec[x_field], []).append(rec)
            rows = []
            row_meta = []
            for x in sorted(by_x):
                trials = by_x[x]
                tputs = [t["applied_per_sec"] for t in trials]
                tput, idx = median(tputs)
                pick = trials[idx]
                rows.append([
                    x,
                    pick["latency_p50_us"] / 1000.0,
                    tput,
                    pick["latency_p99_us"] / 1000.0,
                    cdf_ms(pick),
                    spread(tputs),
                ])
                row_meta.append({
                    "x": x,
                    "trials": len(trials),
                    "chosen": pick["_path"],
                    "files": [t["_path"] for t in trials],
                    "offered_in_window": pick["offered_in_window"],
                    "applied_in_window": pick["applied_in_window"],
                    "peak_outstanding": pick["peak_outstanding"],
                    "commit": pick["commit"],
                    "host": pick["host"],
                    "log_level": pick["log_level"],
                    "max_outstanding": pick["max_outstanding"],
                })
            series[name] = rows
            meta[name] = row_meta

    # The plan requires at least three runs per point. Say so once, loudly,
    # rather than letting a one-trial sweep's plot look like a three-trial one.
    thin = [(k, m["x"], m["trials"]) for k, ms in meta.items() for m in ms if m["trials"] < 3]
    if thin:
        sys.stderr.write(
            "processing: %d of %d points have fewer than 3 trials (min %d). "
            "A single run is not a measurement; re-run with --trials 3.\n"
            % (len(thin), sum(len(m) for m in meta.values()),
               min(t for _, _, t in thin))
        )
    return series, meta


def load_series(root, x_field=X_FIELD, skip_rejected=True):
    """Convenience wrapper: directory in, (series, meta) out."""
    return build_series(load_records(root, skip_rejected=skip_rejected), x_field=x_field)


def _format_table(series, meta):
    lines = []
    for key in sorted(series):
        lines.append("")
        lines.append("=== %s ===" % key)
        lines.append("%12s %6s %14s %12s %10s %10s"
                     % ("offered/s", "trials", "applied/s", "spread", "p50 ms", "p99 ms"))
        for row, rm in zip(series[key], meta[key]):
            x_label = "unthrottled" if row[0] == 0 else str(row[0])
            lines.append("%12s %6d %14.1f %12.1f %10.3f %10.3f"
                         % (x_label, rm["trials"], row[2], row[5], row[1], row[3]))
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="directory of raft_bench JSON records (searched recursively)")
    ap.add_argument("--x-field", default=X_FIELD,
                    help="record key to sweep along (default: %s)" % X_FIELD)
    ap.add_argument("--json", action="store_true", help="emit the series as JSON")
    ap.add_argument("--keep-rejected", action="store_true",
                    help="keep runs whose leader lost leadership (normally dropped)")
    args = ap.parse_args(argv)

    try:
        series, meta = load_series(args.input, x_field=args.x_field,
                                   skip_rejected=not args.keep_rejected)
    except RecordError as exc:
        sys.stderr.write("processing: %s\n" % exc)
        return 1

    if args.json:
        json.dump({"series": series, "meta": meta}, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    else:
        print(_format_table(series, meta))
    return 0


if __name__ == "__main__":
    sys.exit(main())
