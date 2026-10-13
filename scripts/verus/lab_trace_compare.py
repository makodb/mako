#!/usr/bin/env python3
"""Lab equivalence comparator, modulo timing (docs/verus/modification-plan.md A.4).

    lab_trace_compare.py run LAB_DIR OUT_LOG
        Run one full lab suite from LAB_DIR (a ci.sh lab tree such as
        build_rust_raftlab) with MAKO_RAFT_LAB_COMMIT_LOG=1, from the source
        tree that LAB_DIR belongs to, writing stdout+stderr to OUT_LOG.

    lab_trace_compare.py calibrate --out EXEMPTIONS.json LOG [LOG ...]
        Same-build runs: every field that differs between them is exempt for
        that case. Writes the machine-readable list (the reasons go in
        docs/verus/lab-compare-exemptions.md, by hand).

    lab_trace_compare.py compare [--exemptions EXEMPTIONS.json] LOG... -- LOG...
        Parent runs before "--", child runs after. Prints one row per case and
        exits 1 on any difference in a non-exempt field. With no "--", checks
        the runs against each other (the validation step).

What one log yields, per case (a case runs from "TEST <n>: ..." to
"TEST <n> Passed" / "TEST <n> Failed: ..."):

  verdict      Passed / Failed / missing             never exempt
  agreement    no two replicas committed different   never exempt
               (term, payload) at one index
  leaders      the (term, leader) sequence of LABLEADER lines, repeats
               collapsed
  log          the committed log, union over replicas, as (index, term, payload)
  log_values   the same without terms: (index, payload)
  log_length   the highest committed index
  new_payloads the payloads at indices above the previous case's log_length,
               sorted (order-insensitive), i.e. what this case committed
  new_count    how many such entries

The log fields are cumulative (each case dumps the whole committed table), so
one timing-dependent case would otherwise make every later case's log differ;
the two new_* fields stay comparable after it.

Run-level: the "ALL TESTS PASSED" marker, never exempt.
"""
import json
import os
import re
import subprocess
import sys

CASE_START = re.compile(r"^TEST (\d+): (.*)$")
CASE_END = re.compile(r"^TEST (\d+) (Passed|Failed)")
LEADER = re.compile(r"^LABLEADER (\d+) (-?\d+)$")
COMMIT = re.compile(r"^LABCOMMIT (\d+) (\d+) (\S+) (-?\d+)$")
FIELDS = ("leaders", "log", "log_values", "log_length", "new_payloads", "new_count")
NEVER_EXEMPT = ("verdict", "agreement")


def parse(path):
    cases = {}
    cur = None
    with open(path, errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            m = CASE_START.match(line)
            if m:
                cur = int(m.group(1))
                cases[cur] = {"desc": m.group(2), "verdict": "missing",
                              "leaders": [], "commits": []}
                continue
            m = CASE_END.match(line)
            if m:
                cid = int(m.group(1))
                cases.setdefault(cid, {"desc": "", "verdict": "missing", "leaders": [], "commits": []})
                cases[cid]["verdict"] = m.group(2)
                continue
            if cur is None:
                continue
            m = LEADER.match(line)
            if m:
                obs = (int(m.group(1)), int(m.group(2)))
                if not cases[cur]["leaders"] or cases[cur]["leaders"][-1] != obs:
                    cases[cur]["leaders"].append(obs)
                continue
            m = COMMIT.match(line)
            if m:
                cases[cur]["commits"].append((int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4))))
    out = {}
    prev_len = 0
    for cid, c in cases.items():
        by_index = {}
        disagree = []
        for node, idx, term, payload in c["commits"]:
            prev = by_index.get(idx)
            val = (term, payload)
            if prev is None:
                by_index[idx] = val
            elif prev != val:
                # A term that compaction hid ("-") is not a disagreement by itself.
                if prev[1] != payload or (prev[0] != "-" and term != "-" and prev[0] != term):
                    disagree.append((idx, prev, val, node))
                elif prev[0] == "-":
                    by_index[idx] = val
        out[cid] = {
            "desc": c["desc"],
            "verdict": c["verdict"],
            "agreement": "ok" if not disagree else f"DISAGREE {disagree[:3]}",
            "leaders": c["leaders"],
            "log": sorted((i, t, p) for i, (t, p) in by_index.items()),
            "log_values": sorted((i, p) for i, (_t, p) in by_index.items()),
            "log_length": max(by_index) if by_index else 0,
            "new_payloads": sorted(p for i, (_t, p) in by_index.items() if i > prev_len),
            "new_count": sum(1 for i in by_index if i > prev_len),
        }
        if by_index:
            prev_len = max(prev_len, max(by_index))
    all_passed = False
    with open(path, errors="replace") as f:
        all_passed = any(line.startswith("ALL TESTS PASSED") for line in f)
    return {"cases": out, "all_passed": all_passed}


