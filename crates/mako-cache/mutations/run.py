#!/usr/bin/env python3
"""Strict, source-isolated mutation gate for mako-cache Phase 1F.

The runner never enables a mutation branch in the production crate.  For the
baseline and for every mutant it creates a new temporary Cargo workspace,
copies mako-cache source into it, rewrites path dependencies to absolute paths
back to the original workspace, and uses a distinct initially-empty target
directory.  This makes a green result from a stale original artefact a harness
error rather than evidence.

Progress is written to stderr.  The complete machine-readable report is JSON
on stdout, and may additionally be written with --report.
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
from typing import Iterable, Sequence


RUNNER_DIR = Path(__file__).resolve().parent
ORIGINAL_CRATE = RUNNER_DIR.parent
ORIGINAL_CRATES = ORIGINAL_CRATE.parent
ORIGINAL_LOCKFILE = ORIGINAL_CRATES / "Cargo.lock"

BASE_CARGO_ARGS = (
    "test",
    "--locked",
    "-p",
    "mako-cache",
    "--features",
    "test-support",
)


@dataclasses.dataclass(frozen=True)
class TestTarget:
    cargo_selector: tuple[str, ...]
    exact_name: str
    needs_native: bool = False
    needs_hooks: bool = False

    @property
    def failure_signature(self) -> str:
        return f"test {self.exact_name} ... FAILED"


@dataclasses.dataclass(frozen=True)
class Mutation:
    name: str
    relative_path: str
    anchor: str
    anchor_sha256: str
    replacement: str
    test: TestTarget
    breaks: str


WRITEBACK_UNIT = lambda name: TestTarget(
    ("--lib",), f"writeback::tests::{name}"
)
LIB_UNIT_NATIVE = lambda name: TestTarget(
    ("--lib",), f"tests::{name}", needs_native=True
)
NATIVE_INTEGRATION = lambda name: TestTarget(
    ("--test", "native"), name, needs_native=True
)


REPLAY_ANCHOR = """    #[cfg(test)]
    let mut records_replayed = 0usize;
    for record in records {
"""


MUTATIONS: tuple[Mutation, ...] = (
    Mutation(
        name="native-replay-drops-put-value",
        relative_path="src/record.rs",
        anchor="""                PUT_TAG => {
                    let value = cursor.take(value_len)?;
                    parsed.push(ParsedMutation::Put {
                        table_id,
                        key,
                        value,
                    });
                }
""",
        anchor_sha256="d0883774d9a0de40a3427a643047cbf5ea300aaaed3d903d89bcbe11119a717f",
        replacement="""                PUT_TAG => {
                    let value = cursor.take(value_len)?;
                    // MUTANT: replay every native put as an empty value.
                    parsed.push(ParsedMutation::Put {
                        table_id,
                        key,
                        value: &value[..0],
                    });
                }
""",
        test=NATIVE_INTEGRATION(
            "every_same_key_mutation_pair_has_one_canonical_backend_result"
        ),
        breaks=(
            "The native transaction installs the canonical final same-key value "
            "while asynchronous RocksDB replay materializes an empty value."
        ),
    ),
    Mutation(
        name="early-detached-capacity-discharge",
        relative_path="src/writeback.rs",
        anchor="""        permit.prepared = Some(Box::new(LegacyCommitRecord::prepare(
            mutations,
            self.config.max_record_bytes,
        )?));
        Ok(permit)
""",
        anchor_sha256="d6888cd1a604c60cdaa0f1a97573a9729075ad7ea5bc54953396cbd9143c92ed",
        replacement="""        permit.prepared = Some(Box::new(LegacyCommitRecord::prepare(
            mutations,
            self.config.max_record_bytes,
        )?));
        // MUTANT: advertise capacity while the detached permit still exists.
        permit.owns_claim = false;
        self.release_detached_claim(None);
        Ok(permit)
""",
        test=WRITEBACK_UNIT(
            "detached_permit_backpressures_another_reserver_until_drop"
        ),
        breaks=(
            "A second producer can enter a capacity-one queue before the first "
            "detached reservation is consumed or dropped."
        ),
    ),
    Mutation(
        name="hook-time-allocation",
        relative_path="src/writeback.rs",
        anchor="""        assert!(self.owns_claim, "binding must own a detached claim");
        // Legacy preparation remains boxed through the ordered hook. Its large
""",
        anchor_sha256="3e33505a4c3715689a4a27e13a387d0d4b96bdddf838b4ce5ef53a99882a199e",
        replacement="""        assert!(self.owns_claim, "binding must own a detached claim");
        // MUTANT: allocate while Silo's post-validation hook holds write locks.
        let hook_allocation = Box::new(mako_timestamp.get());
        std::hint::black_box(&hook_allocation);
        drop(hook_allocation);
        // Legacy preparation remains boxed through the ordered hook. Its large
""",
        test=TestTarget(
            ("--test", "hook_allocation"),
            "detached_bind_is_allocation_free_and_the_tripwire_is_live",
        ),
        breaks=(
            "DetachedPermit::bind performs a heap round trip in the native "
            "post-validation critical section."
        ),
    ),
    Mutation(
        name="conflict-cancellation-slot",
        relative_path="src/writeback.rs",
        anchor="""        } else {
            self.occupied.release();
            self.notify_capacity_release();
        }
