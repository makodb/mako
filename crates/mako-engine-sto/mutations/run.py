#!/usr/bin/env python3
"""Source-isolated mutation gate for the Rust STO engine (mako-engine-sto).

Each mutant is a plausible defect in the engine's own logic: the canonical
write set, the one-put witness, the record encoder, the publication-cell
protocol, the item budget, or a terminal's outcome. The runner copies the
crate into a fresh temporary workspace, rewrites its path dependencies to
absolute paths into the original tree, compiles into a distinct empty target
directory, and kills a mutant only when its designated test reports failure
or names itself in a panic. A panic-abort profile may terminate before Cargo
prints the failure line. Anything else is a harness error, not a survivor.

The engine's tests link the native Masstree archives: run this with the
`MAKO_MTREE_NATIVE_*` environment the `rust_sto_integration` CMake target
sets (see `run_mako_engine_sto_tests`). Machine-readable JSON goes to stdout;
progress goes to stderr.
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
ORIGINAL_TOOLCHAIN = ORIGINAL_CRATES.parent / "rust-toolchain.toml"
PATH_DEPENDENCY = re.compile(r'(?P<prefix>path\s*=\s*)"(?P<path>[^"]+)"')


@dataclasses.dataclass(frozen=True)
class Mutation:
    name: str
    relative_path: str
    anchor: str
    anchor_sha256: str
    replacement: str
    # ("--lib", "record::tests::name") or ("--test", "native", "name")
    target: tuple[str, ...]
    test: str
    breaks: str

    @property
    def failure_signature(self) -> str:
        return f"test {self.test} ... FAILED"


def native(name: str) -> tuple[str, ...]:
    return ("--test", "native")


LIB = ("--lib",)

MUTATIONS: tuple[Mutation, ...] = (
    Mutation(
        name="terminal-version-exhaustion-decode-rejected",
        relative_path="src/outcome.rs",
        anchor="                | Error::VersionExhausted\n",
        anchor_sha256="39cecd9b5e087c4357569a9411af26485c2abd86ea857df17b076930afca082e",
        replacement="",
        target=LIB,
        test="txn::ownership_tests::terminal_version_capacity_is_not_a_transaction_size_error",
        breaks="A definite version-exhaustion abort is rejected by the compact terminal decoder.",
    ),
    Mutation(
        name="terminal-version-exhaustion-as-size",
        relative_path="src/txn.rs",
        anchor="        AbortReason::Capacity(CapacityError::VersionExhausted) => Error::VersionExhausted,\n",
        anchor_sha256="34fd1383682187a20c73f2fc97fabf26fbbc46967b72175f2ca5dad077a3be63",
        replacement="        AbortReason::Capacity(CapacityError::VersionExhausted) => Error::TransactionTooLarge,\n",
        target=LIB,
        test="txn::ownership_tests::terminal_version_capacity_is_not_a_transaction_size_error",
        breaks="Terminal version exhaustion is misreported as an attempt-local size limit.",
    ),
    Mutation(
        name="facade-retains-global-commit-clock",
        relative_path="src/db.rs",
        anchor="            .with_record_local_versions(true);\n",
        anchor_sha256="951f10f58024b18b3ec08d611585b40189c9dbb1b51c74eb29e98c5494307055",
        replacement="            .with_record_local_versions(false);\n",
        target=LIB,
        test="txn::local_version_tests::facade_tables_use_local_versions_and_preserve_mixed_ordered_writes",
        breaks="The facade silently loses its non-opaque record-local version policy.",
    ),
    Mutation(
        name="record-ops-reversed",
        relative_path="src/txn.rs",
        anchor="""    fn canonical_ops(&self) -> Vec<OpRef<'_>> {
        self.touched
            .iter()
""",
        anchor_sha256="485094450ff20963e945454ff6e51e3f5f839b91fa71518228dced5f99b4fcf7",
        replacement="""    fn canonical_ops(&self) -> Vec<OpRef<'_>> {
        // MUTANT: emit the canonical write set in reverse touch order.
        self.touched
            .iter()
            .rev()