def cmd_run(lab_dir, out_log):
    lab_dir = os.path.realpath(lab_dir)
    src_root = os.path.dirname(lab_dir)
    binary = os.path.join(lab_dir, "deptran_server")
    env = dict(os.environ, MAKO_RAFT_LAB_COMMIT_LOG="1")
    os.makedirs(os.path.dirname(os.path.abspath(out_log)), exist_ok=True)
    with open(out_log, "w") as f:
        p = subprocess.run(["timeout", "1800", binary, "-f", "config/raft_lab_test.yml", "-P", "localhost"],
                           cwd=src_root, env=env, stdout=f, stderr=subprocess.STDOUT)
    parsed = parse(out_log)
    passed = sum(1 for c in parsed["cases"].values() if c["verdict"] == "Passed")
    print(f"{out_log}: exit {p.returncode}, {passed}/{len(parsed['cases'])} cases passed, "
          f"ALL TESTS PASSED={'yes' if parsed['all_passed'] else 'no'}")
    return 0 if p.returncode == 0 and parsed["all_passed"] else 1


def safety_problems(name, parsed):
    probs = []
    if not parsed["all_passed"]:
        probs.append(f"{name}: no ALL TESTS PASSED")
    for cid, c in sorted(parsed["cases"].items()):
        if c["verdict"] != "Passed":
            probs.append(f"{name}: case {cid} verdict {c['verdict']}")
        if c["agreement"] != "ok":
            probs.append(f"{name}: case {cid} {c['agreement']}")
    return probs


def varying_fields(runs):
    """case -> fields whose value is not identical across the parsed runs."""
    ids = sorted(set().union(*(r["cases"].keys() for r in runs)))
    out = {}
    for cid in ids:
        vals = [r["cases"].get(cid) for r in runs]
        out[cid] = [f for f in FIELDS
                    if any(v is None for v in vals) or len({json.dumps(v[f]) for v in vals}) > 1]
    return out


def cmd_calibrate(out_path, logs):
    runs = [parse(p) for p in logs]
    probs = [x for p, r in zip(logs, runs) for x in safety_problems(os.path.basename(p), r)]
    for x in probs:
        print("SAFETY", x)
    vary = varying_fields(runs)
    exemptions = {str(cid): fields for cid, fields in vary.items() if fields}
    with open(out_path, "w") as f:
        json.dump({"runs": [os.path.abspath(p) for p in logs], "exempt": exemptions}, f, indent=1, sort_keys=True)
    n = len(vary)
    leaders = sum(1 for fs in vary.values() if "leaders" in fs)
    print(f"{len(logs)} runs, {n} cases; cases exempting each field:")
    for fld in FIELDS:
        k = sum(1 for fs in vary.values() if fld in fs)
        print(f"  {fld:11s} {k:3d}/{n}")
    for cid, fs in sorted(vary.items()):
        desc = runs[0]["cases"].get(cid, {}).get("desc", "")
        print(f"  case {cid:3d} {desc[:48]:48s} exempt: {', '.join(fs) if fs else '-'}")
    if leaders * 2 > n:
        print(f"STOP (plan 0.7 point 8): {leaders}/{n} cases exempt the (term, leader) sequence")
    return 1 if probs else 0


def cmd_compare(exemptions_path, parent_logs, child_logs):
    exempt = {}
    if exemptions_path:
        with open(exemptions_path) as f:
            exempt = {int(k): set(v) for k, v in json.load(f)["exempt"].items()}
    parent = [parse(p) for p in parent_logs]
    child = [parse(p) for p in child_logs] if child_logs else []
    runs = parent + child
    names = [os.path.basename(p) for p in parent_logs + (child_logs or [])]
    bad = False
    for name, r in zip(names, runs):
        for x in safety_problems(name, r):
            print("FAIL", x)
            bad = True
    vary = varying_fields(runs)
    print(f"{len(parent)} parent run(s), {len(child)} child run(s)")
    for cid, fields in sorted(vary.items()):
        new = [f for f in fields if f not in exempt.get(cid, set())]
        desc = runs[0]["cases"].get(cid, {}).get("desc", "")
        status = "equal" if not new else "DIFFER " + ",".join(new)
        if new:
            bad = True
        print(f"  case {cid:3d} {desc[:48]:48s} {status}"
              + (f"  (exempt: {','.join(sorted(exempt[cid]))})" if exempt.get(cid) else ""))
    print("RESULT:", "differ" if bad else "equal")
    return 1 if bad else 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "run" and len(argv) == 4:
        return cmd_run(argv[2], argv[3])
    if cmd == "calibrate" and len(argv) >= 5 and argv[2] == "--out":
        return cmd_calibrate(argv[3], argv[4:])
    if cmd == "compare":
        args = argv[2:]
        ex = None
        if args[:1] == ["--exemptions"]:
            ex, args = args[1], args[2:]
        if "--" in args:
            i = args.index("--")
            return cmd_compare(ex, args[:i], args[i + 1:])
        return cmd_compare(ex, args, [])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
