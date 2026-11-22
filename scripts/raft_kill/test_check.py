#!/usr/bin/env python3
"""Tests scripts/raft_kill/check.py: a good synthetic run passes, and each
rule, broken once, fails it. The evidence is in the formats the programs
write (raft_kill_node's events, the replay recorder's settle records, the
shell's reveal lines)."""
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check  # noqa: E402

GOOD = {
    "localhost": ["start localhost 0 1 create", "ready", "role leader", "propose localhost.0.0",
                  "apply leader localhost.0.0", "ack localhost.0.0", "propose localhost.0.1",
                  "apply leader localhost.0.1", "ack localhost.0.1", "stopping", "stopped"],
    "p1": ["start p1 0 2 create", "ready", "apply follower localhost.0.0",
           "start p1 1 3", "ready", "apply follower localhost.0.0", "apply follower localhost.0.1",
           "stopping", "stopped"],
    "p2": ["start p2 0 4 create", "ready", "apply follower localhost.0.0", "apply follower localhost.0.1",
           "stopping", "stopped"],
}
OK_LOG = "[DISK-VERIFY] site=1 ok records=3 last=2 commit=2\n"


def settle(term, cand, grants, won):
    """A `settle` record as the recorder writes it: the event, the log level,
    then the actions (a win sets the role and appends the no-op) and the
    reply."""
    votes = " ".join(f"{v} {int(g)} {term}" for v, g in grants.items())
    actions = "A 6 0 0 0 2 0 1 1 0 A 1 0 0 0 2 0 1 1 0 " if won else ""
    return f"E settle {term} {cand} 3 0 0 1 0 0 {len(grants)} {votes} | 2 | {actions}R settled {int(won)}"


REPLAY = {"1.0": [settle(2, 1, {2: True, 3: False}, True)],
          "2.0": [settle(3, 2, {1: True, 3: True}, True)]}
REVEALS = {"2": ["created", "reveal vote 2 1 - - 5", "reveal ack 2 - - 7 9",
                 "recovered 2 1 4 7 9", "reveal ack 3 - - 8 12", "recovered 4 65535 4 6 12"],
           "1": ["created", "reveal append 2 - 4 - 6", "reveal campaign 2 1 - - 3",
                 "recovered 2 1 4 7 6"]}
DIRS = []


def build(events, logs=None, creating=False, replay=None, reveals=None, segments=0):
    d = Path(tempfile.mkdtemp(prefix="raft-kill-check-"))
    DIRS.append(d)
    (d / "events").mkdir()
    (d / "logs").mkdir()
    if replay is not False:
        (d / "replay").mkdir()
        for name, lines in (REPLAY if replay is None else replay).items():
            (d / "replay" / f"{name}.rec").write_text("\n".join(lines) + "\n")
    if reveals is not False:
        (d / "evidence").mkdir()
        for site, lines in (REVEALS if reveals is None else reveals).items():
            (d / "evidence" / f"{site}.reveal").write_text("\n".join(lines) + "\n")
    for proc, lines in events.items():
        (d / "events" / f"{proc}.events").write_text("\n".join(lines) + "\n")
        incs = [int(l.split()[2]) for l in lines if l.startswith("start")]
        for inc in incs:
            text = (logs or {}).get(f"{proc}.{inc}", OK_LOG)
            (d / "logs" / f"{proc}.{inc}.log").write_text(text)
    if creating:
        (d / "1-0.creating").mkdir()
    if segments:
        (d / "1-0" / "wal").mkdir(parents=True)
        for k in range(segments):
            (d / "1-0" / "wal" / f"{k + 1:020}.seg").write_bytes(b"")
    return d


