#!/usr/bin/env python3
"""Mutation-test the Rust cache.

A test suite that has never been shown to FAIL has not been shown to test
anything. This injects each durability defect the C++ implementation was
verified against, runs the suite, and requires that at least one test
catches it. Anything that survives is a property the suite only appears to
cover.

Each mutation is an exact-string replacement. If the anchor text no longer
matches, that is a hard error rather than a skip: a silently-skipped
mutation reports as a pass and is worse than no mutation testing at all.

Usage:  python3 crates/mrx-core/mutations/run.py [name ...]
"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CRATE = Path(__file__).resolve().parent.parent

# (name, relative path, anchor, replacement, what it breaks)
MUTATIONS = [
    (
        "publish-gap",
        "src/durability.rs",
        """            // ORDER IS LOAD-BEARING
            let a = w.announce.load(Ordering::Acquire);
            if a < m {
                m = a;
            }
""",
        "",
        "drops the announce floor, so the watermark can pass a version "
        "that is drawn and published but not yet in any batch",
    ),
    (
        "stale-writeback",
        "src/store.rs",
        """                if cur.kind() != Kind::Evicted {
                    held.push((e, cur));
                }
                wrote.push(*idx);""",
        """                // MUTANT: skip an entry whose current version has
                // moved past the ticket, on the theory that the newer
                // write will carry it. It will not: a hot key is never
                // current at drain time.
                if cur.version() == *owed && cur.kind() != Kind::Evicted {
                    held.push((e, cur));
                }
                wrote.push(*idx);""",
        "confirms an obligation without writing the current bytes, so a "
        "key overwritten faster than drain latency is reported durable "
        "having never reached the store",
    ),
    (
        "discharge-before-write",
        "src/store.rs",
        """            match self.blobs.write_batch(&ops) {
                Ok(()) => {""",
        """            {
                let mut d = self.dirty.lock().expect("dirty lock poisoned");
                for idx in &wrote {
                    d.remove(idx);  // MUTANT: erase before the write lands
                }
            }
            match self.blobs.write_batch(&ops) {
                Ok(()) => {""",
        "discharges obligations before knowing the batch landed, so an IO "
        "failure silently advances the watermark",
    ),
    (
        "evict-above-watermark",
        "src/store.rs",
        """        if cur.version() > self.watermark.get() {
            return false; // the only copy
        }
""",
        "",
        "evicts a value the durable store has never seen, discarding the "
        "only copy",
    ),
    (
        "no-shutdown-barrier",
        "src/runtime.rs",
        "        let drained = self.store.drain_fully();",
        "        let drained = true;  // MUTANT: exit without draining",
        "exits maintenance without making acked writes durable",
    ),
    (
        "coalesce-newest",
        "src/store.rs",
        """                let slot = d.entry(t.entry).or_insert(t.version);
                *slot = (*slot).min(t.version);""",
        """                let slot = d.entry(t.entry).or_insert(t.version);
                *slot = (*slot).max(t.version);  // MUTANT: keep the newest""",
        "coalesces to the newest version per entry, discharging the older "
        "obligation early",
    ),
]


def run(cmd, cwd):
    return subprocess.run(
        cmd, cwd=cwd, capture_output=True, text=True, timeout=900
    )


def main():
    wanted = set(sys.argv[1:])
    selected = [m for m in MUTATIONS if not wanted or m[0] in wanted]
    if wanted and len(selected) != len(wanted):
        sys.exit(f"unknown mutation(s): {wanted - {m[0] for m in selected}}")

    print("baseline: ", end="", flush=True)
    base = run(["cargo", "test", "--quiet"], CRATE)
    if base.returncode != 0:
        print("FAILED\n" + base.stdout + base.stderr)
        sys.exit("the suite must be green before mutating it")
    print("green")

    killed, survived = [], []
    for name, rel, anchor, repl, breaks in selected:
        with tempfile.TemporaryDirectory(prefix=f"mrx-mut-{name}-") as tmp:
            work = Path(tmp) / "mrx-core"
            # Copy the SOURCE only, never a build dir: a copied build tree
            # keeps absolute paths into the original and silently
            # recompiles the unmutated source, which reports every mutation
            # as escaped. (That trap cost a full C++ mutation run.)
            shutil.copytree(
                CRATE,
                work,
                ignore=shutil.ignore_patterns("target", "*.lock"),
            )
            target = work / rel
            src = target.read_text()
            if anchor not in src:
                sys.exit(
                    f"mutation {name!r}: anchor not found in {rel}.\n"
                    "The code moved. Re-anchor it -- a skipped mutation "
                    "reports as a pass."
                )
            if src.count(anchor) != 1:
                sys.exit(
                    f"mutation {name!r}: anchor matches "
                    f"{src.count(anchor)} times in {rel}; make it unique"
                )
            target.write_text(src.replace(anchor, repl))

            print(f"  {name:24s} ", end="", flush=True)
            r = run(["cargo", "test", "--quiet"], work)
            if r.returncode == 0:
                print("SURVIVED  <-- the suite does not test this")
                survived.append((name, breaks))
            else:
                names = sorted(
                    ln.split()[0]
                    for ln in r.stdout.splitlines()
                    if ln.strip().endswith("FAILED") or " FAILED" in ln
                )
                detail = ", ".join(n for n in names if n != "test")[:120]
                print(f"killed    ({detail or 'compile/assert failure'})")
                killed.append(name)

    total = len(selected)
    print(f"\nscore: {len(killed)}/{total}")
    for name, breaks in survived:
        print(f"  SURVIVED {name}: {breaks}")
    sys.exit(0 if not survived else 1)


if __name__ == "__main__":
    main()