""",
        target=native("general_record_terminal_serializes_the_canonical_write_set"),
        test="general_record_terminal_serializes_the_canonical_write_set",
        breaks="The durable record orders operations differently from the C++ engine.",
    ),
    Mutation(
        name="insert-then-remove-not-elided",
        relative_path="src/txn.rs",
        anchor="""                Staged::Deleted if entry.existed_before == Some(true) => Some(OpRef {
""",
        anchor_sha256="b61f2815b77bc8c8b845c1c4c93173e0048093b4ae71e1b0a34b64ba6271d06f",
        replacement="""                // MUTANT: a removed key that this transaction created still emits a delete.
                Staged::Deleted => Some(OpRef {
""",
        target=native("general_record_terminal_serializes_the_canonical_write_set"),
        test="general_record_terminal_serializes_the_canonical_write_set",
        breaks="Insert-then-remove leaves a spurious delete in the record.",
    ),
    Mutation(
        name="existed-before-from-last-op",
        relative_path="src/txn.rs",
        anchor="""        if existed {
            self.mutations += 1;
            entry.wrote = true;
            entry.deleted = true;
            entry.staged = Staged::Deleted;
            entry.value.clear();
            entry.existed_before.get_or_insert(true);
""",
        anchor_sha256="fd3ab4542a42c55a931989f12437ef1c004d35ad09a952061cd05cbdbd0a43dd",
        replacement="""        if existed {
            self.mutations += 1;
            entry.wrote = true;
            entry.deleted = true;
            entry.staged = Staged::Deleted;
            entry.value.clear();
            // MUTANT: the last observation, not the first, decides the prior state.
            entry.existed_before = Some(true);
""",
        target=native("general_record_terminal_serializes_the_canonical_write_set"),
        test="general_record_terminal_serializes_the_canonical_write_set",
        breaks="A key inserted and removed in one transaction is recorded as a delete.",
    ),
    Mutation(
        name="wrong-created-bit",
        relative_path="src/txn.rs",
        anchor="""        Ok(!existed)
    }

    /// Insert only when absent, returning whether insertion was staged.
""",
        anchor_sha256="aa019e335191fe00203fe1faea6b0f5cf7e97c3ac6b6a134b38ab7d25d2ae049",
        replacement="""        // MUTANT: report the prior presence instead of creation.
        Ok(existed)
    }

    /// Insert only when absent, returning whether insertion was staged.
""",
        target=native("point_operations_compose_with_read_your_writes"),
        test="point_operations_compose_with_read_your_writes",
        breaks="put reports the inverse of the `created` bit the cache relies on.",
    ),
    Mutation(
        name="candidate-survives-read",
        relative_path="src/txn.rs",
        anchor="""        self.check_active()?;
        self.check_table(table)?;
        self.invalidate_borrowed_witness();
        if !self.charge(1) {
            return self.fail_closed(Error::TransactionTooLarge);
        }
        let worker = self.attachment.worker;
        let result = table.entry.table.get(self.inner_mut(), worker, key);
""",
        anchor_sha256="dc31f4fbf8eee2a27eb9dbda14c20947ae1306a630db6b20667fee297a130b59",
        replacement="""        self.check_active()?;
        self.check_table(table)?;
        // MUTANT: a read no longer retires the direct one-put witness.
        if !self.charge(1) {
            return self.fail_closed(Error::TransactionTooLarge);
        }
        let worker = self.attachment.worker;
        let result = table.entry.table.get(self.inner_mut(), worker, key);
""",
        target=native("one_put_candidate_tracks_the_direct_witness"),
        test="one_put_candidate_tracks_the_direct_witness",
        breaks="A one-put record can be serialized for a transaction that also read.",
    ),
    Mutation(
        name="preflight-does-not-seal",
        relative_path="src/txn.rs",
        anchor="""        self.plan = Some(preflight);
        Ok(preflight)
    }
""",
        anchor_sha256="7d5c0cf321e60d486d54c3ea3817c8bfc23b51fc186b4f68e0db8189e20507c8",
        replacement="""        // MUTANT: the plan is sized but never sealed against later operations.
        Ok(preflight)
    }
""",
        target=native("general_record_terminal_serializes_the_canonical_write_set"),
        test="general_record_terminal_serializes_the_canonical_write_set",
        breaks="Operations after preflight can change the write set the record was sized for.",
    ),
    Mutation(
        name="arena-bound-not-published",
        relative_path="src/txn.rs",
        anchor="""        turn.store(bound, Ordering::Release);
        self.record = record;
        self.publication = publication;
        self.bound = true;
    }
""",
        anchor_sha256="8b200499e04ff3bdeb40c1e28c414810384b394cff764e317ad6e0a5975b3980",
        replacement="""        // MUTANT: the arena record is written without publishing BOUND.
        self.record = record;
        self.publication = publication;
        self.bound = true;
    }
