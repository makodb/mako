#!/usr/bin/env python3
"""The Verus gate over the Raft core (docs/verus/disk-persistence-plan.md §4).

verify_core.sh runs Verus with --output-json --time-expanded and hands the log
here. The gate fails when:
  * Verus reports an error, or did not verify the whole crate;
  * a function named in scripts/verus/verified_functions.txt is missing from
    Verus's per-function breakdown, or failed (a function verified today stays
    verified unless a commit removes it from the list, with its reason);
  * the core trusts anything beyond scripts/verus/core_trusted.txt, whose
    entries are `<file>:<function>` (relative to the core's src). Trust is
    an outer or inner attribute naming a trusted mode -- external_body,
    external, or an external_{fn,type,trait}_specification /
    external_trait_extension -- however spelled (`verifier::x`,
    `verifier(x)`, inside `cfg_attr(..)`, `verus_verify(x)`); an `axiom fn`;
    assume_specification; and any assume(...) or admit() call (the core's
    own `.admit(` method does not count). Every .rs file under the core's
    src is scanned, subdirectories included. This fixes B26, where only
    `#[verifier::external_body]` followed by a lower-case `fn` within four
    lines was seen, and the spellings a review found to pass (an axiom
    proving false, `cfg_attr(verus_keep_ghost, verifier::external_body)`).

  verus_gate.py check LOG CORE_SRC_DIR LIST TRUSTED
  verus_gate.py write LOG LIST          (regenerate the list from a run)
"""
import json
import re
import sys
from pathlib import Path

TRUST_ATTR = re.compile(
    r"#!?\[[^\]]*?\b(?:verifier\s*(?:::\s*|\(\s*)|verus_verify\s*\(\s*)"
    r"(external_body|external_fn_specification|external_type_specification"
    r"|external_trait_specification|external_trait_extension|external)\b")
AXIOM = re.compile(r"\baxiom\s+fn\s+([A-Za-z_][A-Za-z0-9_]*)")
TRUST_MACRO = re.compile(r"\bassume_specification\b")
ASSUME = re.compile(r"(?<![.\w])assume\s*\(")
ADMIT = re.compile(r"(?<![.\w])admit\s*\(\s*\)")
FN = re.compile(r"\bfn\s+([A-Za-z_][A-Za-z0-9_]*)")


def load_report(log_text):
    """The JSON object Verus printed (the one holding 'verification-results')."""
    dec = json.JSONDecoder()
    i = 0
    while True:
        k = log_text.find("{", i)
        if k < 0:
            raise ValueError("no Verus JSON report in the log")
        try:
            obj, end = dec.raw_decode(log_text, k)
        except ValueError:
            i = k + 1
            continue
        if isinstance(obj, dict) and "verification-results" in obj:
            return obj
        i = end


def functions(report):
    """{name: success} from the per-function SMT breakdown."""
    out = {}
    smt = report.get("times-ms", {}).get("smt", {})
    for mod in smt.get("smt-run-module-times", []):
        for f in mod.get("function-breakdown", []):
            name = f["function"]
            out[name] = out.get(name, True) and bool(f.get("success", True))
    return out


def strip_comments(line):
    # Good enough for the core: no `//` inside string literals there.
    return line.split("//", 1)[0]


def trust_sites(src_dir):
    """[(kind, site)] for every trusted item in the core; a site is
    `<file>:<function>` where a function is named, else `<file>:<line>`."""
    sites = []
    root = Path(src_dir)
    for path in sorted(root.rglob("*.rs")):
        rel = path.relative_to(root).as_posix()
        lines = path.read_text().splitlines()
        for n, raw in enumerate(lines):
            line = strip_comments(raw)
            at = f"{rel}:{n + 1}"
            m = TRUST_ATTR.search(line)
            if m:
                name = None
                for follow in lines[n:n + 8]:
                    f = FN.search(strip_comments(follow))
                    if f:
                        name = f.group(1)
                        break
                sites.append((m.group(1), f"{rel}:{name}" if name else at))
            a = AXIOM.search(line)
            if a:
                sites.append(("axiom", f"{rel}:{a.group(1)}"))
            if TRUST_MACRO.search(line):
                sites.append(("assume_specification", at))
            if ASSUME.search(line):
                sites.append(("assume", at))
            if ADMIT.search(line):
                sites.append(("admit", at))
    return sites


def read_list(path):
    p = Path(path)
    if not p.exists():
        return []
    return [l.split()[0] for l in p.read_text().splitlines() if l.strip() and not l.lstrip().startswith("#")]


def check(log_path, src_dir, list_path, trusted_path):
    problems = []
    report = load_report(Path(log_path).read_text(errors="replace"))
    res = report["verification-results"]
    if not res.get("success") or res.get("errors", 1) != 0 or res.get("encountered-error") \
            or res.get("encountered-vir-error"):
        problems.append(f"Verus did not succeed: {res}")
    if not res.get("is-verifying-entire-crate", False):
        problems.append("Verus did not verify the entire crate")
    funcs = functions(report)
    want = read_list(list_path)
    if not want:
        problems.append(f"{list_path} is empty or missing")
    missing = [w for w in want if w not in funcs]
    failed = [w for w in want if funcs.get(w) is False]
    if missing:
        problems.append(f"{len(missing)} listed function(s) no longer verified: " + ", ".join(missing[:20]))
    if failed:
        problems.append(f"{len(failed)} listed function(s) failed: " + ", ".join(failed[:20]))
    allowed = set(read_list(trusted_path))
    trusted = trust_sites(src_dir)
    extra = [f"{k} {s}" for k, s in trusted if s not in allowed]
    if extra:
        problems.append("trusted but not in core_trusted.txt: " + "; ".join(extra))
    new = sorted(set(funcs) - set(want))
    summary = (f"{res.get('verified')} verified, {res.get('errors')} errors; "
               f"{len(want)} listed functions all verified")
    return problems, summary, sorted(s for _, s in trusted), new


def main(argv):
    if len(argv) >= 2 and argv[1] == "write" and len(argv) == 4:
        report = load_report(Path(argv[2]).read_text(errors="replace"))
        funcs = functions(report)
        bad = [f for f, ok in funcs.items() if not ok]
        if bad:
            print(f"verus_gate: refusing to write a list with failed functions: {bad[:10]}")
            return 1
        header = ("# Functions Verus verifies in the Raft core (src/deptran/raft/core), from\n"
                  "# `verify_core.sh` with --output-json --time-expanded; the gate in\n"
                  "# scripts/verus/verus_gate.py fails if one goes missing or fails. A commit\n"
                  "# that removes a name says why (docs/verus/disk-persistence-plan.md §4).\n")
        Path(argv[3]).write_text(header + "".join(f"{f}\n" for f in sorted(funcs)))
        print(f"verus_gate: wrote {len(funcs)} functions to {argv[3]}")
        return 0
    if len(argv) == 6 and argv[1] == "check":
        problems, summary, trusted, new = check(*argv[2:6])
        if problems:
            for p in problems:
                print(f"verus_gate: FAILED: {p}")
            return 1
        extra = f"; {len(new)} new verified function(s) not yet listed" if new else ""
        print(f"verus_gate: ok ({summary}; trusted: {' '.join(trusted) or 'none'}{extra})")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
