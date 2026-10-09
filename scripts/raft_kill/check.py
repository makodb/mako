#!/usr/bin/env python3
"""Checks a raft_kill run (docs/verus/disk-persistence-plan.md P6).

Reads <run>/events/<proc>.events (raft_kill_node's lines, all incarnations
of a node appended to one file) and <run>/logs/<proc>.<inc>.log (each
launch's stderr), and requires:

  1. every launch reached `ready`, except one killed by its own armed crash
     point (its log holds `crash <point>`);
  2. every incarnation's apply sequence is a prefix of one history (Raft's
     state machine safety), with no id applied twice in it;
  3. every acknowledged id (a leader applied its own proposal: committed) is
     in that history, and in every node's last incarnation's sequence (they
     were quiesced before the stop);
  4. every launch that stopped cleanly printed `[DISK-VERIFY] ... ok`, and no
     launch printed a mismatch;
  5. no store holds a `.creating` side directory after the run;
  6. each store's WAL is bounded: checkpoints deleted the segments its base
     covers (at most 16 remain).

  check.py RUN_DIR   -> exit 0 and a summary, or exit 1 and the violations
"""
import os
import re
import sys
from pathlib import Path


def incarnations(events_text):
    """[(inc, [lines])] in file order; a `start` line opens one."""
    out = []
    for line in events_text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "start":
            out.append((int(parts[2]), []))
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
    for ev in sorted((run / "events").glob("*.events")):
        proc = ev.stem
        for inc, lines in incarnations(ev.read_text()):
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
            if not ready and not crashed:
                problems.append(f"(1) {proc}.{inc} never became ready")
            stopped = any(l[0] == "stopped" for l in lines)
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
    summary = (f"{len(sequences)} incarnation(s) of {len(last_inc)} node(s); history {len(history)} "
               f"applies; {len(acks)} acknowledged")
    return problems, summary


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