""",
        target=native("concurrent_arena_and_holder_terminals_drive_the_publication_ring"),
        test="concurrent_arena_and_holder_terminals_drive_the_publication_ring",
        breaks="The consumer never sees the arena generation leave FREE.",
    ),
    Mutation(
        name="holder-ready-extent-untagged",
        relative_path="src/txn.rs",
        anchor="""            .write_unaligned(NATIVE_HOLDER_RECORD_TAG | exact);
""",
        anchor_sha256="069caa3e5985dbfa51097a7e338e229c4eeb93ef3f3b699f241558a5c423b796",
        replacement="""            // MUTANT: publish the holder extent without the holder tag.
            .write_unaligned(exact);
""",
        target=native("concurrent_arena_and_holder_terminals_drive_the_publication_ring"),
        test="concurrent_arena_and_holder_terminals_drive_the_publication_ring",
        breaks="The consumer reads the holder record as arena bytes.",
    ),
    Mutation(
        name="preselected-record-timestamp-hidden",
        relative_path="src/txn.rs",
        anchor="""                if !self.serialize(target.bytes().as_ptr()) {
                    return self.fail(Error::CommitHookRejected);
                }
                self.timestamp_stamp = self.minted_stamp;
                self.bound = true;
                self.written = true;
""",
        anchor_sha256="965805f0e725c0d62778f9631269bd973bf0098bb7bae9322b8c517832d3fd04",
        replacement="""                if !self.serialize(target.bytes().as_ptr()) {
                    return self.fail(Error::CommitHookRejected);
                }
                // MUTANT: accept the record without exposing its timestamp.
                self.bound = true;
                self.written = true;
""",
        target=native("single_producer_preselected_record_and_holder_terminals"),
        test="single_producer_preselected_record_and_holder_terminals",
        breaks="A written preselected record reports no accepted timestamp.",
    ),
    Mutation(
        name="budget-never-charged",
        relative_path="src/txn.rs",
        anchor="""    fn charge(&mut self, credits: usize) -> bool {
        match self.budget_used.checked_add(credits) {
""",
        anchor_sha256="947d0408c012af0fa70819f29fb22d40c8d8fc9fe9ba37affb13b63954846c40",
        replacement="""    fn charge(&mut self, credits: usize) -> bool {
        // MUTANT: the item budget is unbounded.
        return true;
        #[allow(unreachable_code)]
        match self.budget_used.checked_add(credits) {
""",
        target=native("item_budget_bounds_a_transaction"),
        test="item_budget_bounds_a_transaction",
        breaks="A transaction can exceed the 512-credit item budget the C++ engine enforces.",
    ),
    Mutation(
        name="crc-not-finalized",
        relative_path="src/record.rs",
        anchor="""        crc = CRC32C_TABLE[((crc ^ u32::from(*byte)) & 0xff) as usize] ^ (crc >> 8);
    }
    !crc
}
""",
        anchor_sha256="8600ab8f2d09ffdce1153472194b45902d143e1360cf4fe1c030e609eeef2730",
        replacement="""        crc = CRC32C_TABLE[((crc ^ u32::from(*byte)) & 0xff) as usize] ^ (crc >> 8);
    }
    // MUTANT: skip the final inversion.
    crc
}
""",
        target=LIB,
        test="record::tests::crc32c_matches_the_reference_vector",
        breaks="Every v5 record carries a checksum replay rejects.",
    ),
    Mutation(
        name="v6-magic-for-v5",
        relative_path="src/record.rs",
        anchor="""        CommitRecordChecksum::Crc32c => (&MAGIC_V5, VERSION_V5),