""",
        anchor_sha256="fab75e41259f2c51141e60ecc98a3f29e6ff657d425cc37ca082bb200e7cdf35",
        replacement="""        } else {
            // MUTANT: turn a pre-validation abort into a cancelled log position.
            let cancelled = self
                .next_bound
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
                    current.checked_add(1)
                })
                .expect("mutant cancellation sequence exhausted")
                + 1;
            let mut state = lock_recover(&self.state);
            state.last_bound = cancelled;
            drop(state);
            self.occupied.release();
            self.notify_capacity_release();
        }
""",
        test=WRITEBACK_UNIT(
            "detached_abort_uses_capacity_but_never_assigns_a_sequence_or_slot"
        ),
        breaks=(
            "A validation loser advances last_bound and leaves a gap in the "
            "dense committed CacheSeq history."
        ),
    ),
    Mutation(
        name="missing-ready-publication",
        relative_path="src/writeback.rs",
        anchor="""        self.publication_cell(sequence)
            .publish_attached(sequence, self.publication_shift);
        self.finish_ready_publication(token)
""",
        anchor_sha256="a174de5042bfcd9b4174be299b053339b6679770a87cd98f02ea7631c779bfa7",
        replacement="""        // MUTANT: acknowledge without publishing the completed record cell.
        self.acknowledged.store(sequence.get(), Ordering::Release);
        Ok(())
""",
        test=WRITEBACK_UNIT(
            "bind_and_publish_make_the_prepared_to_ready_transition_explicit"
        ),
        breaks=(
            "A successful publish advances the acknowledged frontier but leaves "
            "the write-back consumer permanently blocked."
        ),
    ),
    Mutation(
        name="premature-ready-publication",
        relative_path="src/writeback.rs",
        anchor="""            state.queue.push_back(Slot {
                sequence,
                record: None,
                state: SlotState::Prepared { pinned: false },
            });
""",
        anchor_sha256="e7d3403c850a828f34a4ecd59253a7e30fd97ef5e353c86329f617394bdf5455",
        replacement="""            state.queue.push_back(Slot {
                sequence,
                record: None,
                // MUTANT: expose Ready before the finalized record exists.
                state: SlotState::Ready,
            });
""",
        test=WRITEBACK_UNIT(
            "bind_and_publish_make_the_prepared_to_ready_transition_explicit"
        ),
        breaks=(
            "The consumer may observe Ready before native success and before the "
            "finalized record has been stored."
        ),
    ),
    Mutation(
        name="unpinned-unknown-outcome",
        relative_path="src/writeback.rs",
        anchor="""        Ok(BoundReservation {
            owner: self.owner,
            token: QueueToken::new(sequence),
            mako_timestamp,
            legacy_prepared,
            native_buffer,
            on_drop: DropAction::PinUnknown,
        })
""",
        anchor_sha256="572c0fd358b6e53b5aaa7e0d3bb193eaff6925cb3c01c8ca2c501c7c823cf25f",
        replacement="""        Ok(BoundReservation {
            owner: self.owner,
            token: QueueToken::new(sequence),
            mako_timestamp,
            legacy_prepared,
            native_buffer,
            // MUTANT: silently discard an ambiguous post-bind outcome.
            on_drop: DropAction::Done,
        })
