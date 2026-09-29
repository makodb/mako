#!/usr/bin/env python3
"""Drive Jetpack's own (unedited) analysis scripts over a run_jetpack_sweep.sh
result tree, and tabulate the arms side by side.

    jetpack_compare.py ROOT [--baseline baseline]

ROOT/<setting>/round<k>/log/ holds Jetpack-format .res/.csv files (one set per
host group server0..9) plus a <prefix>.sidecar.json per run. For every setting
(e.g. wan20ms, loopback) this writes, under ROOT/<setting>/:

  round<k>/view/log/      symlinks renaming each run into the one workload
                          Jetpack's figure scripts hard-code (rw_1000000); a
                          Zipf run becomes protocol none_<arm>_zipf<theta>
  round<k>/view/figs/     Jetpack's figures: gen_tput_p90_figures.
                          draw_compare_figure (throughput vs p50/p90/p99) and
                          gen_latency_cdf.draw_compare, titled with the delay
  summary.md / .json      per point: Jetpack's aggregate() throughput and
                          median-of-hosts p50/p90/p99, median over rounds with
                          the min..max spread, the paired per-round ratio to
                          the baseline, the offered load, and the knee from
                          derive_fixed_conc.find_latency_envelope_conc

Nothing in the copied scripts is edited; only their module attributes
(protocol lists, axis limits) are set here.
"""
import argparse
import glob
import json
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_tput_p90_figures as G  # noqa: E402
import gen_latency_cdf as C  # noqa: E402
import derive_fixed_conc as D  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402

FILE_RE = re.compile(
    r"^none_(?P<arm>[A-Za-z0-9]+)-60c1s5r10p-(?P<wl>rw_[A-Za-z0-9_.]+)-concurrent_(?P<n>\d+)"
    r"-0-YCSB_A(?:-(?P<rest>server\d+\.(?:res|csv))|\.sidecar\.json)$")
COLORS = ["#437c17", "#B22222", "#1f77b4", "#ff7f0e", "#8c564b"]
MARKERS = ["^", "o", "D", "P", "X"]
STYLES = ["-", "--", "-.", ":", "-"]
METRICS = ["p50", "p90", "p99"]


def wl_tag(wl):
    return "" if wl == "rw_1000000" else "_" + wl.replace("rw_", "").replace("_", "")


def view_proto(arm, wl):
    return f"none_{arm}{wl_tag(wl)}"


def build_view(round_dir):
    """Symlink round_dir/log into round_dir/view/log under Jetpack's naming."""
    src = os.path.join(round_dir, "log")
    dst = os.path.join(round_dir, "view", "log")
    os.makedirs(dst, exist_ok=True)
    runs = {}
    for name in os.listdir(src):
        m = FILE_RE.match(name)
        if not m:
            continue
        arm, wl, n, rest = m["arm"], m["wl"], int(m["n"]), m["rest"]
        if rest is None:
            with open(os.path.join(src, name)) as f:
                runs[(arm, wl, n)] = json.load(f)
            continue
        link = os.path.join(dst, f"{view_proto(arm, wl)}-60c1s5r10p-rw_1000000-concurrent_{n}-0-YCSB_A-{rest}")
        if not os.path.lexists(link):
            os.symlink(os.path.join(src, name), link)
    return dst, runs


def offered_per_s(round_dir, arm, wl, n):
    p = os.path.join(round_dir, "log", f"none_{arm}-60c1s5r10p-{wl}-concurrent_{n}-0-YCSB_A-server0.res")
    try:
        txt = open(p).read()
    except FileNotFoundError:
        return None
    m = re.search(r"issued_total=(\d+).*", txt)
    d = re.search(r"duration_s=([0-9.]+)", txt)
    return int(m.group(1)) / float(d.group(1)) if m and d else None


def delay_label(runs):
    delays = sorted({r.get("wan_delay_ms", 0) for r in runs.values()})
    if delays == [0]:
        return "loopback, no injected delay", 0
    if len(delays) == 1:
        d = delays[0]
        return f"WAN_DELAY_MS={d:g}: {d:g} ms injected RTT (requests delayed, as Jetpack)", d
    return f"MIXED delays {delays}", None


def rescaling_savefig(orig):
    """Jetpack's figures fix y to 0-2000 ms; rescale to the data first."""
    def wrapped(fig, path, **kw):
        for ax in fig.axes:
            ys = [y for line in ax.get_lines() for y in line.get_ydata()]
            if ys and ax.get_ylabel().lower().find("cumulative") < 0:
                ax.set_ylim(0, max(ys) * 1.15)
        return orig(fig, path, **kw)
    return wrapped


