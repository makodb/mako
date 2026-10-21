#!/usr/bin/env python3
"""Compare two raft_bench record sets and say whether performance moved.

    python3 scripts/raft_perf/compare.py <before-dir> <after-dir>
    python3 scripts/raft_perf/compare.py before/ after/ --threshold 5

This is the thing that turns the harness into a criterion. It matches points
between the two sets by configuration, and for each metric reports the change
alongside the noise floor of the runs themselves, so a delta is never read
without the evidence that it is bigger than run-to-run variation.

WHAT IT COMPARES, and in which direction:

    throughput (applied entries/sec)   higher is better; a drop is a regression
    p50 latency, p99 latency           lower is better; a rise is a regression

WHY THERE ARE NO p-VALUES. With three trials per point a t-test is mostly
reporting the smallness of n. Instead each comparison is judged against the
MINIMUM DETECTABLE EFFECT for the trial count actually used --
2.8 * CV / sqrt(n), the usual z-based constant for 5% significance at 80%
power -- pooled across the two sides. A delta smaller than that is reported as
within noise, because with that many runs it is not distinguishable from
noise. Raising the trial count lowers the bar; the tool tells you where it is.

Exit status: 0 if nothing regressed beyond the threshold, 1 if something did,
2 on a usage or data error. Suitable for a CI gate.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import processing  # noqa: E402

# processing groups on commit (deliberately: two commits are not repetitions of
# each other), so a before/after pair renders keys like
# "p1/single/4096B/b1/rate [commit=abc1234]". For comparison that suffix is
# exactly what we are matching ACROSS, so strip it.
_DISAMBIGUATOR = re.compile(r"\s*\[[^\]]*\]\s*$")


def normalize(key):
    return _DISAMBIGUATOR.sub("", key)


def cv(mean, sd):
    return 100.0 * sd / mean if mean else 0.0


def mde(mean_a, sd_a, n_a, mean_b, sd_b, n_b):
    """Minimum detectable effect (percent) for these two samples, pooled."""
    pooled_cv = ((cv(mean_a, sd_a) ** 2 + cv(mean_b, sd_b) ** 2) / 2.0) ** 0.5
    n = min(n_a, n_b)
    return 2.8 * pooled_cv / (n ** 0.5) if n else float("inf")


# (row[6] key for mean, row[6] key for sd, label, higher_is_better)
METRICS = (
    ("tput_mean", "tput_sd", "throughput", True),
    ("p50_mean", "p50_sd", "p50 latency", False),
    ("p99_mean", "p99_sd", "p99 latency", False),
)


def index_by_config(series):
    """{normalized key: {x: row}}"""
    out = {}
    for key, rows in series.items():
        bucket = out.setdefault(normalize(key), {})
        for row in rows:
            bucket[row[0]] = row
    return out


def commits_in(root):
    try:
        return sorted({r["commit"] for r in processing.load_records(root)})
    except processing.RecordError:
        return []


def compare(before_dir, after_dir, threshold, x_field):
    before, _ = processing.load_series(before_dir, x_field=x_field)
    after, _ = processing.load_series(after_dir, x_field=x_field)
    b_idx, a_idx = index_by_config(before), index_by_config(after)

    print("before: %s" % before_dir)
    print("        commit(s): %s" % ", ".join(commits_in(before_dir)))
    print("after:  %s" % after_dir)
    print("        commit(s): %s" % ", ".join(commits_in(after_dir)))
    print("regression threshold: %.1f%%" % threshold)

    only_b = sorted(set(b_idx) - set(a_idx))
    only_a = sorted(set(a_idx) - set(b_idx))
    for k in only_b:
        print("\n!! only in before, not compared: %s" % k)
    for k in only_a:
        print("\n!! only in after, not compared: %s" % k)

    regressions, compared, inconclusive = [], 0, 0

    for key in sorted(set(b_idx) & set(a_idx)):
        print("\n=== %s ===" % key)
        xs = sorted(set(b_idx[key]) & set(a_idx[key]))
        missing = sorted((set(b_idx[key]) ^ set(a_idx[key])))
        if missing:
            print("    (points present on only one side, skipped: %s)"
                  % ", ".join("unthrottled" if m == 0 else str(m) for m in missing))
        for x in xs:
            rb, ra = b_idx[key][x], a_idx[key][x]
            db, da = rb[6], ra[6]
            x_label = "unthrottled" if x == 0 else str(x)
            print("  offered %s  (n=%d before, n=%d after)"
                  % (x_label, db["trials"], da["trials"]))
            for mkey, skey, label, higher_better in METRICS:
                mb, sb = db[mkey], db[skey]
                ma, sa = da[mkey], da[skey]
                if not mb:
                    continue
                delta = 100.0 * (ma - mb) / mb
                floor = mde(mb, sb, db["trials"], ma, sa, da["trials"])
                worse = (delta < 0) if higher_better else (delta > 0)
                magnitude = abs(delta)

                if magnitude <= floor:
                    verdict = "within noise"
                elif worse and magnitude > threshold:
                    verdict = "REGRESSION"
                    regressions.append((key, x_label, label, delta))
                elif worse:
                    verdict = "worse, under threshold"
                else:
                    verdict = "better"

                if magnitude > floor and db["trials"] < 3:
                    inconclusive += 1
                compared += 1
                print("    %-12s %10.3f +-%-8.3f -> %10.3f +-%-8.3f  %+7.2f%%  "
                      "(noise floor %.2f%%)  %s"
                      % (label, mb, sb, ma, sa, delta, floor, verdict))

    print("\n" + "=" * 72)
    print("compared %d metric points across %d configurations"
          % (compared, len(set(b_idx) & set(a_idx))))
    if inconclusive:
        print("NOTE: %d comparisons exceeded the noise floor but rest on fewer than "
              "3 trials; re-run with --trials 3 or more before believing them."
              % inconclusive)
    if regressions:
        print("REGRESSIONS (worse by more than %.1f%% and beyond the noise floor):"
              % threshold)
        for key, x_label, label, delta in regressions:
            print("  %-40s offered %-12s %-12s %+.2f%%" % (key, x_label, label, delta))
        return 1
    print("No regression beyond %.1f%%." % threshold)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("before", help="directory of records taken before the change")
    ap.add_argument("after", help="directory of records taken after the change")
    ap.add_argument("--threshold", type=float, default=5.0,
                    help="percent a metric may worsen before it counts as a "
                         "regression (default: %(default)s)")
    ap.add_argument("--x-field", default=processing.X_FIELD,
                    help="record key identifying a sweep point (default: %(default)s)")
    args = ap.parse_args(argv)

    for d in (args.before, args.after):
        if not os.path.exists(d):
            sys.stderr.write("compare: no such path: %s\n" % d)
            return 2
    try:
        return compare(args.before, args.after, args.threshold, args.x_field)
    except processing.RecordError as exc:
        sys.stderr.write("compare: %s\n" % exc)
        return 2


if __name__ == "__main__":
    sys.exit(main())