""",
        anchor_sha256="0073a86f5c4c643b77447de3efa2c6d187398f96a99d84c588d8af73372b9d56",
        replacement="""        // MUTANT: checksummed records carry the unchecked magic.
        CommitRecordChecksum::Crc32c => (&MAGIC_V6, VERSION_V5),
""",
        target=LIB,
        test="record::tests::v5_record_carries_a_crc_over_every_preceding_byte",
        breaks="Replay decodes a checksummed record as unchecked.",
    ),
    Mutation(
        name="abort-cleanup-poison-hidden",
        relative_path="src/txn.rs",
        anchor="            cleanup?;\n",
        anchor_sha256="f4102ac993fc93e16df3072f86662e2393b10fcdcd499b72dce9a29238ee1c3a",
        replacement="            // MUTANT: discard actual abort cleanup poison.\n",
        target=LIB,
        test="txn::poison_tests::explicit_abort_reports_real_core_cleanup_poison",
        breaks="A real STO cleanup panic is reported as a successful explicit abort.",
    ),
    Mutation(
        name="poisoned-begin-worker-not-retired",
        relative_path="src/txn.rs",
        anchor="""        BeginError::Poisoned(_) => {
            worker::poison_current_worker();
            Error::WorkerPoisoned
        }
""",
        anchor_sha256="414557ba3296ff1b21b005530e35200ef1348a698224fbf093e868ee823f788b",
        replacement="""        BeginError::Poisoned(_) => {
            // MUTANT: report runtime poison without retiring this worker.
            Error::WorkerPoisoned
        }
