#!/usr/bin/env python3
"""Plan phase N12: judge a rotation_trial.sh run of every point against the
plan's gates, and write the summary as Markdown.

    scripts/raft_perf/n12_summary.py N12_DIR [--rounds 25] > SUMMARY.md

N12_DIR holds one directory per point, named as scripts in the plan's N12
runner name them (s4k_i<interval>_b<bytes>, s286k_..., stall_b<bytes>,
off_...). Arms: build_rust_pre (pre), build_rust_store (store), build_rust
(post), build_hybrid_pre and build (hybrid pre / post).

Gates (the two-lane RPC plan (removed; see git history), N12 "Pass/fail"):
  * store vs pre at every point: median B/A-1 within +-2% for throughput and
    p50, +-5% for p99, and no significant regression (sign test p <= 0.05
    in the worse direction); max_apply_gap_us, the stalled follower's
    snapshot_install_us_p50 and catchup_ms not worse by more than 5%.
  * RSS at 4 KB with 16 and 60 MiB: an arm's rss_peak_kb minus its own peak
    with snapshots off (off_4k_r240) at most one snapshot image above pre's.
  * post vs store: install_rpcs_sent per catch-up at most 2, and the
    store-vs-pre bounds hold for post vs store too.
  * hybrid post vs pre: the same bounds as store vs pre.
  * disabled path (off_*): post vs pre under the same bounds.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PRE, STORE, POST, HPRE, HPOST = ("build_rust_pre", "build_rust_store", "build_rust",
                                 "build_hybrid_pre", "build")
BOUNDS = {"applied_per_sec": 0.02, "latency_p50_us": 0.02, "latency_p99_us": 0.05}
NOT_WORSE = ("max_apply_gap_us", "stalled_snapshot_install_us_p50", "stalled_catchup_ms")


def stats(d, rounds, a, b):
    p = subprocess.run([sys.executable, os.path.join(HERE, "paired_stats.py"), d, str(rounds),
                        a, b, "--json"], capture_output=True, text=True)
    return json.loads(p.stdout) if p.stdout.strip() else None


def arm_median(d, rounds, arm, key):
    vals = []
    for i in range(1, rounds + 1):
        try:
            with open(os.path.join(d, f"r{i}.{arm}.json")) as f:
                r = json.load(f)
        except (OSError, ValueError):
            continue
        if key.startswith("stalled_"):
            v = r.get("stalled_follower")
            x = r.get(f"{v}_{key[len('stalled_'):]}") if v else None
        else:
            x = r.get(key)
        if isinstance(x, (int, float)):
            vals.append(x)
    return statistics.median(vals) if vals else None


def judge(s, min_pairs):
    """(verdict, failures) for one comparison under the equivalence bounds."""
    if s is None or s["pairs"] < min_pairs:
        return False, [f"only {0 if s is None else s['pairs']} complete rounds"]
    bad = []
    for key, bound in BOUNDS.items():
        m = s["metrics"].get(key)
        if m is None:
            continue
        worse = m["median_ratio"] < 0 if m["better"] == "higher" else m["median_ratio"] > 0
        # Beyond the bound in the better direction is an improvement, not a
        # failure of equivalence: it is reported, not failed.
        if abs(m["median_ratio"]) > bound and worse:
            bad.append(f"{key} {m['median_ratio']:+.2%} outside +-{bound:.0%}")
        n_worse = m["neg"] if m["better"] == "higher" else m["pos"]
        n_better = m["pos"] if m["better"] == "higher" else m["neg"]
        if worse and m["sign_p"] <= 0.05 and n_worse > n_better:
            bad.append(f"{key} regresses (sign p {m['sign_p']:.3f})")
    for key in NOT_WORSE:
        m = s["metrics"].get(key)
        if m is not None and m["median_ratio"] > 0.05:
            bad.append(f"{key} {m['median_ratio']:+.2%} worse than 5%")
    return not bad, bad


def improved(s):
    """Metrics past their bound in the better direction."""
    out = []
    for key, bound in list(BOUNDS.items()) + [(k, 0.05) for k in NOT_WORSE]:
        m = s["metrics"].get(key) if s else None
        if m is None:
            continue
        better = m["median_ratio"] > 0 if m["better"] == "higher" else m["median_ratio"] < 0
        if better and abs(m["median_ratio"]) > bound:
            out.append(f"{key} {m['median_ratio']:+.1%}")
    return out


def fmt(s, key):
    m = s["metrics"].get(key) if s else None
    if m is None:
        return "--"
    return f"{m['median_ratio']:+.2%} (p={m['sign_p']:.2f})"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--rounds", type=int, default=25)
    o = ap.parse_args()
    points = sorted(p for p in os.listdir(o.dir) if os.path.isdir(os.path.join(o.dir, p)))
    out = ["| point | comparison | rounds | throughput | p50 | p99 | p99.9 | max apply gap | verdict |",
           "|---|---|---|---|---|---|---|---|---|"]
    fails = []
    detail = []
    for p in points:
        d = os.path.join(o.dir, p)
        rounds = 10 if p.startswith("stall") else o.rounds
        min_pairs = rounds * 23 // 25
        if p.startswith("off_"):
            comps = [("post vs pre (disabled path)", PRE, POST, True)]
        else:
            comps = [("store vs pre", PRE, STORE, True), ("post vs store", STORE, POST, True),
                     ("hybrid post vs pre", HPRE, HPOST, True), ("post vs pre (context)", PRE, POST, False)]
        for label, a, b, gated in comps:
            s = stats(d, rounds, a, b)
            ok, bad = judge(s, min_pairs) if gated else (None, [])
            verdict = "context" if not gated else ("pass" if ok else "**FAIL**: " + "; ".join(bad))
            if gated and ok and improved(s):
                verdict = "pass; improved: " + ", ".join(improved(s))
            if gated and not ok:
                fails.append(f"{p} {label}: " + "; ".join(bad))
            out.append(f"| {p} | {label} | {s['pairs'] if s else 0}/{rounds} | "
                       f"{fmt(s, 'applied_per_sec')} | {fmt(s, 'latency_p50_us')} | "
                       f"{fmt(s, 'latency_p99_us')} | {fmt(s, 'latency_p999_us')} | "
                       f"{fmt(s, 'max_apply_gap_us')} | {verdict} |")
        if p.startswith("stall"):
            s = stats(d, rounds, STORE, POST)
            for arm in (PRE, STORE, POST, HPRE, HPOST):
                detail.append(f"| {p} | {arm} | {arm_median(d, rounds, arm, 'install_rpcs_sent')} | "
                              f"{arm_median(d, rounds, arm, 'stalled_install_rpcs_received')} | "
                              f"{arm_median(d, rounds, arm, 'stalled_catchup_ms')} | "
                              f"{arm_median(d, rounds, arm, 'stalled_snapshot_install_us_p50')} | "
                              f"{arm_median(d, rounds, arm, 'latency_p99_us')} |")
            sent = arm_median(d, rounds, POST, "install_rpcs_sent")
            if sent is None or sent > 2:
                fails.append(f"{p}: post's install_rpcs_sent per catch-up {sent} > 2")
    print("## Paired comparisons (median B/A - 1, sign-test p)\n")
    print("\n".join(out))
    if detail:
        print("\n## Catch-up (stall runs), medians\n")
        print("| point | arm | install RPCs sent (leader) | received (stalled) | catchup_ms | "
              "install_us p50 | leader p99 us (whole run) |")
        print("|---|---|---|---|---|---|---|")
        print("\n".join(detail))
    # RSS: snapshot bytes over the same arm's snapshots-off peak.
    off = os.path.join(o.dir, "off_4k_r240")
    rss_rows = []
    for p in points:
        if not p.startswith("s4k_") or not (p.endswith("b16777216") or p.endswith("b62914560")):
            continue
        img_kb = int(p.rsplit("_b", 1)[1]) // 1024
        d = os.path.join(o.dir, p)
        extra = {}
        for arm in (PRE, STORE, POST):
            on = arm_median(d, o.rounds, arm, "rss_peak_kb")
            base = arm_median(off, o.rounds, arm, "rss_peak_kb")
            extra[arm] = None if on is None or base is None else on - base
        ok = all(extra[x] is not None for x in extra) and \
            all(extra[x] - extra[PRE] <= img_kb for x in (STORE, POST))
        if not ok:
            fails.append(f"{p}: RSS over snapshots-off {extra} exceeds pre's by more than {img_kb} KiB")
        rss_rows.append(f"| {p} | {extra[PRE]} | {extra[STORE]} | {extra[POST]} | {img_kb} | "
                        f"{'pass' if ok else '**FAIL**'} |")
    if rss_rows:
        print("\n## RSS: leader peak minus the same arm's peak with snapshots off, KiB (medians)\n")
        print("| point | pre | store | post | one image | verdict |")
        print("|---|---|---|---|---|---|")
        print("\n".join(rss_rows))
    print("\n## Gates missed\n")
    print("\n".join(f"- {f}" for f in fails) if fails else "None.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
