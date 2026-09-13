#!/usr/bin/env python3
"""Source-isolated mutation gate for the mako-clock HLC port.

Every mutant is a small, plausible defect in the clock rules that
`Transaction.cc` enforces and that the cache relies on. For the baseline and
for every mutant the runner copies the crate into a fresh temporary workspace,
rewrites its path dependencies to absolute paths into the original tree, and
compiles into a distinct, initially empty target directory, so a green result
can never come from a stale artefact. A mutant is killed only when its named
test fails with the exact cargo failure line; any other outcome is a harness
error, not a survivor.

Machine-readable JSON goes to stdout; progress goes to stderr.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Sequence

RUNNER_DIR = Path(__file__).resolve().parent
ORIGINAL_CRATE = RUNNER_DIR.parent
ORIGINAL_CRATES = ORIGINAL_CRATE.parent
ORIGINAL_LOCKFILE = ORIGINAL_CRATES / "Cargo.lock"
PATH_DEPENDENCY = re.compile(r'(?P<prefix>path\s*=\s*)"(?P<path>[^"]+)"')
LIB_RS = "src/lib.rs"


@dataclasses.dataclass(frozen=True)
class Mutation:
    name: str
    anchor: str
    anchor_sha256: str
    replacement: str
    test: str
    breaks: str

    @property
    def failure_signature(self) -> str:
        return f"test tests::{self.test} ... FAILED"


MUTATIONS: tuple[Mutation, ...] = (
    Mutation(
        name="stamp-reuse",
        anchor="""                next = previous + 1;
            }
            if !valid_stamp(next) {
""",
        anchor_sha256="9fdc6f623469cb7f3373064b5edc78156f52ec712a575a9c9c98cf84301e014c",
        replacement="""                // MUTANT: a backward physical clock re-mints the current stamp.
                next = previous;
            }
            if !valid_stamp(next) {
""",
        test="backward_physical_clock_cannot_move_hlc_backward",
        breaks="Two commits share one Mako timestamp after the wall clock steps back.",
    ),
    Mutation(
        name="final-stamp-unmintable",
        anchor="""                if previous == STAMP_MAX {
                    return StampAllocation::TimestampExhausted;
                }
""",
        anchor_sha256="1af1edf6f040a572e2d70272f228466ee463648e934a37463b396ff08de33244",
        replacement="""                // MUTANT: give up one stamp early.
                if previous >= STAMP_MAX - 1 {
                    return StampAllocation::TimestampExhausted;
                }
""",
        test="recovery_leaves_the_final_timestamp_mintable",
        breaks="The last representable timestamp can never be committed.",
    ),
    Mutation(
        name="restricted-ignores-gate",
        anchor="""            if reject_general_gate && current & GENERAL_LOCK != 0 {
                return StampAllocation::GeneralLocked;
            }
""",
        anchor_sha256="22965f0b6d92365b03469103c64c25bbbbf5c186ddef86e4e169c52b92ee7d71",
        replacement="""            // MUTANT: the restricted profile no longer yields to the gate.
            if false {
                return StampAllocation::GeneralLocked;
            }
""",
        test="restricted_allocation_refuses_while_the_general_gate_is_held",
        breaks="A one-put commit can take a stamp inside a general commit's certification window.",
    ),
    Mutation(
        name="leave-general-drops-stamp",
        anchor="""        let previous = self.hlc.fetch_and(!GENERAL_LOCK, Ordering::Release);
""",
        anchor_sha256="1af9930d228655ff7002c1a4b883d66221fd2f1e83e4c1d5018577fd5e11e186",
        replacement="""        // MUTANT: releasing the gate resets the clock word.
        let previous = self.hlc.swap(GENERAL_LOCK, Ordering::Release) & GENERAL_LOCK | GENERAL_LOCK;
        self.hlc.store(0, Ordering::Release);
""",
        test="validation_prefix_preserves_hlc_gate_and_sequence",
        breaks="Leaving the gate rewinds the HLC to zero; later commits reuse old timestamps.",
    ),
    Mutation(
        name="enter-general-never-waits",
        anchor="""            if current & GENERAL_LOCK != 0 {
                std::hint::spin_loop();
                current = self.hlc.load(Ordering::Acquire);
                continue;
            }
""",
        anchor_sha256="3880d83d7ad40a4f62ed3041f0190aa7817b8ec4baf12acc2b15b9bf54f9f0c9",
        replacement="""            // MUTANT: a second general committer enters a held gate.
            if false {
                std::hint::spin_loop();
                current = self.hlc.load(Ordering::Acquire);
                continue;
            }
""",
        test="enter_general_waits_for_the_holder",
        breaks="Two general commits certify concurrently; the gate no longer excludes.",
    ),
    Mutation(
        name="reseed-under-gate",
        anchor="""        if self.general_gate_held() {
            return false;
        }
        self.sequence.store(sequence, Ordering::Release);
""",
        anchor_sha256="388317ae3a02b9d6402b7e038ee0dca68d8dba5b092e8882dc81262d234da3c3",
        replacement="""        // MUTANT: reseed while a general commit may be mid-sequence.
        self.sequence.store(sequence, Ordering::Release);
""",
        test="reseed_is_refused_while_the_gate_is_held",
        breaks="Namespace admission rewrites the dense sequence under a live committer.",
    ),
    Mutation(
        name="sequence-past-lane-boundary",
        anchor="""            if current >= SEQUENCE_MAX {
                return None;
            }
""",
        anchor_sha256="fd7facc2bf4c03122ea62302a491734b57b05b3186f7809a3a1a41a3407e9ca3",
        replacement="""            // MUTANT: mint one sequence past the 48-bit lane boundary.
            if current > SEQUENCE_MAX {
                return None;
            }
""",
        test="concurrent_sequence_stops_at_rust_lane_boundary",
        breaks="A cache sequence overflows into the worker-lane bits of the Rust sequence word.",
    ),
    Mutation(
        name="order-pair-sequence-first",
        anchor="""        match self.allocate_stamp(true) {
            StampAllocation::Accepted(stamp) => match self.try_allocate_sequence() {
                Some(sequence) => OrderPairAllocation::Accepted { sequence, stamp },
                None => OrderPairAllocation::SequenceExhausted,
            },
            StampAllocation::GeneralLocked => OrderPairAllocation::GeneralLocked,
            StampAllocation::TimestampExhausted => OrderPairAllocation::TimestampExhausted,
        }
""",
        anchor_sha256="870af16736c90c08b9eb965a3edbee9b0db603c9ccdb8f2a64a51bd5ddd36afc",
        replacement="""        // MUTANT: take the dense sequence before the timestamp.
        let Some(sequence) = self.try_allocate_sequence() else {
            return OrderPairAllocation::SequenceExhausted;
        };
        match self.allocate_stamp(true) {
            StampAllocation::Accepted(stamp) => OrderPairAllocation::Accepted { sequence, stamp },
            StampAllocation::GeneralLocked => OrderPairAllocation::GeneralLocked,
            StampAllocation::TimestampExhausted => OrderPairAllocation::TimestampExhausted,
        }
""",
        test="order_pair_allocates_the_timestamp_before_the_sequence",
        breaks="A failed timestamp leaves a hole in the dense queue instead of a harmless stamp gap.",
    ),
    Mutation(
        name="observe-lowers-clock",
        anchor="""        while stamp_of(current) < observed_stamp {
            let next = with_stamp(current, observed_stamp);
""",
        anchor_sha256="881d5eb876fa8871a68e1351e99c939cd238d80ce5f034b0889a2cb0be63effa",
        replacement="""        // MUTANT: observation overwrites instead of catching up.
        while stamp_of(current) != observed_stamp {
            let next = with_stamp(current, observed_stamp);
""",
        test="observe_stamp_ignores_invalid_and_older_stamps",
        breaks="Observing an older remote stamp moves the local clock backward.",
    ),
    Mutation(
        name="floor-ignores-submillisecond",
        anchor="""        if remainder != 0 || logical > LOGICAL_MASK {
""",
        anchor_sha256="a43595d8d140a58e1c431482e7445dfa3b30d48f2ff3ec503f14c5f5bb62776b",
        replacement="""        // MUTANT: forget that sub-millisecond time is later than the millisecond.
        if logical > LOGICAL_MASK {
""",
        test="recovery_floor_carries_submillisecond_and_wide_logical_bounds",
        breaks="After recovery the clock can mint a timestamp that precedes a recovered record.",
    ),
    Mutation(
        name="floor-off-by-one",
        anchor="""        let floor = minimum_next - 1;
""",
        anchor_sha256="bc248a204d3cb5c87bd2033001146a57d89e6eed39a10a48c091db72c1a694b4",
        replacement="""        // MUTANT: skip the first admissible stamp after recovery.
        let floor = minimum_next;
""",
        test="recovery_floor_precedes_next_allocation",
        breaks="Recovery burns one timestamp per restart and the Rust and C++ clocks diverge.",
    ),
    Mutation(
        name="floor-ignores-foreign-origin",
        anchor="""            let same_tuple_is_after = ORIGIN.get() > observed.origin();
""",
        anchor_sha256="444a07924adde1613f694db0f5d0415257e04b4032888c58482276f8232e80d8",
        replacement="""            // MUTANT: treat every same-tuple stamp as already ordered after.
            let same_tuple_is_after = true;
""",
        test="recovery_floor_precedes_next_allocation",
        breaks="A recovered (tuple, origin 1) record can be re-minted as the same tuple.",
    ),
)


class HarnessFailure(Exception):
    pass


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def validate_definitions() -> None:
    source = (ORIGINAL_CRATE / LIB_RS).read_text(encoding="utf-8")
    names = [mutation.name for mutation in MUTATIONS]
    if len(set(names)) != len(names):
        raise HarnessFailure("duplicate mutation names")
    for mutation in MUTATIONS:
        count = source.count(mutation.anchor)
        if count != 1:
            raise HarnessFailure(f"{mutation.name}: anchor occurs {count} times in {LIB_RS}, expected 1")
        digest = sha256_text(mutation.anchor)
        if digest != mutation.anchor_sha256:
            raise HarnessFailure(
                f"{mutation.name}: anchor sha256 is {digest}, definition pins {mutation.anchor_sha256!r}"
            )
        if mutation.replacement == mutation.anchor:
            raise HarnessFailure(f"{mutation.name}: replacement equals anchor")
        if f"fn {mutation.test}(" not in source:
            raise HarnessFailure(f"{mutation.name}: test {mutation.test} is not defined")


def create_workspace(label: str, temp_root: Path) -> Path:
    temp_root.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=f"mako-clock-mut-{label}-", dir=temp_root))
    crate = root / "mako-clock"
    shutil.copytree(
        ORIGINAL_CRATE,
        crate,
        ignore=shutil.ignore_patterns("target", ".git", "__pycache__", "*.pyc", "mutations"),
    )
    manifest = crate / "Cargo.toml"
    text = manifest.read_text(encoding="utf-8")

    def replace(match: re.Match[str]) -> str:
        dependency = (ORIGINAL_CRATE / match.group("path")).resolve()
        if not dependency.joinpath("Cargo.toml").is_file():
            raise HarnessFailure(f"path dependency does not resolve to a crate: {dependency}")
        return f'{match.group("prefix")}"{dependency}"'

    rewritten, count = PATH_DEPENDENCY.subn(replace, text)
    if count == 0:
        raise HarnessFailure("mako-clock manifest has no path dependencies to isolate")
    # Workspace-inherited keys need a workspace; provide one with those keys.
    manifest.write_text(rewritten, encoding="utf-8")
    original_workspace = (ORIGINAL_CRATES / "Cargo.toml").read_text(encoding="utf-8")
    package_section = re.search(r"\[workspace\.package\][^\[]*", original_workspace)
    if package_section is None:
        raise HarnessFailure("original workspace has no [workspace.package] table")
    (root / "Cargo.toml").write_text(
        '[workspace]\nresolver = "2"\nmembers = ["mako-clock"]\n\n' + package_section.group(0) + "\n",
        encoding="utf-8",
    )
    shutil.copy2(ORIGINAL_LOCKFILE, root / "Cargo.lock")
    return root


def run(command: Sequence[str], cwd: Path, env: dict[str, str], timeout: int) -> dict[str, object]:
    started = time.monotonic()
    completed = subprocess.run(
        list(command), cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout, check=False
    )
    return {
        "command": list(command),
        "returncode": completed.returncode,
        "seconds": round(time.monotonic() - started, 3),
        "stdout_tail": completed.stdout[-4000:],
        "stderr_tail": completed.stderr[-4000:],
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def normalize_lockfile(cargo: str, root: Path, env: dict[str, str], timeout: int) -> None:
    # The copied full-workspace lock carries dev edges of other members; one
    # offline metadata pass reduces it to this topology. Everything after is
    # --locked.
    result = run([cargo, "metadata", "--offline", "--format-version", "1"], root, env, timeout)
    if result["returncode"] != 0:
        raise HarnessFailure("lockfile normalization failed: " + str(result["stderr_tail"]))


def test_command(cargo: str, test: str) -> list[str]:
    return [
        cargo, "test", "--locked", "--offline", "-p", "mako-clock", "--all-features", "--lib",
        f"tests::{test}", "--", "--exact", "--nocapture",
    ]


def environment(root: Path) -> dict[str, str]:
    env = dict(os.environ)
    target = root / "cargo-target-empty"
    if target.exists():
        raise HarnessFailure(f"target directory unexpectedly exists: {target}")
    env["CARGO_TARGET_DIR"] = str(target)
    env.pop("RUSTFLAGS", None)
    return env


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo", default=os.environ.get("CARGO", "cargo"))
    parser.add_argument("--temp-root", default=os.environ.get("MAKO_CLOCK_MUTATION_TMP", ""))
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--report", default=None)
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args(argv)
    temp_root = Path(args.temp_root) if args.temp_root else Path(tempfile.gettempdir())

    report: dict[str, object] = {
        "crate": str(ORIGINAL_CRATE),
        "source_sha256": sha256_text((ORIGINAL_CRATE / LIB_RS).read_text(encoding="utf-8")),
        "mutations": [],
    }
    try:
        validate_definitions()
        selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
        if args.only and len(selected) != len(set(args.only)):
            raise HarnessFailure(f"unknown mutation names in {args.only}")

        # Baseline: every targeted test passes on the untouched copy.
        root = create_workspace("baseline", temp_root)
        env = environment(root)
        normalize_lockfile(args.cargo, root, env, args.timeout)
        baseline_tests = sorted({m.test for m in selected})
        baseline: list[dict[str, object]] = []
        for test in baseline_tests:
            print(f"[baseline] {test}", file=sys.stderr, flush=True)
            result = run(test_command(args.cargo, test), root, env, args.timeout)
            ok = result["returncode"] == 0 and f"test tests::{test} ... ok" in str(result["stdout"])
            baseline.append({"test": test, "ok": ok, "seconds": result["seconds"]})
            if not ok:
                raise HarnessFailure(f"baseline test {test} did not pass:\n{result['stdout_tail']}\n{result['stderr_tail']}")
        report["baseline"] = baseline
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)

        killed = 0
        for mutation in selected:
            print(f"[mutant] {mutation.name} -> {mutation.test}", file=sys.stderr, flush=True)
            root = create_workspace(mutation.name, temp_root)
            env = environment(root)
            normalize_lockfile(args.cargo, root, env, args.timeout)
            path = root / "mako-clock" / LIB_RS
            source = path.read_text(encoding="utf-8")
            if source.count(mutation.anchor) != 1:
                raise HarnessFailure(f"{mutation.name}: copied anchor count != 1")
            path.write_text(source.replace(mutation.anchor, mutation.replacement, 1), encoding="utf-8")
            result = run(test_command(args.cargo, mutation.test), root, env, args.timeout)
            stdout = str(result["stdout"])
            compiled = "error[E" not in str(result["stderr_tail"]) and "could not compile" not in str(result["stderr_tail"])
            if not compiled:
                raise HarnessFailure(f"{mutation.name}: mutant did not compile:\n{result['stderr_tail']}")
            # A failed assertion inside the targeted test is a kill even when
            # the unwinding trips a debug assertion in a guard's destructor
            # and the test process aborts before printing the FAILED line.
            combined = stdout + str(result["stderr"])
            own_panic = f"thread 'tests::{mutation.test}'" in combined and "panicked at" in combined
            was_killed = result["returncode"] != 0 and (mutation.failure_signature in stdout or own_panic)
            survived = result["returncode"] == 0 and f"test tests::{mutation.test} ... ok" in stdout
            if not was_killed and not survived:
                raise HarnessFailure(
                    f"{mutation.name}: neither killed nor survived (rc {result['returncode']}):\n{result['stdout_tail']}\n{result['stderr_tail']}"
                )
            killed += was_killed
            report["mutations"].append({
                "name": mutation.name,
                "test": mutation.test,
                "breaks": mutation.breaks,
                "outcome": "killed" if was_killed else "SURVIVED",
                "seconds": result["seconds"],
                "workspace": str(root) if args.keep else None,
            })
            print(f"[mutant] {mutation.name}: {'killed' if was_killed else 'SURVIVED'}", file=sys.stderr, flush=True)
            if not args.keep:
                shutil.rmtree(root, ignore_errors=True)
        report["killed"] = killed
        report["total"] = len(selected)
        report["verdict"] = "pass" if killed == len(selected) else "fail"
    except HarnessFailure as failure:
        report["verdict"] = "harness-error"
        report["error"] = str(failure)
    output = json.dumps(report, indent=2, sort_keys=True)
    print(output)
    if args.report:
        Path(args.report).write_text(output + "\n", encoding="utf-8")
    return 0 if report.get("verdict") == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