""",
        target=LIB,
        test="txn::poison_tests::actual_hook_poison_retires_existing_and_new_workers_and_releases_gate",
        breaks="An attached worker remains healthy after begin observes poisoned runtime state.",
    ),
    Mutation(
        name="poisoned-attach-worker-not-retired",
        relative_path="src/worker.rs",
        anchor="                    state.poison();\n",
        anchor_sha256="636fa680ee89c529282ebd0674884d6fb85cc5e872f84ecb50cbef1c8427978e",
        replacement="                    // MUTANT: leave a newly attaching worker reusable.\n",
        target=LIB,
        test="txn::poison_tests::actual_hook_poison_retires_existing_and_new_workers_and_releases_gate",
        breaks="A new worker remains reusable after attachment observes runtime poison.",
    ),
    Mutation(
        name="driver-drop-keeps-clock-gate",
        relative_path="src/txn.rs",
        anchor="""            self.gate_held = false;
            self.clock.leave_general();
            if self.spec.gate == Gate::Ticket {
""",
        anchor_sha256="55fae44fe0170f560ee72358c1be928f6ba4ef5dfe0a3b9e12473a9617b2f4a9",
        replacement="""            self.gate_held = false;
            // MUTANT: leave the clock gate locked after a hook failure.
            if self.spec.gate == Gate::Ticket {
""",
        target=LIB,
        test="txn::poison_tests::actual_hook_poison_retires_existing_and_new_workers_and_releases_gate",
        breaks="A pre-install hook failure leaves the shared clock gate held.",
    ),
)


class HarnessFailure(Exception):
    pass


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def source_hashes() -> dict[str, str]:
    # The copied engine still links these path dependencies from this tree.
    # Edits to them during a campaign invalidate its result as well.
    packages = ("mako-engine-sto", "mako-engine-api", "mako-clock", "sto-core",
                "sto-masstree", "masstree", "mtree-sys")
    paths = [ORIGINAL_CRATES / "Cargo.toml", ORIGINAL_LOCKFILE]
    for package in packages:
        directory = ORIGINAL_CRATES / package
        paths.extend(path for name in ("Cargo.toml", "build.rs")
                     if (path := directory / name).is_file())
        for name in ("src", "tests", "mutations"):
            paths.extend(path for path in (directory / name).rglob("*")
                         if path.is_file() and path.suffix in (".rs", ".toml", ".py"))
    hashes = {path.relative_to(ORIGINAL_CRATES).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in sorted(paths)}
    hashes["../rust-toolchain.toml"] = hashlib.sha256(ORIGINAL_TOOLCHAIN.read_bytes()).hexdigest()
    return hashes


def validate_definitions() -> None:
    names = [mutation.name for mutation in MUTATIONS]
    if len(set(names)) != len(names):
        raise HarnessFailure("duplicate mutation names")
    for mutation in MUTATIONS:
        source = (ORIGINAL_CRATE / mutation.relative_path).read_text(encoding="utf-8")
        count = source.count(mutation.anchor)
        if count != 1:
            raise HarnessFailure(
                f"{mutation.name}: anchor occurs {count} times in {mutation.relative_path}, expected 1"
            )
        digest = sha256_text(mutation.anchor)
        if digest != mutation.anchor_sha256:
            raise HarnessFailure(
                f"{mutation.name}: anchor sha256 is {digest}, definition pins {mutation.anchor_sha256!r}"
            )
        if mutation.replacement == mutation.anchor:
            raise HarnessFailure(f"{mutation.name}: replacement equals anchor")
        test_name = mutation.test.rsplit("::", 1)[-1]
        haystacks = [
            (ORIGINAL_CRATE / "tests" / "native.rs").read_text(encoding="utf-8"),
            (ORIGINAL_CRATE / "src" / "record.rs").read_text(encoding="utf-8"),
            (ORIGINAL_CRATE / "src" / "txn_poison_tests.rs").read_text(encoding="utf-8"),
            (ORIGINAL_CRATE / "src" / "txn_ownership_tests.rs").read_text(encoding="utf-8"),
            (ORIGINAL_CRATE / "src" / "txn_local_version_tests.rs").read_text(encoding="utf-8"),
        ]
        if not any(f"fn {test_name}(" in haystack for haystack in haystacks):
            raise HarnessFailure(f"{mutation.name}: test {mutation.test} is not defined")


def create_workspace(label: str, temp_root: Path) -> Path:
    temp_root.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=f"mako-engine-sto-mut-{label}-", dir=temp_root))
    crate = root / "mako-engine-sto"
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
        raise HarnessFailure("mako-engine-sto manifest has no path dependencies to isolate")
    manifest.write_text(rewritten, encoding="utf-8")
    # The differential test includes the corpus fixture by a path relative to
    # the original crate; it is not a mutation killer, so drop it here.
    (crate / "tests" / "differential.rs").unlink()
    original_workspace = (ORIGINAL_CRATES / "Cargo.toml").read_text(encoding="utf-8")
    package_section = re.search(r"\[workspace\.package\][^\[]*", original_workspace)
    if package_section is None:
        raise HarnessFailure("original workspace has no [workspace.package] table")
    (root / "Cargo.toml").write_text(
        '[workspace]\nresolver = "2"\nmembers = ["mako-engine-sto"]\n\n'
        + package_section.group(0)
        + "\n",
        encoding="utf-8",
    )
    shutil.copy2(ORIGINAL_LOCKFILE, root / "Cargo.lock")
    # An isolated directory no longer inherits the repository's compiler pin.
    # Preserve it rather than silently using the host's default stable channel.
    shutil.copy2(ORIGINAL_TOOLCHAIN, root / "rust-toolchain.toml")
    return root


def run(command: Sequence[str], cwd: Path, env: dict[str, str], timeout: int) -> dict[str, object]:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            list(command), cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout, check=False
        )
    except subprocess.TimeoutExpired as error:
        raise HarnessFailure(f"command exceeded {timeout}s in {cwd}: {' '.join(command)}") from error
    except OSError as error:
        raise HarnessFailure(f"cannot execute command in {cwd}: {error}") from error
    return {
        "command": list(command),
        "returncode": completed.returncode,
        "seconds": round(time.monotonic() - started, 3),
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def normalize_lockfile(cargo: str, root: Path, env: dict[str, str], timeout: int) -> None:
    result = run([cargo, "metadata", "--offline", "--format-version", "1"], root, env, timeout)
    if result["returncode"] != 0:
        raise HarnessFailure("lockfile normalization failed: " + str(result["stderr"])[-2000:])


def test_command(cargo: str, target: tuple[str, ...], test: str) -> list[str]:
    return [
        cargo,
        "test",
        "--locked",
        "--offline",
        "-p",
        "mako-engine-sto",
        "--all-features",
        *target,
        test,
        "--",
        "--exact",
        "--nocapture",
        "--test-threads=1",
    ]


def environment(root: Path) -> dict[str, str]:
    env = dict(os.environ)
    target = root / "cargo-target-empty"
    if target.exists():
        raise HarnessFailure(f"target directory unexpectedly exists: {target}")
    env["CARGO_TARGET_DIR"] = str(target)
    env.pop("RUSTFLAGS", None)
    env.pop("CARGO_ENCODED_RUSTFLAGS", None)
    if env.get("MAKO_MTREE_NATIVE_INTEGRATION") != "1":
        raise HarnessFailure(
            "MAKO_MTREE_NATIVE_INTEGRATION=1 (plus MAKO_MTREE_NATIVE_LIB_DIRS and MAKO_MTREE_NATIVE_LIBS) is required"
        )
    return env


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo", default=os.environ.get("CARGO", "cargo"))
    parser.add_argument("--temp-root", default=os.environ.get("MAKO_ENGINE_STO_MUTATION_TMP", ""))
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--check", action="store_true", help="validate all pinned anchors without running Cargo")
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--report", default=None)
    parser.add_argument("--timeout", type=int, default=2400)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    temp_root = Path(args.temp_root) if args.temp_root else Path(tempfile.gettempdir())

    before = source_hashes()
    report: dict[str, object] = {
        "crate": str(ORIGINAL_CRATE),
        "toolchain_pin_sha256": hashlib.sha256(ORIGINAL_TOOLCHAIN.read_bytes()).hexdigest(),
        "rustup_toolchain_override": os.environ.get("RUSTUP_TOOLCHAIN"),
        "source_sha256": {
            name: sha256_text((ORIGINAL_CRATE / name).read_text(encoding="utf-8"))
            for name in ("src/txn.rs", "src/record.rs", "src/holder.rs", "src/db.rs", "src/worker.rs", "src/txn_poison_tests.rs")
        },
        "mutations": [],
    }
    try:
        validate_definitions()
        selected = [m for m in MUTATIONS if not args.only or m.name in args.only]
        if args.only and len(selected) != len(set(args.only)):
            raise HarnessFailure(f"unknown mutation names in {args.only}")
        if args.check:
            report.update(verdict="checked", total=len(selected))
            output = json.dumps(report, indent=2, sort_keys=True)
            print(output)
            if args.report:
                Path(args.report).write_text(output + "\n", encoding="utf-8")
            return 0

        root = create_workspace("baseline", temp_root)
        env = environment(root)
        toolchain = {}
        for name, command in (("rustc", [env.get("RUSTC", "rustc"), "--version", "--verbose"]),
                              ("cargo", [args.cargo, "--version", "--verbose"])):
            version = run(command, root, env, args.timeout)
            if version["returncode"] != 0:
                raise HarnessFailure(f"cannot identify baseline {name}: {version['stderr']}")
            toolchain[name] = str(version["stdout"]).strip()
        report["baseline_toolchain"] = toolchain
        normalize_lockfile(args.cargo, root, env, args.timeout)
        baseline: list[dict[str, object]] = []
        for target, test in sorted({(m.target, m.test) for m in selected}):
            print(f"[baseline] {test}", file=sys.stderr, flush=True)
            result = run(test_command(args.cargo, target, test), root, env, args.timeout)
            ok = result["returncode"] == 0 and f"test {test} ... ok" in str(result["stdout"])
            baseline.append({"test": test, "ok": ok, "seconds": result["seconds"]})
            if not ok:
                raise HarnessFailure(
                    f"baseline test {test} did not pass:\n{str(result['stdout'])[-3000:]}\n{str(result['stderr'])[-3000:]}"
                )
        report["baseline"] = baseline
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)

        killed = 0
        for mutation in selected:
            print(f"[mutant] {mutation.name} -> {mutation.test}", file=sys.stderr, flush=True)
            root = create_workspace(mutation.name, temp_root)
            env = environment(root)
            normalize_lockfile(args.cargo, root, env, args.timeout)
            path = root / "mako-engine-sto" / mutation.relative_path
            source = path.read_text(encoding="utf-8")
            if source.count(mutation.anchor) != 1:
                raise HarnessFailure(f"{mutation.name}: copied anchor count != 1")
            path.write_text(source.replace(mutation.anchor, mutation.replacement, 1), encoding="utf-8")
            result = run(test_command(args.cargo, mutation.target, mutation.test), root, env, args.timeout)
            stdout = str(result["stdout"])
            stderr = str(result["stderr"])
            if "error[E" in stderr or "could not compile" in stderr:
                raise HarnessFailure(f"{mutation.name}: mutant did not compile:\n{stderr[-3000:]}")
            combined = stdout + stderr
            own_panic = f"thread '{mutation.test}'" in combined and "panicked at" in combined
            was_killed = result["returncode"] != 0 and (mutation.failure_signature in stdout or own_panic)
            survived = result["returncode"] == 0 and f"test {mutation.test} ... ok" in stdout
            if not was_killed and not survived:
                raise HarnessFailure(
                    f"{mutation.name}: neither killed nor survived (rc {result['returncode']}):\n{stdout[-3000:]}\n{stderr[-3000:]}"
                )
            killed += was_killed
            report["mutations"].append(
                {
                    "name": mutation.name,
                    "test": mutation.test,
                    "breaks": mutation.breaks,
                    "outcome": "killed" if was_killed else "SURVIVED",
                    "seconds": result["seconds"],
                    "workspace": str(root) if args.keep else None,
                }
            )
            print(
                f"[mutant] {mutation.name}: {'killed' if was_killed else 'SURVIVED'}",
                file=sys.stderr,
                flush=True,
            )
            if not args.keep:
                shutil.rmtree(root, ignore_errors=True)
        report["killed"] = killed
        report["total"] = len(selected)
        report["verdict"] = "pass" if killed == len(selected) else "fail"
    except (HarnessFailure, OSError) as failure:
        report["verdict"] = "harness-error"
        report["error"] = str(failure)
    after = source_hashes()
    changed = sorted(path for path in before.keys() | after.keys() if before.get(path) != after.get(path))
    report["source_integrity"] = {"unchanged": not changed, "changed_paths": changed, "file_sha256": before}
    if changed:
        report["verdict"] = "harness-error"
        report["error"] = "engine or path-dependency sources changed during the campaign"
    output = json.dumps(report, indent=2, sort_keys=True)
    print(output)
    if args.report:
        Path(args.report).write_text(output + "\n", encoding="utf-8")
    return 0 if report.get("verdict") == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