G.savefig_all = rescaling_savefig(G.savefig_all)


def arm_lines(arms, labels):
    g_lines, c_lines = [], []
    for i, arm in enumerate(arms):
        g_lines.append((labels.get(arm, arm), COLORS[i % 5], MARKERS[i % 5], STYLES[i % 5], f"none_{arm}", 0))
        c_lines.append((labels.get(arm, arm), COLORS[i % 5], STYLES[i % 5], f"none_{arm}", 0, arm))
    return g_lines, c_lines


def fmt_spread(vals, digits=1):
    vals = [v for v in vals if v is not None]
    if not vals:
        return "n/a"
    med = statistics.median(vals)
    return f"{med:.{digits}f} ({min(vals):.{digits}f}..{max(vals):.{digits}f})"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--baseline", default="baseline")
    ap.add_argument("--label", action="append", default=[],
                    help="arm=Display name, repeatable")
    args = ap.parse_args()
    labels = dict(x.split("=", 1) for x in args.label)

    for setting_dir in sorted(glob.glob(os.path.join(args.root, "*"))):
        rounds = sorted(glob.glob(os.path.join(setting_dir, "round*")),
                        key=lambda p: int(re.sub(r"\D", "", os.path.basename(p))))
        if not rounds:
            continue
        setting = os.path.basename(setting_dir)
        all_runs = {}
        views = []
        for rd in rounds:
            v, runs = build_view(rd)
            views.append((rd, v, runs))
            all_runs.update({(os.path.basename(rd),) + k: r for k, r in runs.items()})
        title, delay = delay_label(all_runs)
        arms = sorted({k[1] for k in all_runs}, key=lambda a: (a != args.baseline, a))
        points = sorted({(k[2], k[3]) for k in all_runs}, key=lambda p: (p[0] != "rw_1000000", p[0], p[1]))
        g_lines, c_lines = arm_lines(arms, labels)

        # ---- per-round numbers, via Jetpack's aggregate() ----
        data = {}  # (arm, wl, n) -> list over rounds of {tput, p50, p90, p99, offered}
        knees = {}  # (arm, wl) -> list over rounds of knee N
        for rd, vlog, runs in views:
            for arm in arms:
                for wl, n in points:
                    row = {"round": os.path.basename(rd)}
                    for metric in METRICS:
                        agg = G.aggregate(vlog, view_proto(arm, wl), n, 0, metric)
                        if agg is None:
                            row = None
                            break
                        row["tput"], row[metric] = agg
                    if row is not None:
                        row["offered"] = offered_per_s(rd, arm, wl, n)
                        data.setdefault((arm, wl, n), []).append(row)
            tp = D.collect_throughputs(vlog, site="60c1s5r10p", servers=[f"server{i}" for i in range(10)])
            lat = D.collect_latencies(vlog, site="60c1s5r10p", servers=[f"server{i}" for i in range(10)])
            for arm in arms:
                for wl in sorted({p[0] for p in points}):
                    proto = view_proto(arm, wl)
                    if proto not in tp:
                        continue
                    sel = D.find_latency_envelope_conc(tp[proto], lat.get(proto, {}), mode="0")
                    if sel[0] is not None and len(tp[proto]) >= 3:
                        knees.setdefault((arm, wl), []).append(int(sel[0].split("_")[1]))

            # ---- Jetpack's figures for this round ----
            figs = os.path.join(os.path.dirname(vlog), "figs")
            os.makedirs(figs, exist_ok=True)
            G.COMPARE_LINES = g_lines
            for metric in METRICS:
                G.draw_compare_figure(vlog, os.path.join(figs, f"tput_{metric}_arms.pdf"), metric=metric,
                                      dc_label=f"{title} - {os.path.basename(rd)}")
            C.COMPARE_LINES = c_lines
            for wl, n in points:
                if wl != "rw_1000000":
                    continue
                if n not in (1, 150, 300):
                    continue
                p99s = [r["p99"] for a in arms for r in data.get((a, wl, n), [])]
                C.X_AXIS_MAX_MS = max(p99s) * 1.5 if p99s else 1000
                C.draw_compare(vlog, os.path.join(figs, f"latency_cdf_arms_c{n}.pdf"),
                               dc_label=f"{title} - {os.path.basename(rd)}", conc=n)

        # ---- median-of-rounds figure, Jetpack's style ----
        fig, ax = plt.subplots(figsize=(10, 6.5))
        for (label, color, marker, ls, proto, _m), arm in zip(g_lines, arms):
            xs, ys = [], []
            for wl, n in points:
                rows = data.get((arm, wl, n))
                if wl == "rw_1000000" and rows:
                    xs.append(statistics.median(r["tput"] for r in rows))
                    ys.append(statistics.median(r["p90"] for r in rows))
            ax.plot(xs, ys, label=label, color=color, marker=marker, linestyle=ls,
                    linewidth=G.LINE_WIDTH, ms=G.MARKER_SIZE)
        ax.set_xlabel("Throughput (ops/s)", fontsize=G.XLABEL_FONT_SIZE)
        ax.set_ylabel(G.LATENCY_METRICS["p90"][1], fontsize=G.YLABEL_FONT_SIZE)
        ax.grid(True, linestyle="--", alpha=0.5)
        ax.set_xlim(left=0)
        ax.set_title(f"{title} - median of {len(rounds)} rounds", fontsize=16)
        ax.legend(fontsize=G.LEGEND_FONT_SIZE, loc="upper left", framealpha=0.95)
        fig.tight_layout()
        G.savefig_all(fig, os.path.join(setting_dir, "tput_p90_arms_median.pdf"), bbox_inches="tight")
        plt.close(fig)

        # ---- tables ----
        out = [f"# Jetpack-format comparison: {setting}", "",
               f"Network delay: **{title}**. Rounds: {len(rounds)}, arms rotated. "
               f"Values are the median over rounds with (min..max). Throughput is Jetpack's "
               f"windowed throughput (commits in 15-25 s, gen_tput_p90_figures.aggregate); "
               f"latency is the median over the 10 host groups of each .res p50/p90/p99 "
               f"(ms), requests dispatched in the middle 10 s.", ""]
        hdr = "| delay | workload | N | arm | offered r/s | throughput r/s | p50 ms | p90 ms | p99 ms |"
        out += [hdr, "|" + "---|" * 9]
        summary = {"setting": setting, "delay_label": title, "wan_delay_ms": delay, "rounds": len(rounds), "points": []}
        for wl, n in points:
            for arm in arms:
                rows = data.get((arm, wl, n), [])
                out.append(f"| {delay if delay is not None else '?'} ms | {wl} | {n} | {labels.get(arm, arm)} | "
                           f"{fmt_spread([r['offered'] for r in rows], 0)} | "
                           f"{fmt_spread([r['tput'] for r in rows], 0)} | "
                           + " | ".join(fmt_spread([r[m] for r in rows], 2) for m in METRICS) + " |")
                summary["points"].append({"arm": arm, "workload": wl, "n": n, "wan_delay_ms": delay, "rounds": rows})
        out += ["", f"## Paired ratio to {args.baseline} (per round, then median (min..max) over rounds)", "",
                "| delay | workload | N | arm | throughput | p50 | p90 | p99 |", "|" + "---|" * 8]
        for wl, n in points:
            base = {r["round"]: r for r in data.get((args.baseline, wl, n), [])}
            for arm in arms:
                if arm == args.baseline:
                    continue
                rows = [r for r in data.get((arm, wl, n), []) if r["round"] in base]
                cells = []
                for k in ["tput"] + METRICS:
                    ratios = [r[k] / base[r["round"]][k] for r in rows if base[r["round"]][k]]
                    cells.append(fmt_spread(ratios, 3))
                out.append(f"| {delay if delay is not None else '?'} ms | {wl} | {n} | {labels.get(arm, arm)} | " + " | ".join(cells) + " |")
        out += ["", "## Knee (derive_fixed_conc.find_latency_envelope_conc: largest N with p50 <= 2x the N=1 p50)", "",
                "| delay | workload | arm | knee N per round |", "|---|---|---|---|"]
        for (arm, wl), ks in sorted(knees.items()):
            out.append(f"| {delay if delay is not None else '?'} ms | {wl} | {labels.get(arm, arm)} | {', '.join(map(str, ks))} |")
        summary["knees"] = {f"{a}/{w}": k for (a, w), k in knees.items()}
        out += ["", "Figures: `round<k>/view/figs/` (Jetpack's draw_compare_figure and "
                "draw_compare) and `tput_p90_arms_median.pdf`.", ""]
        with open(os.path.join(setting_dir, "summary.md"), "w") as f:
            f.write("\n".join(out))
        with open(os.path.join(setting_dir, "summary.json"), "w") as f:
            json.dump(summary, f, indent=1)
        print("\n".join(out))


if __name__ == "__main__":
    main()
