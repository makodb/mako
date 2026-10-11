#!/usr/bin/env python3
"""
Derive fixed concurrency values from experiment 0 results.

For each protocol family, finds the largest concurrency level that still
preserves the minimum-concurrency latency envelope in the original
(vanilla) mode.  The selected point is the largest concurrency whose
median (p50) latency does not exceed ``LATENCY_MULTIPLIER`` times the
baseline (lowest-concurrency) p50.

Saves results to:
  - results/fixed_conc.json (machine-readable)
  - <result_dir>/fixed_conc_selection.md (human-readable)

Usage:
    python3 scripts/derive_fixed_conc.py <result_dir>

Example:
    python3 scripts/derive_fixed_conc.py results/2026-03-23-10:26:07-zoo-5machines
"""

import os
import re
import json
import sys
from collections import defaultdict

from res_file_utils import read_res_tail

# A fixed concurrency is "in the same latency class" when its p50 is no
# more than LATENCY_MULTIPLIER × the baseline (lowest-concurrency) p50.
LATENCY_MULTIPLIER = 2.0


def parse_latency_p50(filepath):
    """Extract the p50 latency (ms) from *All-original-path-attempts*.

    Returns None when the file is missing, empty, or has no matching line.
    Falls back to *All-efficient-attempts* when the original-path line is
    absent (e.g. fast-path-only modes).
    """
    for line in read_res_tail(filepath):
        if 'All-original-path-attempts' in line and 'statistics' in line:
            m = re.search(r'50pct\s+([\d.]+)', line)
            if m:
                val = float(m.group(1))
                return val if val > 0 else None
        if 'All-efficient-attempts' in line and 'statistics' in line:
            m = re.search(r'50pct\s+([\d.]+)', line)
            if m:
                val = float(m.group(1))
                return val if val > 0 else None
    return None


def parse_mid_throughput(filepath):
    """Extract throughput from a .res file.

    Prefers 'Mid throughput' (steady-state measurement). Falls back to
    'Total throughtput' when Mid is unavailable — some protocols (e.g.
    MongoDB) produce only the total line in shorter runs.
    """
    mid_tp = None
    total_tp = None
    for line in read_res_tail(filepath):
        if mid_tp is None:
            m = re.search(r'Mid throughput is ([\d.]+)', line)
            if m:
                mid_tp = float(m.group(1))
        if total_tp is None:
            m2 = re.search(r'Total throughtput is ([\d.]+)', line)
            if m2:
                total_tp = float(m2.group(1))
    return mid_tp if mid_tp is not None else total_tp


def collect_throughputs(result_dir, site="30c1s5r5p-zoo", servers=None):
    """
    Scan result_dir for .res files and compute total throughput per
    (protocol, workload, concurrent, mode, ycsb) tuple.

    Returns:
        dict: {protocol: {concurrent: {mode: total_throughput}}}
    """
    if servers is None:
        servers = [f"zoo{i}" for i in range(5)]

    # Pattern: <protocol>-<site>-<workload>-<concurrent>-<mode>-<ycsb>-<server>.res
    pattern = re.compile(
        r'^(.+?)-' +                    # protocol
        re.escape(site) + r'-' +        # site
        r'(rw_\d+)-' +                  # workload
        r'(concurrent_\d+)-' +          # concurrent
        r'(\d+)-' +                     # mode
        r'(YCSB_[A-Z])-' +             # ycsb
        r'(.+)\.res$'                   # server
    )

    # Accumulate throughputs
    # key = (protocol, workload, concurrent, mode, ycsb)
    throughput_parts = defaultdict(dict)

    for fname in os.listdir(result_dir):
        m = pattern.match(fname)
        if not m:
            continue
        protocol, workload, concurrent, mode, ycsb, server = m.groups()
        if server not in servers:
            continue

        filepath = os.path.join(result_dir, fname)
        tp = parse_mid_throughput(filepath)
        if tp is None:
            continue

        key = (protocol, workload, concurrent, mode, ycsb)
        throughput_parts[key][server] = tp

    # Sum per-server throughputs to get total
    result = defaultdict(lambda: defaultdict(dict))
    for (protocol, workload, concurrent, mode, ycsb), server_tps in throughput_parts.items():
        if len(server_tps) < len(servers):
            # Incomplete — not all servers reported
            continue
        total_tp = sum(server_tps.values())
        result[protocol][concurrent][mode] = total_tp

    return result


