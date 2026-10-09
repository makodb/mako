#!/usr/bin/env python3
"""Tests scripts/verus/verus_gate.py on a real Verus log (argv[1]) and scratch
copies of the core: the gate passes as is, and fails when a listed function
goes missing or fails, when Verus reports an error, and on each spelling of
trust (external_body in both attribute forms and before a capitalised name,
external, assume_specification, assume(...), admit()). Each break is checked
alone, so each must be caught by its own rule."""
import json, shutil, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import verus_gate  # noqa: E402

ROOT = HERE.parent.parent
CORE = ROOT / "src/deptran/raft/core/src"
LIST = HERE / "verified_functions.txt"
TRUSTED = HERE / "core_trusted.txt"


def run(log_text, src):
    with tempfile.TemporaryDirectory() as d:
        lp = Path(d) / "log"
        lp.write_text(log_text)
        problems, _, _, _ = verus_gate.check(str(lp), str(src), str(LIST), str(TRUSTED))
        return problems


def main():
    log = Path(sys.argv[1]).read_text(errors="replace")
    report = verus_gate.load_report(log)
    fails = 0

    def expect(name, problems, should_fail):
        nonlocal fails
        ok = bool(problems) == should_fail
        fails += not ok
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {problems}"))

    expect("the real run passes", run(log, CORE), False)

    def mutated(fn):
        r = json.loads(json.dumps(report))
        fn(r)
        return json.dumps(r)

    first = next(iter(verus_gate.read_list(LIST)))

    def drop(r):
        for m in r["times-ms"]["smt"]["smt-run-module-times"]:
            m["function-breakdown"] = [f for f in m["function-breakdown"] if f["function"] != first]
    expect("a listed function removed", run(mutated(drop), CORE), True)

    def fail(r):
        for m in r["times-ms"]["smt"]["smt-run-module-times"]:
            for f in m["function-breakdown"]:
                if f["function"] == first:
                    f["success"] = False
    expect("a listed function failed", run(mutated(fail), CORE), True)

    def err(r):
        r["verification-results"]["errors"] = 1
        r["verification-results"]["success"] = False
    expect("a Verus error", run(mutated(err), CORE), True)

    breaks = {
        "external_body (::)": "#[verifier::external_body]\npub fn sneaky() {}\n",
        "external_body (paren)": "#[verifier(external_body)]\npub fn sneaky() {}\n",
        "external_body, capitalised name": "#[verifier::external_body]\npub fn Sneaky() {}\n",
        "external": "#[verifier::external]\npub fn sneaky() {}\n",
        "assume_specification": "pub assume_specification[ core::cmp::max ](a: u64, b: u64) -> u64;\n",
        "assume(true)": "proof fn sneaky() { assume(true); }\n",
        "admit()": "proof fn sneaky() { admit(); }\n",
    }
    for name, code in breaks.items():
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "src"
            shutil.copytree(CORE, src)
            (src / "zz_break.rs").write_text(code)
            expect(f"trust: {name}", run(log, src), True)
    # The core's own `.admit(` method and a comment mentioning assume( stay legal.
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / "src"
        shutil.copytree(CORE, src)
        (src / "zz_ok.rs").write_text("fn f(x: &X) { x.admit(1); } // assume(true) in a comment\n")
        expect("method .admit( and comments are not trust", run(log, src), False)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
