#!/usr/bin/env python3
"""Tests scripts/raft_kill/check.py: a good synthetic run passes, and each
rule, broken once, fails it."""
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check  # noqa: E402

GOOD = {
    "localhost": ["start localhost 0 1", "ready", "role leader", "propose localhost.0.0",
                  "apply leader localhost.0.0", "ack localhost.0.0", "propose localhost.0.1",
                  "apply leader localhost.0.1", "ack localhost.0.1", "stopping", "stopped"],
    "p1": ["start p1 0 2", "ready", "apply follower localhost.0.0",
           "start p1 1 3", "ready", "apply follower localhost.0.0", "apply follower localhost.0.1",
           "stopping", "stopped"],
    "p2": ["start p2 0 4", "ready", "apply follower localhost.0.0", "apply follower localhost.0.1",
           "stopping", "stopped"],
}
OK_LOG = "[DISK-VERIFY] site=1 ok records=3 last=2 commit=2\n"


def build(events, logs=None, creating=False):
    d = Path(tempfile.mkdtemp(prefix="raft-kill-check-"))
    (d / "events").mkdir()
    (d / "logs").mkdir()
    for proc, lines in events.items():
        (d / "events" / f"{proc}.events").write_text("\n".join(lines) + "\n")
        incs = [int(l.split()[2]) for l in lines if l.startswith("start")]
        for inc in incs:
            text = (logs or {}).get(f"{proc}.{inc}", OK_LOG)
            (d / "logs" / f"{proc}.{inc}.log").write_text(text)
    if creating:
        (d / "1-0.creating").mkdir()
    return d


def main():
    fails = 0

    def expect(name, d, rule):
        nonlocal fails
        problems, _ = check.check(d)
        ok = (not problems) if rule is None else any(p.startswith(f"({rule})") for p in problems)
        fails += not ok
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {problems}"))

    expect("a good run passes", build(GOOD), None)
    bad = {k: list(v) for k, v in GOOD.items()}
    bad["p2"] = ["start p2 0 4", "apply follower localhost.0.0"]
    expect("(1) a launch that never became ready", build(bad), 1)
    crashed = build(bad, logs={"p2.0": "crash wal.sync.done\n"})
    problems, _ = check.check(crashed)
    print(("ok   " if not any(p.startswith("(1)") for p in problems) else "FAIL ")
          + "(1) excused by its own crash point")
    bad = {k: list(v) for k, v in GOOD.items()}
    bad["p2"][3] = "apply follower localhost.0.9"
    expect("(2) a diverging apply sequence", build(bad), 2)
    bad = {k: list(v) for k, v in GOOD.items()}
    bad["p2"].insert(3, "apply follower localhost.0.0")
    expect("(2) an id applied twice", build(bad), 2)
    bad = {k: list(v) for k, v in GOOD.items()}
    bad["localhost"].insert(9, "ack localhost.0.7")
    expect("(3) an acknowledged id not in the history", build(bad), 3)
    bad = {k: list(v) for k, v in GOOD.items()}
    bad["p1"] = bad["p1"][:6] + ["stopping", "stopped"]
    expect("(3) a last incarnation missing an ack", build(bad), 3)
    expect("(4) a mismatch", build(GOOD, logs={"p1.1": "[DISK-VERIFY] site=2 MISMATCH: x\n"}), 4)
    expect("(4) a clean stop without its verify line", build(GOOD, logs={"p2.0": "nothing\n"}), 4)
    expect("(5) a .creating left behind", build(GOOD, creating=True), 5)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