def collect_latencies(result_dir, site="30c1s5r5p-zoo", servers=None):
    """Scan result_dir for .res files and compute **average p50 latency**
    per (protocol, concurrent, mode) tuple.

    Returns:
        dict: {protocol: {concurrent: {mode: avg_p50_latency}}}
    """
    if servers is None:
        servers = [f"zoo{i}" for i in range(5)]

    pattern = re.compile(
        r'^(.+?)-' +
        re.escape(site) + r'-' +
        r'(rw_\d+)-' +
        r'(concurrent_\d+)-' +
        r'(\d+)-' +
        r'(YCSB_[A-Z])-' +
        r'(.+)\.res$'
    )

    latency_parts = defaultdict(dict)

    for fname in os.listdir(result_dir):
        m = pattern.match(fname)
        if not m:
            continue
        protocol, workload, concurrent, mode, ycsb, server = m.groups()
        if server not in servers:
            continue

        filepath = os.path.join(result_dir, fname)
        p50 = parse_latency_p50(filepath)
        if p50 is None:
            continue

        key = (protocol, workload, concurrent, mode, ycsb)
        latency_parts[key][server] = p50

    result = defaultdict(lambda: defaultdict(dict))
    for (protocol, workload, concurrent, mode, ycsb), server_lats in latency_parts.items():
        if len(server_lats) < len(servers):
            continue
        avg_p50 = sum(server_lats.values()) / len(server_lats)
        result[protocol][concurrent][mode] = avg_p50

    return result


def find_max_throughput_conc(protocol_data, mode="0"):
    """
    Given {concurrent: {mode: throughput}}, find the concurrent that
    maximizes throughput for the given mode.

    Returns (best_conc, best_throughput, all_results)
    """
    best_conc = None
    best_tp = -1
    all_results = []

    for conc, mode_data in sorted(protocol_data.items(),
                                   key=lambda x: int(x[0].split('_')[1])):
        tp = mode_data.get(mode, 0)
        all_results.append((conc, tp))
        if tp > best_tp:
            best_tp = tp
            best_conc = conc

    return best_conc, best_tp, all_results


def find_latency_envelope_conc(throughput_data, latency_data, mode="0",
                                multiplier=None):
    """Select the largest concurrency that preserves the low-concurrency
    latency envelope.

    Algorithm:
      1. Sort concurrencies numerically.
      2. Baseline = p50 at the **lowest** concurrency with both throughput
         and latency data for *mode*.
      3. Threshold = baseline × *multiplier*.
      4. Walk concurrencies **high → low**; the first one whose p50 ≤
         threshold **and** whose throughput > 0 is selected.

    Returns:
        (selected_conc, baseline_p50, selected_p50, max_throughput,
         all_results)

    *all_results* is a list of ``(conc, throughput, p50)`` tuples sorted
    by concurrency (ascending).  Missing data is represented as ``None``.
    """
    if multiplier is None:
        multiplier = LATENCY_MULTIPLIER

    sorted_concs = sorted(
        set(list(throughput_data.keys()) + list(latency_data.keys())),
        key=lambda x: int(x.split('_')[1]) if isinstance(x, str) and '_' in x else 0,
    )

    # Build merged view: (conc, tp, p50)
    all_results = []
    for conc in sorted_concs:
        tp = throughput_data.get(conc, {}).get(mode)
        p50 = latency_data.get(conc, {}).get(mode)
        all_results.append((conc, tp, p50))

    # Find baseline p50 (lowest concurrency with valid data)
    baseline_p50 = None
    for conc, tp, p50 in all_results:
        if p50 is not None and p50 > 0 and tp is not None and tp > 0:
            baseline_p50 = p50
            break

    if baseline_p50 is None:
        return None, None, None, None, all_results

    threshold = baseline_p50 * multiplier

    # Walk high → low to find largest conc in envelope
    selected_conc = None
    selected_p50 = None
    selected_tp = None
    for conc, tp, p50 in reversed(all_results):
        if (p50 is not None and p50 > 0 and
                tp is not None and tp > 0 and
                p50 <= threshold):
            selected_conc = conc
            selected_p50 = p50
            selected_tp = tp
            break

    return selected_conc, baseline_p50, selected_p50, selected_tp, all_results


