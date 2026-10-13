#!/usr/bin/env python3
"""Checks a raft_kill run (docs/verus/disk-persistence.md §5, §8).

Reads <run>/events/<proc>.events (raft_kill_node's lines, all incarnations
of a node appended to one file) and <run>/logs/<proc>.<inc>.log (each
launch's stderr), and requires:

  1. every launch reached `ready`, except one killed by its own armed crash
     point (its log holds `crash <point>`) and one whose refusal run.py
     expected and recorded (a `refused` line: a launch without
     MAKO_RAFT_CREATE onto a store a crashed creation never finished);
  2. every incarnation's apply sequence is a prefix of one history (Raft's
     state machine safety), with no id applied twice in it;
  3. every acknowledged id (a leader applied its own proposal: committed) is
     in that history, and in every node's last incarnation's sequence (they
     were quiesced before the stop);
  4. every node's last incarnation stopped cleanly (`stopped`) and printed
     `[DISK-VERIFY] ... ok`, as did every other launch that stopped, and no
     launch printed a mismatch (deaths run.py did not cause are its own
     failures, reported beside these);
  5. no store holds a `.creating` side directory after the run;
  6. each store's WAL is bounded: checkpoints deleted the segments its base
     covers (at most 16 remain);
  7. one leader per term, and 8. one granted candidate per voter and term,
     from the core's `settle` records (<run>/replay/*.rec, every node's every
     incarnation: a vote a restart forgot shows as a second grant);
  9. every launch onto an existing store left a `recovered` line in
     <run>/evidence/<site>.reveal, and each covers the
     reveals before it (since the store's `created` line): the WAL through
     each output's tail (d >= tail), no term behind one shown, at an equal
     term the same vote, no commit behind one sent, and at an equal term no
     log shorter than an index acknowledged (a newer leader may cut
     uncommitted entries).
Missing or malformed evidence (no recordings, no evidence directory, a line
that does not parse, no settled election) is a failure, never a pass.

  check.py RUN_DIR   -> exit 0 and a summary, or exit 1 and the violations
"""
import os
import re
import sys
from pathlib import Path


def incarnations(events_text):
    """[(inc, [lines], creating)] in file order; a `start` line opens one
    (`start <proc> <inc> <pid> [create]`)."""
    out = []
    for line in events_text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "start":
            out.append((int(parts[2]), [], parts[4:5] == ["create"]))
        elif out:
            out[-1][1].append(parts)
    return out


def check(run):
    run = Path(run)
    problems = []
    history = []        # the longest apply sequence seen
    sequences = {}      # (proc, inc) -> [ids]
    acks = set()
    last_inc = {}
    stopped_at = {}     # (proc, inc) -> stopped cleanly
    restarts = 0        # ready launches onto an existing store
    for ev in sorted((run / "events").glob("*.events")):
        proc = ev.stem
        for inc, lines, creating in incarnations(ev.read_text()):
            # A snapshot install (`install id,...`) replaces the state
            # machine with the image's ids; applies continue after it.
            seq = []
            for l in lines:
                if l[0] == "apply":
                    seq.append(l[2])
                elif l[0] == "install":
                    seq = l[1].split(",") if len(l) > 1 and l[1] else []
            sequences[(proc, inc)] = seq
            acks.update(l[1] for l in lines if l[0] == "ack")
            last_inc[proc] = inc
            log_path = run / "logs" / f"{proc}.{inc}.log"
            log = log_path.read_text(errors="replace") if log_path.exists() else ""
            crashed = re.search(r"^crash (\S+)", log, re.M)
            ready = any(l[0] == "ready" for l in lines)
            refused = any(l[0] == "refused" for l in lines)
            if not ready and not crashed and not refused:
                problems.append(f"(1) {proc}.{inc} never became ready")
            restarts += ready and not creating
            stopped = any(l[0] == "stopped" for l in lines)
            stopped_at[(proc, inc)] = stopped
            if "MISMATCH" in log:
                problems.append(f"(4) {proc}.{inc}: " + next(l for l in log.splitlines() if "MISMATCH" in l))
            if stopped and not re.search(r"^\[DISK-VERIFY\] site=\d+ ok", log, re.M):
                problems.append(f"(4) {proc}.{inc} stopped cleanly without a [DISK-VERIFY] ok line")
            if len(set(seq)) != len(seq):
                problems.append(f"(2) {proc}.{inc} applied an id twice")
            if len(seq) > len(history):
                history = seq
    for (proc, inc), seq in sequences.items():
        if history[:len(seq)] != seq:
            k = next(i for i, (a, b) in enumerate(zip(history, seq)) if a != b) if seq else 0
            problems.append(f"(2) {proc}.{inc} diverges from the history at apply {k + 1}: "
                            f"{seq[k] if k < len(seq) else '-'} vs {history[k] if k < len(history) else '-'}")
    in_history = set(history)
    for a in sorted(acks - in_history):
        problems.append(f"(3) acknowledged {a} is not in the history")
    for proc, inc in last_inc.items():
        if not stopped_at[(proc, inc)]:
            problems.append(f"(4) {proc}.{inc} (last incarnation) did not stop cleanly")
        missing = acks - set(sequences[(proc, inc)])
        if missing:
            problems.append(f"(3) {proc}.{inc} (last incarnation) lacks {len(missing)} acknowledged id(s), "
                            f"e.g. {sorted(missing)[0]}")
    for d, _, _ in os.walk(run):
        if d.endswith(".creating"):
            problems.append(f"(5) {d} left behind")
    for wal in run.glob("*-*/wal"):
        n = len(list(wal.glob("*.seg")))
        if n > 16:
            problems.append(f"(6) {wal} holds {n} segments: the WAL is not bounded")
    elections = check_elections(run, problems)
    recoveries = check_reveals(run, problems)
    if recoveries < restarts:
        problems.append(f"(9) {restarts} launch(es) onto an existing store became ready, "
                        f"but only {recoveries} `recovered` line(s) were written")
    summary = (f"{len(sequences)} incarnation(s) of {len(last_inc)} node(s); history {len(history)} "
               f"applies; {len(acks)} acknowledged; {elections} election(s) settled; "
               f"{recoveries} recover(ies) checked")
    return problems, summary