""",
        test=WRITEBACK_UNIT(
            "dropping_bound_unknown_finalizes_and_retains_its_write_set"
        ),
        breaks=(
            "Dropping a bound reservation neither finalizes its record nor pins "
            "the uncertain sequence, permitting unsafe progress."
        ),
    ),
    Mutation(
        name="partial-replay",
        relative_path="src/lib.rs",
        anchor=REPLAY_ANCHOR,
        anchor_sha256="3bd182ec1c7d606ab3d96c7fb4621cfd64235833fc046ee64eddf67867c720f2",
        replacement="""    #[cfg(test)]
    let mut records_replayed = 0usize;
    // MUTANT: silently omit the last committed record during native recovery.
    for record in records.iter().take(records.len().saturating_sub(1)) {
""",
        test=LIB_UNIT_NATIVE(
            "recovery_replays_each_cache_sequence_exactly_once_in_order"
        ),
        breaks=(
            "Recovery exposes a native cache missing a committed suffix while "
            "claiming the complete backend frontier."
        ),
    ),
    Mutation(
        name="reordered-replay",
        relative_path="src/lib.rs",
        anchor=REPLAY_ANCHOR,
        anchor_sha256="3bd182ec1c7d606ab3d96c7fb4621cfd64235833fc046ee64eddf67867c720f2",
        replacement="""    #[cfg(test)]
    let mut records_replayed = 0usize;
    // MUTANT: apply the dense commit history in reverse order.
    for record in records.iter().rev() {
""",
        test=LIB_UNIT_NATIVE(
            "recovery_replays_each_cache_sequence_exactly_once_in_order"
        ),
        breaks=(
            "Recovery violates CacheSeq serialization order and can materialize "
            "an older value or an impossible delete."
        ),
    ),
    Mutation(
        name="duplicate-replay",
        relative_path="src/lib.rs",
        anchor=REPLAY_ANCHOR,
        anchor_sha256="3bd182ec1c7d606ab3d96c7fb4621cfd64235833fc046ee64eddf67867c720f2",
        replacement="""    #[cfg(test)]
    let mut records_replayed = 0usize;
    // MUTANT: replay the first committed record a second time at the tail.
    for record in records.iter().chain(records.iter().take(1)) {
""",
        test=LIB_UNIT_NATIVE(
            "recovery_replays_each_cache_sequence_exactly_once_in_order"
        ),
        breaks=(
            "Recovery applies one committed transaction twice and can overwrite "
            "the correct final application state."
        ),
    ),
    Mutation(
        name="wrong-mako-timestamp",
        relative_path="src/writeback.rs",
        anchor="""            state
                .applied
                .advance(record.sequence(), record.mako_timestamp());
""",
        anchor_sha256="fcb53f319fe853c829e15edcba94840e62665bcd85b540b512bc66821b5626ab",
        replacement="""            // MUTANT: advance the frontier with a valid but different timestamp.
            let record_timestamp = record.mako_timestamp();
            let wrong_timestamp = MakoTimestamp::new(
                if record_timestamp.get() == 1 { 2 } else { 1 },
            )
            .expect("mutant timestamp remains nonzero");
            state.applied.advance(record.sequence(), wrong_timestamp);
""",
        test=TestTarget(
            ("--test", "timestamp"),
            "native_timestamp_matches_the_persisted_record_and_applied_frontier",
            needs_native=True,
            needs_hooks=True,
        ),
        breaks=(
            "The applied frontier no longer agrees with the exact Mako "
            "timestamp encoded by native serialization."
        ),
    ),
    Mutation(
        name="missing-recovery-clock-floor",
        relative_path="src/lib.rs",
        anchor="""    if let Some(timestamp) = applied_mako_timestamp {
        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::RecoveryBeforeClockFloor);
        mako_local::advance_mako_timestamp_past(timestamp)?;
        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::RecoveryAfterClockFloor);
    }
""",
        anchor_sha256="b4ebbd5d9c7f81332a4df1623ea159ff1fee03a17b1e8b4d495f84be466d0a33",
        replacement="""    if let Some(timestamp) = applied_mako_timestamp {
        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::RecoveryBeforeClockFloor);
        // MUTANT: expose recovery without advancing the process-wide clock.
        let _ = timestamp;
        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::RecoveryAfterClockFloor);
    }