# Protocol families: vanilla protocol name -> display name
PROTOCOL_MAP = {
    "none_raft": "Raft",
    "none_copilot": "Copilot",
    "none_mencius": "Mencius",
    "none_mongodb": "MongoDB",
    "none_etcd": "etcd",
    "none_zookeeper": "ZooKeeper",
}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    result_dir = sys.argv[1]
    if not os.path.isdir(result_dir):
        print(f"Error: {result_dir} is not a directory")
        sys.exit(1)

    print(f"Scanning {result_dir}...")
    tp_data = collect_throughputs(result_dir)
    lat_data = collect_latencies(result_dir)

    if not tp_data:
        print("No valid results found. Is the experiment still running?")
        sys.exit(1)

    # Find fixed conc for each protocol using latency-envelope rule
    fixed_conc = {}
    selection_details = []

    for vanilla_proto, display_name in PROTOCOL_MAP.items():
        if vanilla_proto not in tp_data:
            print(f"  {display_name}: no throughput data yet (skipping)")
            continue

        proto_tp = tp_data[vanilla_proto]
        proto_lat = lat_data.get(vanilla_proto, {})

        selected_conc, baseline_p50, sel_p50, sel_tp, all_results = \
            find_latency_envelope_conc(proto_tp, proto_lat, mode="0")

        if selected_conc is None:
            # Fallback: if no latency data, use peak throughput
            best_conc, best_tp, old_results = find_max_throughput_conc(
                proto_tp, mode="0"
            )
            if best_conc is None:
                print(f"  {display_name}: no mode=0 (original) data")
                continue
            selected_conc = best_conc
            sel_tp = best_tp
            baseline_p50 = None
            sel_p50 = None
            all_results = [(c, t, None) for c, t in old_results]
            print(f"  {display_name}: {selected_conc} (tp={best_tp:.2f}) "
                  f"[fallback: no latency data]")
        else:
            print(f"  {display_name}: {selected_conc} "
                  f"(tp={sel_tp:.2f}, p50={sel_p50:.2f}ms, "
                  f"baseline_p50={baseline_p50:.2f}ms, "
                  f"threshold={baseline_p50 * LATENCY_MULTIPLIER:.2f}ms)")

        rule_proto = vanilla_proto.replace("none_", "rule_")
        fixed_conc[vanilla_proto] = selected_conc
        fixed_conc[rule_proto] = selected_conc
        fixed_conc[display_name] = selected_conc

        detail = {
            "protocol": display_name,
            "vanilla_key": vanilla_proto,
            "fixed_conc": selected_conc,
            "throughput": round(sel_tp, 2) if sel_tp else None,
            "baseline_p50": round(baseline_p50, 2) if baseline_p50 else None,
            "selected_p50": round(sel_p50, 2) if sel_p50 else None,
            "threshold": round(baseline_p50 * LATENCY_MULTIPLIER, 2) if baseline_p50 else None,
            "all_concs": [
                (c, round(t, 2) if t else None, round(p, 2) if p else None)
                for c, t, p in all_results
            ],
        }
        selection_details.append(detail)

    if not fixed_conc:
        print("No fixed concurrencies could be derived. Experiment may still be running.")
        sys.exit(1)

    # Save machine-readable JSON
    json_path = os.path.join(os.path.dirname(result_dir) if "/" in result_dir else ".",
                              "results", "fixed_conc.json")
    # If result_dir is already under results/, save alongside it
    if "results/" in result_dir or "results\\" in result_dir:
        json_path = os.path.join(os.path.dirname(result_dir), "fixed_conc.json")

    os.makedirs(os.path.dirname(json_path), exist_ok=True)
    with open(json_path, 'w') as f:
        json.dump(fixed_conc, f, indent=2)
    print(f"\nSaved: {json_path}")

    # Save human-readable markdown in the result folder
    md_path = os.path.join(result_dir, "fixed_conc_selection.md")
    with open(md_path, 'w') as f:
        f.write("# Fixed Concurrency Selection\n\n")
        f.write("Derived from experiment 0 (throughput-latency sweep).\n\n")
        f.write("**Selection rule:** largest concurrency whose p50 latency "
                "stays within\n")
        f.write(f"{LATENCY_MULTIPLIER:.1f}× the baseline (lowest-concurrency) "
                "p50, using mode=0\n")
        f.write("(original / vanilla) data only.\n\n")

        f.write("## Summary\n\n")
        f.write("| Protocol   | Fixed Conc        | Throughput (txn/s) "
                "| Baseline p50 (ms) | Selected p50 (ms) "
                "| Threshold (ms) |\n")
        f.write("|------------|-------------------|--------------------"
                "|-------------------|-------------------"
                "|----------------|\n")
        for d in selection_details:
            tp_str = f"{d['throughput']:>18.2f}" if d['throughput'] else "             N/A"
            bp_str = f"{d['baseline_p50']:>17.2f}" if d['baseline_p50'] else "            N/A"
            sp_str = f"{d['selected_p50']:>17.2f}" if d['selected_p50'] else "            N/A"
            th_str = f"{d['threshold']:>14.2f}" if d['threshold'] else "           N/A"
            f.write(f"| {d['protocol']:<10} | {d['fixed_conc']:<17} "
                    f"| {tp_str} | {bp_str} | {sp_str} | {th_str} |\n")

        f.write("\n## Per-Protocol Sweep Data\n\n")
        for d in selection_details:
            f.write(f"### {d['protocol']} ({d['vanilla_key']})\n\n")
            if d['baseline_p50']:
                f.write(f"Baseline p50: **{d['baseline_p50']}** ms  \n")
                f.write(f"Threshold ({LATENCY_MULTIPLIER:.1f}×): "
                        f"**{d['threshold']}** ms  \n")
            f.write(f"Selected: **{d['fixed_conc']}**")
            if d['throughput']:
                f.write(f" (throughput={d['throughput']} txn/s")
                if d['selected_p50']:
                    f.write(f", p50={d['selected_p50']} ms")
                f.write(")")
            f.write("\n\n")
            f.write("| Concurrency | Throughput (txn/s) | p50 (ms) | In envelope? |\n")
            f.write("|-------------|--------------------|----------|--------------|\n")
            for conc, tp, p50 in d['all_concs']:
                tp_s = f"{tp:>18.2f}" if tp else "               N/A"
                p50_s = f"{p50:>8.2f}" if p50 else "     N/A"
                if p50 and d['threshold'] and p50 > 0:
                    env = "yes" if p50 <= d['threshold'] else "NO"
                else:
                    env = "N/A"
                marker = " **<--**" if conc == d['fixed_conc'] else ""
                f.write(f"| {conc:<11} | {tp_s} | {p50_s} | {env:<12} |{marker}\n")
            f.write("\n")

    print(f"Saved: {md_path}")


if __name__ == "__main__":
    main()