def check_elections(run, problems):
    """(7) and (8), from `E settle <term> <candidate> <n> <timed_out> <stopped>
    <looping> <failover> <debug> <k> (<voter> <granted> <reply term>)*` records,
    `R settled <won>` at the end."""
    winners, grants, n = {}, {}, 0
    recs = sorted((run / "replay").glob("*.rec"))
    if not recs:
        problems.append("(7) no replay recordings: nothing to check elections against")
    for rec in recs:
        for line in rec.read_text(errors="replace").splitlines():
            if not line.startswith("E settle "):
                continue
            # `<event> | <level> | <A..., L..., R reply>`: split as the replay
            # parser does (the result may hold a log line's text).
            parts = line.split(" | ", 2)
            try:
                ev, result = parts[0].split(), parts[2].split()
                term, cand, k = int(ev[2]), int(ev[3]), int(ev[10])
                votes = [(int(ev[11 + 3 * i]), ev[12 + 3 * i] == "1") for i in range(k)]
                if result[-3:-1] != ["R", "settled"]:
                    raise ValueError("no settled reply")
            except (IndexError, ValueError) as e:
                problems.append(f"(7) {rec.name}: unparsable settle record ({e}): {line[:120]}")
                continue
            n += 1
            for voter, granted in votes:
                if granted:
                    grants.setdefault((voter, term), set()).add(cand)
            if result[-1] == "1":
                winners.setdefault(term, set()).add(cand)
    if recs and not winners:
        problems.append("(7) no election was won in the recordings")
    for term, cands in sorted(winners.items()):
        if len(cands) > 1:
            problems.append(f"(7) term {term} has {len(cands)} leaders: {sorted(cands)}")
    for (voter, term), cands in sorted(grants.items()):
        if len(cands) > 1:
            problems.append(f"(8) voter {voter} granted term {term} to {sorted(cands)}")
    return n


def check_reveals(run, problems):
    """(9): `reveal <kind> <term> <vote> <commit> <last> <tail>` lines (`-`:
    not shown) against each later `recovered <term> <vote> <commit> <last> <d>`."""
    n = 0
    if not (run / "evidence").is_dir():
        problems.append("(9) no evidence directory")
    for f in sorted((run / "evidence").glob("*.reveal")):
        site, shown = f.stem, []
        for line in f.read_text(errors="replace").splitlines():
            w = line.split()
            if not w:
                continue
            if w[0] == "created":
                shown = []
            elif (w[0] == "reveal" and len(w) == 7 and w[2].isdigit() and w[6].isdigit()
                  and all(x == "-" or x.isdigit() for x in w[3:6])):
                shown.append(w[1:])
            elif w[0] == "recovered" and len(w) == 6 and all(x.isdigit() for x in w[1:]):
                n += 1
                term, vote, commit, last, d = (int(x) for x in w[1:])
                for kind, rt, rv, rc, rl, tail in shown:
                    what = f"(9) site {site}: recovered {' '.join(w[1:])} after a {kind} reveal"
                    rt, tail = int(rt), int(tail)
                    if d < tail:
                        problems.append(f"{what} that waited for record {tail}")
                    if term < rt:
                        problems.append(f"{what} at term {rt}")
                    if rv != "-" and rt == term and int(rv) != vote:
                        problems.append(f"{what} showing vote {rv} at term {rt}")
                    if rc != "-" and commit < int(rc):
                        problems.append(f"{what} sending commit {rc}")
                    if rl != "-" and rt == term and last < int(rl):
                        problems.append(f"{what} acknowledging {rl} at term {rt}")
            else:
                problems.append(f"(9) {f.name}: unparsable evidence line: {line[:120]}")
    return n


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    problems, summary = check(sys.argv[1])
    for p in problems:
        print(f"check: FAIL {p}")
    print(f"check: {'FAIL' if problems else 'ok'}: {summary}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