""",
        test=LIB_UNIT_NATIVE(
            "recovery_advances_mako_timestamp_past_the_recovered_maximum"
        ),
        breaks=(
            "A post-recovery native commit can reuse or precede a timestamp in "
            "the recovered backend history."
        ),
    ),
)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_text(text: str) -> str:
    return sha256_bytes(text.encode("utf-8"))


def source_files() -> list[Path]:
    roots = [ORIGINAL_CRATE / "Cargo.toml", ORIGINAL_CRATE / "build.rs"]
    for directory in (ORIGINAL_CRATE / "src", ORIGINAL_CRATE / "tests"):
        if directory.exists():
            roots.extend(path for path in directory.rglob("*") if path.is_file())
    return sorted(path for path in roots if path.exists())


def source_hashes() -> dict[str, str]:
    return {
        path.relative_to(ORIGINAL_CRATE).as_posix(): sha256_bytes(path.read_bytes())
        for path in source_files()
    }


def tree_digest(hashes: dict[str, str]) -> str:
    canonical = "".join(f"{path}\0{digest}\n" for path, digest in sorted(hashes.items()))
    return sha256_text(canonical)


def validate_definitions(selected: Sequence[Mutation]) -> list[dict[str, object]]:
    errors: list[str] = []
    names = [mutation.name for mutation in MUTATIONS]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        errors.append(f"duplicate mutation names: {duplicates}")

    validations: list[dict[str, object]] = []
    for mutation in selected:
        target = ORIGINAL_CRATE / mutation.relative_path
        actual_anchor_digest = sha256_text(mutation.anchor)
        source = target.read_text(encoding="utf-8") if target.exists() else ""
        matches = source.count(mutation.anchor)
        replacement_matches = source.count(mutation.replacement)
        valid = True
        reasons: list[str] = []
        if actual_anchor_digest != mutation.anchor_sha256:
            valid = False
            reasons.append(
                "declared anchor SHA256 does not match the runner's anchor bytes: "
                f"declared={mutation.anchor_sha256}, actual={actual_anchor_digest}"
            )
        if matches != 1:
            valid = False
            reasons.append(f"anchor must match exactly once, found {matches}")
        if mutation.anchor == mutation.replacement:
            valid = False
            reasons.append("replacement equals anchor")
        if replacement_matches:
            valid = False
            reasons.append(
                f"replacement already occurs {replacement_matches} time(s) in original source"
            )
        validations.append(
            {
                "name": mutation.name,
                "path": mutation.relative_path,
                "anchor_sha256": mutation.anchor_sha256,
                "anchor_matches": matches,
                "replacement_matches": replacement_matches,
                "valid": valid,
                "reasons": reasons,
            }
        )
        errors.extend(f"{mutation.name}: {reason}" for reason in reasons)

    if not ORIGINAL_LOCKFILE.is_file():
        errors.append(f"workspace lockfile is missing: {ORIGINAL_LOCKFILE}")
    if errors:
        raise HarnessFailure("definition validation failed", details=errors)
    return validations


class HarnessFailure(RuntimeError):
    def __init__(self, message: str, *, details: object | None = None):
        super().__init__(message)
        self.details = details


@dataclasses.dataclass
class TempWorkspace:
    root: Path
    crate: Path
    target: Path
    cleanup: tempfile.TemporaryDirectory[str] | None

    def close(self) -> None:
        if self.cleanup is not None:
            self.cleanup.cleanup()


PATH_DEPENDENCY = re.compile(r'(?P<prefix>\bpath\s*=\s*)"(?P<path>[^"]+)"')


def rewrite_path_dependencies(manifest: Path) -> list[dict[str, str]]:
    source = manifest.read_text(encoding="utf-8")
    rewrites: list[dict[str, str]] = []

    def replace(match: re.Match[str]) -> str:
        raw = match.group("path")
        dependency = (ORIGINAL_CRATE / raw).resolve()
        if not dependency.joinpath("Cargo.toml").is_file():
            raise HarnessFailure(
                f"path dependency {raw!r} does not resolve to a crate",
                details=str(dependency),
            )
        rewrites.append({"from": raw, "to": str(dependency)})
        escaped = str(dependency).replace("\\", "\\\\").replace('"', '\\"')
        return f'{match.group("prefix")}"{escaped}"'

    rewritten = PATH_DEPENDENCY.sub(replace, source)
    if not rewrites:
        raise HarnessFailure("mako-cache manifest has no path dependencies to isolate")
    manifest.write_text(rewritten, encoding="utf-8")
    return rewrites


def create_workspace(
    label: str, keep: bool, temp_root: Path
) -> tuple[TempWorkspace, list[dict[str, str]]]:
    temp_root.mkdir(parents=True, exist_ok=True)
    if keep:
        root = Path(
            tempfile.mkdtemp(prefix=f"mako-cache-mut-{label}-", dir=temp_root)
        )
        cleanup = None
    else:
        holder: tempfile.TemporaryDirectory[str] = tempfile.TemporaryDirectory(
            prefix=f"mako-cache-mut-{label}-", dir=temp_root
        )
        root = Path(holder.name)
        cleanup = holder

    crate = root / "mako-cache"
    shutil.copytree(
        ORIGINAL_CRATE,
        crate,
        ignore=shutil.ignore_patterns("target", ".git", "__pycache__", "*.pyc"),
    )
    rewrites = rewrite_path_dependencies(crate / "Cargo.toml")
    (root / "Cargo.toml").write_text(
        """[workspace]
resolver = "2"
members = ["mako-cache"]

[profile.release]
lto = false
panic = "abort"
codegen-units = 1
opt-level = 3