def main():
    fails = 0

    def expect(name, d, rule, absent=False):
        """`rule` None: no problem at all; else a problem of that rule (or,
        with `absent`, none of it)."""
        nonlocal fails
        problems, _ = check.check(d)
        hit = any(p.startswith(f"({rule})") for p in problems)
        ok = (not problems) if rule is None else (not hit if absent else hit)
        fails += not ok
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {problems}"))

    def edit(proc, fn):
        bad = {k: list(v) for k, v in GOOD.items()}
        fn(bad[proc])
        return bad

    expect("a good run passes", build(GOOD), None)
    bad = edit("p2", lambda l: l.__setitem__(slice(None), ["start p2 0 4 create", "apply follower localhost.0.0"]))
    expect("(1) a launch that never became ready", build(bad), 1)
    expect("(1) excused by its own crash point", build(bad, logs={"p2.0": "crash wal.sync.done\n"}), 1, absent=True)
    expect("(2) a diverging apply sequence", build(edit("p2", lambda l: l.__setitem__(3, "apply follower localhost.0.9"))), 2)
    expect("(2) an id applied twice", build(edit("p2", lambda l: l.insert(3, "apply follower localhost.0.0"))), 2)
    expect("(3) an acknowledged id not in the history",
           build(edit("localhost", lambda l: l.insert(9, "ack localhost.0.7"))), 3)
    expect("(3) a last incarnation missing an ack",
           build(edit("p1", lambda l: l.__setitem__(slice(6, None), ["stopping", "stopped"]))), 3)
    expect("(4) a mismatch", build(GOOD, logs={"p1.1": "[DISK-VERIFY] site=2 MISMATCH: x\n"}), 4)
    expect("(4) a clean stop without its verify line", build(GOOD, logs={"p2.0": "nothing\n"}), 4)
    expect("(4) a last incarnation that did not stop", build(edit("p2", lambda l: l.remove("stopped"))), 4)
    expect("(5) a .creating left behind", build(GOOD, creating=True), 5)
    expect("(6) an unbounded WAL", build(GOOD, segments=17), 6)
    two = dict(REPLAY, **{"3.0": [settle(2, 3, {1: False, 2: True}, True)]})
    expect("(7) two leaders in a term", build(GOOD, replay=two), 7)
    expect("(7) no recordings", build(GOOD, replay=False), 7)
    expect("(7) recordings with no won election", build(GOOD, replay={"1.0": [settle(2, 1, {2: False}, False)]}), 7)
    expect("(7) an unparsable settle record", build(GOOD, replay={"1.0": ["E settle 2 x | 2 | R settled 1"]}), 7)
    lost = dict(REPLAY, **{"3.0": [settle(2, 3, {2: True}, False)]})
    expect("(8) a voter granting two candidates in a term", build(GOOD, replay=lost), 8)

    def broken(line):
        return {"2": ["created", "reveal vote 2 1 3 7 9", line]}
    expect("(9) a WAL behind an output's tail", build(GOOD, reveals=broken("recovered 2 1 3 7 8")), 9)
    expect("(9) a term behind one shown", build(GOOD, reveals=broken("recovered 1 1 3 7 9")), 9)
    expect("(9) another vote at the same term", build(GOOD, reveals=broken("recovered 2 3 3 7 9")), 9)
    expect("(9) a commit behind one sent", build(GOOD, reveals=broken("recovered 2 1 2 7 9")), 9)
    expect("(9) a log shorter than an ack at the same term",
           build(GOOD, reveals=broken("recovered 2 1 3 6 9")), 9)
    expect("(9) a store made anew forgets the old one's reveals",
           build(GOOD, reveals={"2": ["reveal vote 5 1 9 9 99", "created", "recovered 0 0 0 0 0"]}), None)
    expect("(9) no evidence directory", build(GOOD, reveals=False), 9)
    expect("(9) an unparsable evidence line", build(GOOD, reveals={"2": ["created", "reveal vote 2"]}), 9)
    expect("(9) a restart that wrote no recovered line", build(GOOD, reveals={"2": ["created"]}), 9)
    for d in DIRS:
        shutil.rmtree(d, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