[profile.dev]
lto = false
""",
        encoding="utf-8",
    )
    shutil.copy2(ORIGINAL_LOCKFILE, root / "Cargo.lock")
    target = root / "cargo-target-empty"
    if target.exists():
        raise HarnessFailure(f"new target directory unexpectedly exists: {target}")
    return TempWorkspace(root, crate, target, cleanup), rewrites


def command_display(command: Sequence[str]) -> str:
    # JSON command arrays are authoritative; this form is only for progress.
    return " ".join(command)


def command_result(
    command: Sequence[str],
    *,
    cwd: Path,
    env: dict[str, str],
    timeout: int,
) -> dict[str, object]:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            list(command),
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            check=False,
        )
        timed_out = False
        stdout = completed.stdout
        stderr = completed.stderr
        returncode: int | None = completed.returncode
    except subprocess.TimeoutExpired as error:
        timed_out = True
        stdout = error.stdout.decode() if isinstance(error.stdout, bytes) else (error.stdout or "")
        stderr = error.stderr.decode() if isinstance(error.stderr, bytes) else (error.stderr or "")
        returncode = None
    elapsed = time.monotonic() - started
    combined = stdout + stderr
    running_test_counts = [
        int(match) for match in re.findall(r"(?m)^running (\d+) tests?\s*$", combined)
    ]
    failed_test_lines = [
        line.strip()
        for line in combined.splitlines()
        if line.startswith("test ") and line.rstrip().endswith(" FAILED")
    ]
    return {
        "command": list(command),
        "cwd": str(cwd),
        "timeout_seconds": timeout,
        "timed_out": timed_out,
        "returncode": returncode,
        "elapsed_seconds": round(elapsed, 3),
        "output_sha256": sha256_text(combined),
        "output_tail": combined[-6000:],
        "running_test_counts": running_test_counts,
        "failed_test_lines": failed_test_lines,
    }


def cargo_environment(
    workspace: TempWorkspace,
    *,
    native_build: Path | None,
    require_hooks: bool,
) -> dict[str, str]:
    env = os.environ.copy()
    env["CARGO_TARGET_DIR"] = str(workspace.target)
    if native_build is not None:
        env["MAKO_LOCAL_FAKE_ABI"] = "0"
        env["MAKO_BUILD_DIR"] = str(native_build)
        env["MAKO_LOCAL_REQUIRE_NATIVE"] = "1"
    else:
        # mako-local lives at its original absolute path and would otherwise
        # auto-discover a possibly stale default CMake tree there. Pure-Rust
        # mutants intentionally compile against its explicit fake ABI.
        env["MAKO_LOCAL_FAKE_ABI"] = "1"
        env.pop("MAKO_BUILD_DIR", None)
        env.pop("MAKO_LOCAL_REQUIRE_NATIVE", None)
    if require_hooks:
        env["MAKO_CACHE_REQUIRE_NATIVE_CRASH_HOOKS"] = "1"
    else:
        env.pop("MAKO_CACHE_REQUIRE_NATIVE_CRASH_HOOKS", None)
    return env


def prepare_lockfile(
    args: argparse.Namespace,
    workspace: TempWorkspace,
    env: dict[str, str],
) -> dict[str, object]:
    """Normalize the copied full-workspace lock to the one-member topology.

    Cargo records dev-dependency edges for every workspace member.  Once only
    mako-cache is copied, dependencies such as mako-local are ordinary path
    dependencies and their dev-only edges disappear.  Cargo therefore refuses
    the byte-for-byte full-workspace lock under --locked even though every
    resolved registry version is already pinned.  One offline metadata pass
    performs that topology-only normalization; every compile/test after it is
    still --locked.
    """

    lockfile = workspace.root / "Cargo.lock"
    copied_sha = sha256_bytes(lockfile.read_bytes())
    command = [
        args.cargo,
        "metadata",
        "--offline",
        "--format-version",
        "1",
    ]
    result = command_result(
        command,
        cwd=workspace.root,
        env=env,
        timeout=args.compile_timeout,
    )
    normalized_sha = sha256_bytes(lockfile.read_bytes())
    target_still_empty = not workspace.target.exists()
    return {
        "copied_lockfile_sha256": copied_sha,
        "normalized_lockfile_sha256": normalized_sha,
        "normalization": result,
        "target_still_absent": target_still_empty,
    }


def test_command(cargo: str, target: TestTarget) -> list[str]:
    return [
        cargo,
        *BASE_CARGO_ARGS,
        *target.cargo_selector,
        target.exact_name,
        "--",
        "--exact",
        "--nocapture",
    ]


def test_count(result: dict[str, object]) -> int:
    return sum(int(count) for count in result["running_test_counts"])


def resolve_build_dirs(
    args: argparse.Namespace, selected: Sequence[Mutation]
) -> tuple[Path | None, bool]:
    hooks_needed = any(mutation.test.needs_hooks for mutation in selected)
    native_needed = hooks_needed or any(mutation.test.needs_native for mutation in selected)

    hook_value = args.hook_build_dir
    native_value = args.mako_build_dir
    if native_needed:
        hook_value = hook_value or os.environ.get("MAKO_CACHE_HOOK_BUILD_DIR")
        native_value = native_value or os.environ.get("MAKO_BUILD_DIR")

    if hook_value:
        chosen = Path(hook_value).expanduser().resolve()
        require_hooks = True
    elif native_value:
        chosen = Path(native_value).expanduser().resolve()
        require_hooks = False
    else:
        chosen = None
        require_hooks = False

    if native_needed and chosen is None:
        raise HarnessFailure(
            "selected mutants require native Mako",
            details=(
                "pass --mako-build-dir PATH (or MAKO_BUILD_DIR); the full/default "
                "matrix includes the timestamp mutant and therefore requires "
                "--hook-build-dir PATH (or MAKO_CACHE_HOOK_BUILD_DIR)"
            ),
        )
    if hooks_needed and not require_hooks:
        raise HarnessFailure(
            "selected mutants require the hook-enabled native archive",
            details=(
                "pass --hook-build-dir PATH or set MAKO_CACHE_HOOK_BUILD_DIR; "
                "the directory must contain libmako.a built with "
                "MAKO_LOCAL_TEST_HOOKS=ON"
            ),
        )
    if chosen is not None:
        for required in (chosen / "libmako.a", chosen / "CMakeCache.txt"):
            if not required.is_file():
                raise HarnessFailure(
                    f"native build is incomplete: {required} is missing"
                )
    return chosen, require_hooks


def mutate_copy(workspace: TempWorkspace, mutation: Mutation) -> dict[str, str]:
    target = workspace.crate / mutation.relative_path
    source = target.read_text(encoding="utf-8")
    matches = source.count(mutation.anchor)
    if matches != 1:
        raise HarnessFailure(
            f"{mutation.name}: copied-source anchor count is {matches}, expected 1"
        )
    original_sha = sha256_text(source)
    mutated = source.replace(mutation.anchor, mutation.replacement, 1)
    if mutated == source:
        raise HarnessFailure(f"{mutation.name}: replacement did not change source")
    target.write_text(mutated, encoding="utf-8")
    observed = target.read_text(encoding="utf-8")
    if observed.count(mutation.anchor) != 0 or observed.count(mutation.replacement) != 1:
        raise HarnessFailure(
            f"{mutation.name}: mutation was not present exactly once after write"
        )
    return {
        "original_file_sha256": original_sha,
        "mutated_file_sha256": sha256_text(observed),
        "replacement_sha256": sha256_text(mutation.replacement),
    }


def run_baseline(
    args: argparse.Namespace,
    native_build: Path | None,
    require_hooks: bool,
) -> dict[str, object]:
    workspace, rewrites = create_workspace(
        "baseline", args.keep_workdirs, args.resolved_temp_root
    )
    try:
        env = cargo_environment(
            workspace, native_build=native_build, require_hooks=require_hooks
        )
        lock_setup = prepare_lockfile(args, workspace, env)
        if (
            lock_setup["normalization"]["timed_out"]
            or lock_setup["normalization"]["returncode"] != 0
            or not lock_setup["target_still_absent"]
        ):
            return {
                "status": "harness_error",
                "reason": "copied lockfile normalization failed or populated target",
                "workspace": str(workspace.root) if args.keep_workdirs else None,
                "path_rewrites": rewrites,
                "lock_setup": lock_setup,
            }
        compile_command = [args.cargo, *BASE_CARGO_ARGS, "--no-run"]
        print(f"baseline compile: {command_display(compile_command)}", file=sys.stderr)
        compile_result = command_result(
            compile_command,
            cwd=workspace.root,
            env=env,
            timeout=args.compile_timeout,
        )
        if compile_result["timed_out"] or compile_result["returncode"] != 0:
            return {
                "status": "harness_error",
                "reason": "baseline did not compile",
                "workspace": str(workspace.root) if args.keep_workdirs else None,
                "path_rewrites": rewrites,
                "lock_setup": lock_setup,
                "compile": compile_result,
            }

        test_command_line = [args.cargo, *BASE_CARGO_ARGS]
        print(f"baseline tests:   {command_display(test_command_line)}", file=sys.stderr)
        test_result = command_result(
            test_command_line,
            cwd=workspace.root,
            env=env,
            timeout=args.baseline_timeout,
        )
        status = (
            "green"
            if not test_result["timed_out"] and test_result["returncode"] == 0
            else "harness_error"
        )
        return {
            "status": status,
            "reason": None if status == "green" else "baseline tests were not green",
            "workspace": str(workspace.root) if args.keep_workdirs else None,
            "path_rewrites": rewrites,
            "lock_setup": lock_setup,
            "compile": compile_result,
            "tests": test_result,
        }
    finally:
        workspace.close()


def run_mutation(
    args: argparse.Namespace,
    mutation: Mutation,
    native_build: Path | None,
    require_hooks: bool,
) -> dict[str, object]:
    workspace, rewrites = create_workspace(
        mutation.name, args.keep_workdirs, args.resolved_temp_root
    )
    base: dict[str, object] = {
        "name": mutation.name,
        "path": mutation.relative_path,
        "anchor_sha256": mutation.anchor_sha256,
        "expected_signature": mutation.test.failure_signature,
        "designated_test": mutation.test.exact_name,
        "breaks": mutation.breaks,
        "workspace": str(workspace.root) if args.keep_workdirs else None,
        "path_rewrites": rewrites,
    }
    try:
        base.update(mutate_copy(workspace, mutation))
        env = cargo_environment(
            workspace, native_build=native_build, require_hooks=require_hooks
        )
        lock_setup = prepare_lockfile(args, workspace, env)
        base["lock_setup"] = lock_setup
        if (
            lock_setup["normalization"]["timed_out"]
            or lock_setup["normalization"]["returncode"] != 0
            or not lock_setup["target_still_absent"]
        ):
            base.update(
                status="harness_error",
                reason="copied lockfile normalization failed or populated target",
            )
            return base
        compile_command = [
            args.cargo,
            *BASE_CARGO_ARGS,
            *mutation.test.cargo_selector,
            "--no-run",
        ]
        print(f"{mutation.name}: compile", file=sys.stderr, flush=True)
        compile_result = command_result(
            compile_command,
            cwd=workspace.root,
            env=env,
            timeout=args.compile_timeout,
        )
        base["compile"] = compile_result
        if compile_result["timed_out"]:
            base.update(status="harness_error", reason="mutant compilation timed out")
            return base
        if compile_result["returncode"] != 0:
            base.update(
                status="harness_error",
                reason="mutant did not compile; compile failures do not count as kills",
            )
            return base

        command = test_command(args.cargo, mutation.test)
        print(
            f"{mutation.name}: test {mutation.test.exact_name}",
            file=sys.stderr,
            flush=True,
        )
        result = command_result(
            command,
            cwd=workspace.root,
            env=env,
            timeout=args.test_timeout,
        )
        base["test"] = result
        if result["timed_out"]:
            base.update(
                status="harness_error",
                reason="designated test timed out; hangs do not count as kills",
            )
            return base

        executed = test_count(result)
        base["executed_test_count"] = executed
        if executed != 1:
            base.update(
                status="harness_error",
                reason=f"designated command executed {executed} tests, expected exactly 1",
            )
            return base
        if result["returncode"] == 0:
            base.update(
                status="survived",
                reason="designated test passed with the mutant present",
            )
            return base
        signature = mutation.test.failure_signature
        if signature not in result["failed_test_lines"]:
            base.update(
                status="harness_error",
                reason="test process failed without the designated failure signature",
            )
            return base
        base.update(
            status="killed",
            reason="designated exact test failed with the expected signature",
            observed_signature=signature,
        )
        return base
    except HarnessFailure as error:
        base.update(status="harness_error", reason=str(error), details=error.details)
        return base
    finally:
        workspace.close()


def mutation_summary(mutation: Mutation) -> dict[str, object]:
    return {
        "name": mutation.name,
        "path": mutation.relative_path,
        "anchor_sha256": mutation.anchor_sha256,
        "designated_test": mutation.test.exact_name,
        "needs_native": mutation.test.needs_native,
        "needs_hooks": mutation.test.needs_hooks,
        "breaks": mutation.breaks,
    }


def write_report(report: dict[str, object], destination: str | None) -> None:
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if destination:
        path = Path(destination).expanduser()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(rendered, encoding="utf-8")
    sys.stdout.write(rendered)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("mutations", nargs="*", help="mutation names; default: all")
    result.add_argument("--list", action="store_true", help="list mutation contracts as JSON")
    result.add_argument(
        "--check",
        action="store_true",
        help="validate names, exact anchors, digests, and source integrity without Cargo",
    )
    result.add_argument("--cargo", default="cargo", help="Cargo executable")
    result.add_argument(
        "--mako-build-dir",
        help="CMake tree containing libmako.a for native tests (or MAKO_BUILD_DIR)",
    )
    result.add_argument(
        "--hook-build-dir",
        help=(
            "CMake tree containing the MAKO_LOCAL_TEST_HOOKS=ON libmako.a; "
            "takes precedence over --mako-build-dir (or MAKO_CACHE_HOOK_BUILD_DIR)"
        ),
    )
    result.add_argument("--compile-timeout", type=int, default=900)
    result.add_argument("--baseline-timeout", type=int, default=1200)
    result.add_argument("--test-timeout", type=int, default=180)
    result.add_argument(
        "--temp-root",
        help=(
            "parent for isolated workspaces/targets; defaults to "
            "MAKO_CACHE_MUTATION_TEMP_ROOT or the user's cache directory"
        ),
    )
    result.add_argument(
        "--keep-workdirs",
        action="store_true",
        help="retain isolated workspaces for debugging and report their paths",
    )
    result.add_argument(
        "--report",
        help="also write the JSON report to this path; JSON is always printed to stdout",
    )
    return result


def select_mutations(names: Iterable[str]) -> list[Mutation]:
    wanted = list(names)
    by_name = {mutation.name: mutation for mutation in MUTATIONS}
    unknown = sorted(set(wanted) - set(by_name))
    if unknown:
        raise HarnessFailure("unknown mutation name(s)", details=unknown)
    return [by_name[name] for name in wanted] if wanted else list(MUTATIONS)


def resolve_temp_root(value: str | None) -> Path:
    configured = value or os.environ.get("MAKO_CACHE_MUTATION_TEMP_ROOT")
    if configured:
        return Path(configured).expanduser().resolve()
    cache_home = os.environ.get("XDG_CACHE_HOME")
    base = Path(cache_home).expanduser() if cache_home else Path.home() / ".cache"
    return (base / "mako-cache-mutations").resolve()


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    args.resolved_temp_root = resolve_temp_root(args.temp_root)
    started_wall = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    started = time.monotonic()
    before = source_hashes()
    report: dict[str, object] = {
        "schema": "mako-cache-phase1f-mutations-v1",
        "started_utc": started_wall,
        "original_crate": str(ORIGINAL_CRATE),
        "temp_root": str(args.resolved_temp_root),
        "scope": "applied-to-Rocks; asynchronous and not a Rocks WAL durability claim",
    }
    exit_code = 2
    try:
        selected = select_mutations(args.mutations)
        report["selected"] = [mutation.name for mutation in selected]
        if args.list:
            report["status"] = "listed"
            report["mutations"] = [mutation_summary(mutation) for mutation in selected]
            exit_code = 0
            return finish(report, before, started, args.report, exit_code)

        validations = validate_definitions(selected)
        report["anchor_validation"] = validations
        if args.check:
            # Exercise the copy/manifest rewrite path too.  This catches a new
            # relative dependency (for example mako-history) before a long run.
            workspace, rewrites = create_workspace(
                "check", False, args.resolved_temp_root
            )
            try:
                report["path_rewrites"] = rewrites
                report["copied_lockfile_sha256"] = sha256_bytes(
                    (workspace.root / "Cargo.lock").read_bytes()
                )
                report["empty_target_confirmed"] = not workspace.target.exists()
            finally:
                workspace.close()
            report["status"] = "checked"
            exit_code = 0
            return finish(report, before, started, args.report, exit_code)

        native_build, require_hooks = resolve_build_dirs(args, selected)
        report["native_profile"] = {
            "build_dir": str(native_build) if native_build else None,
            "hook_archive_required": require_hooks,
        }

        baseline = run_baseline(args, native_build, require_hooks)
        report["baseline"] = baseline
        if baseline["status"] != "green":
            report["status"] = "harness_error"
            report["reason"] = "unmutated baseline must compile and pass first"
            exit_code = 2
            return finish(report, before, started, args.report, exit_code)

        results = []
        for mutation in selected:
            result = run_mutation(args, mutation, native_build, require_hooks)
            results.append(result)
            print(
                f"{mutation.name}: {result['status']}", file=sys.stderr, flush=True
            )
        report["mutations"] = results
        killed = sum(result["status"] == "killed" for result in results)
        survived = sum(result["status"] == "survived" for result in results)
        harness_errors = sum(result["status"] == "harness_error" for result in results)
        report["score"] = {
            "killed": killed,
            "total": len(results),
            "survived": survived,
            "harness_errors": harness_errors,
        }
        if harness_errors:
            report["status"] = "harness_error"
            exit_code = 2
        elif survived:
            report["status"] = "mutation_survived"
            exit_code = 1
        else:
            report["status"] = "all_killed"
            exit_code = 0
        return finish(report, before, started, args.report, exit_code)
    except HarnessFailure as error:
        report["status"] = "harness_error"
        report["reason"] = str(error)
        report["details"] = error.details
        return finish(report, before, started, args.report, 2)


def finish(
    report: dict[str, object],
    before: dict[str, str],
    started: float,
    destination: str | None,
    intended_exit: int,
) -> int:
    after = source_hashes()
    changed = sorted(
        path
        for path in set(before) | set(after)
        if before.get(path) != after.get(path)
    )
    integrity = {
        "file_count": len(before),
        "before_tree_sha256": tree_digest(before),
        "after_tree_sha256": tree_digest(after),
        "unchanged": not changed,
        "changed_paths": changed,
        "file_sha256": before,
    }
    report["source_integrity"] = integrity
    report["elapsed_seconds"] = round(time.monotonic() - started, 3)
    if changed:
        report["status"] = "harness_error"
        report["reason"] = "original mako-cache source changed during the run"
        intended_exit = 2
    write_report(report, destination)
    return intended_exit


if __name__ == "__main__":
    raise SystemExit(main())
