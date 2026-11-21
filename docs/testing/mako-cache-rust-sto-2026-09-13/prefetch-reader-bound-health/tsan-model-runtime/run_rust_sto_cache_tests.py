#!/usr/bin/env python3
"""Run the native Rust STO adapter/cache tests with explicit engine selection.

Sanitized invocations retain their compiler instrumentation. ASan runs normal
tests with leak detection, then runs each named intentional-quarantine test
separately with only leak reporting disabled. The adapter/cache runs do not
inherit the broad legacy Masstree TSan or UBSan suppression files. ASan exempts
only two documented process-lifetime worker allocation roots and first proves
that an unrelated lost transaction buffer still fails leak detection.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


POISON_TESTS = tuple(
    f"txn::poison_tests::{name}"
    for name in (
        "explicit_abort_reports_real_core_cleanup_poison",
        "dropped_transaction_retires_worker_after_real_core_cleanup_panic",
        "rejected_record_preserves_real_abort_cleanup_uncertainty",
        "failed_operation_preserves_real_abort_cleanup_uncertainty",
        "committed_cleanup_panic_keeps_the_definite_commit",
        "actual_hook_poison_retires_existing_and_new_workers_and_releases_gate",
        "post_install_panic_keeps_record_bound_and_outcome_unknown",
        "mid_install_panic_is_indeterminate_and_does_not_emit_all_installed",
    )
)
QUARANTINE = {
    "sto-masstree": [
        ("--lib", "local_version_tests::indeterminate_local_release_ignores_mixed_ordered_id"),
    ],
    "mako-engine-sto": [("--lib", name) for name in POISON_TESTS],
    "mako-cache": [
        ("cleanup", "cleanup_uncertainty_quarantines_fresh_cache_workers"),
        ("--lib", "tests::poisoned_sto_runtime_makes_cache_unhealthy_from_another_worker"),
    ],
}
PROFILES = {
    "sto-masstree": ["--all-features", "--lib", "local_version_tests::"],
    "mako-engine-sto": ["--all-features", "--lib", "--test", "native", "--test", "value_boundaries"],
    "mako-cache": ["--no-default-features", "--features", "engine-sto-test-hooks,test-support", "--all-targets"],
}
REQUIRED_MASSTREE_SUITES = (
    ("native local versions", ["--all-features", "--test", "local_versions_native"], False, frozenset({
        "native_fixed_u64_rejects_record_local_policy_before_directory_creation",
        "native_local_cross_worker_same_key_and_phantom_conflicts",
        "native_local_fixed_batches_terminal_reads_and_scan_read_your_writes",
        "native_local_versions_mix_with_ordered_tables_and_cells",
    })),
    ("fixed-u64 admission", ["--all-features", "--lib",
                            "fixed_u64::tests::local_version_option_is_rejected_before_object_registration"],
     True, frozenset({"fixed_u64::tests::local_version_option_is_rejected_before_object_registration"})),
)
LSAN_TRIPWIRE = "worker::lsan_tests::lost_transaction_log_is_reported_with_startup_exceptions"
LSAN_ROOTS = ("mako_engine_sto::worker::retain_process_lifetime_native_worker",
              "mako_engine_sto::worker::retain_process_lifetime_worker_slot")
ADDRESS_MODEL_SUITES = ("commit_fence_loom", "occupancy_credit_loom")


def options_without_suppressions(options: str) -> str:
    return ":".join(item for item in options.split(":") if item and not item.startswith("suppressions="))


def test_outcomes(output: str) -> tuple[set[str], set[str]]:
    outcomes = re.findall(r"^test ([^\r\n]+?) \.\.\. (ok|ignored(?:,[^\r\n]*)?)$", output, re.MULTILINE)
    passed = {name.removesuffix(" - should panic") for name, result in outcomes if result == "ok"}
    # Abort-contract tests intentionally spawn a child that dies before printing
    # its own result. The serialized parent prints its successful result next.
    passed.update(name.removesuffix(" - should panic") for name in re.findall(
        r"^test ([^\r\n]+?) \.\.\. \n(?:(?!^test |^test result:)[^\n]*\n)*?ok$", output, re.MULTILINE))
    return passed, {name for name, result in outcomes if result.startswith("ignored")}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--manifest-path", required=True)
    parser.add_argument("--target")
    parser.add_argument("-Zbuild-std", action="store_true", dest="build_std")
    parser.add_argument("--report", required=True)
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    env = dict(os.environ)
    env.pop("CARGO_BUILD_TARGET", None)
    env.pop("CARGO_ENCODED_RUSTFLAGS", None)
    for name in ("LSAN_OPTIONS", "TSAN_OPTIONS", "UBSAN_OPTIONS"):
        env[name] = options_without_suppressions(env.get(name, ""))
    address = "-Zsanitizer=address" in env.get("RUSTFLAGS", "")
    thread_sanitizer = "-Zsanitizer=thread" in env.get("RUSTFLAGS", "")
    separate_models = address or thread_sanitizer
    if address:
        options = options_without_suppressions(env.get("ASAN_OPTIONS", ""))
        options = ":".join(item for item in options.split(":") if item and not item.startswith("detect_leaks="))
        env["ASAN_OPTIONS"] = options + ":detect_leaks=1:halt_on_error=1"
        suppression = Path(__file__).with_name("rust_sto_cache_lsan.supp")
        entries = [line for line in suppression.read_text().splitlines() if line and not line.startswith("#")]
        if entries != [f"leak:{name}" for name in LSAN_ROOTS]:
            parser.error("LSan suppression file must contain only the two reviewed startup allocation roots")
        env["LSAN_OPTIONS"] = env["LSAN_OPTIONS"] + f":suppressions={suppression}:print_suppressions=1"
    report = {
        "format": "mako-rust-sto-cache-tests-v1",
        "native": env.get("MAKO_MTREE_NATIVE_INTEGRATION") == "1",
        "rustflags": env.get("RUSTFLAGS", ""),
        "native_library_dirs": env.get("MAKO_MTREE_NATIVE_LIB_DIRS", ""),
        "native_libraries": env.get("MAKO_MTREE_NATIVE_LIBS", ""),
        "target": args.target,
        "rebuilt_std": args.build_std,
        "sanitizer_suppressions": list(LSAN_ROOTS) if address else [],
        "suppression_file_sha256": hashlib.sha256(suppression.read_bytes()).hexdigest() if address else None,
        "address_model_exclusions": list(ADDRESS_MODEL_SUITES) if address else [],
        "address_model_qualification": "generator-0.8.9 switches stacks without ASan fiber hooks; require separate unsanitized model execution" if address else None,
        "thread_model_exclusions": list(ADDRESS_MODEL_SUITES) if thread_sanitizer else [],
        "thread_model_qualification": "generator-0.8.9 switches stacks without TSan fiber hooks; exhaustive model execution exhausts the TSan shadow call stack; require separate unsanitized model execution" if thread_sanitizer else None,
        "runs": [],
    }
    started = time.monotonic()

    def run(command, environment, label, leak_check, expect_lsan=False):
        print(f"[adapter/cache] {label}", file=sys.stderr, flush=True)
        result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=args.timeout, check=False)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        passed, ignored = test_outcomes(result.stdout)
        report["runs"].append({"label": label, "command": command, "returncode": result.returncode, "leak_check": leak_check,
                               "expected_lsan_failure": expect_lsan,
                               "passed_tests": sorted(passed), "ignored_tests": sorted(ignored)})
        if expect_lsan:
            if (result.returncode == 0 or LSAN_TRIPWIRE not in passed
                    or "ERROR: LeakSanitizer: detected memory leaks" not in result.stderr
                    or "TouchLog" not in result.stderr):
                raise RuntimeError("LSan lost-transaction-buffer negative control did not report its leak")
            return result.stdout
        if result.returncode != 0:
            raise RuntimeError(f"{label} failed with status {result.returncode}")
        return result.stdout

    try:
        if not report["native"]:
            raise RuntimeError("native adapter/cache gate requires MAKO_MTREE_NATIVE_INTEGRATION=1")
        platform = (["--target", args.target] if args.target else []) + (["-Zbuild-std"] if args.build_std else [])
        if separate_models:
            model_env = dict(env)
            for name in ("RUSTFLAGS", "RUSTDOCFLAGS", "ASAN_OPTIONS", "LSAN_OPTIONS", "TSAN_OPTIONS", "UBSAN_OPTIONS",
                         "MAKO_BUILD_DIR", "MAKO_LOCAL_REQUIRE_NATIVE", "MAKO_MTREE_NATIVE_INTEGRATION",
                         "MAKO_MTREE_NATIVE_LIB_DIRS", "MAKO_MTREE_NATIVE_LIBS"):
                model_env.pop(name, None)
            model_env["CARGO_TARGET_DIR"] = str(Path(args.report).resolve().parent / "rust-model-target")
            model_tests = [part for suite in ADDRESS_MODEL_SUITES for part in ("--test", suite)]
            model_base = [args.cargo, "test", "--manifest-path", args.manifest_path, "--locked", "-p", "mako-cache",
                          "--no-default-features", "--features", "engine-sto-test-hooks,test-support", *model_tests]
            listing = run([*model_base, "--", "--list", "--format", "terse"], model_env, "unsanitized Loom model inventory", "not-address")
            expected_models = {line.removesuffix(": test") for line in listing.splitlines() if line.endswith(": test")}
            output = run([*model_base, "--", "--test-threads=1"], model_env, "unsanitized Loom models", "not-address")
            model_passed, _ = test_outcomes(output)
            if not expected_models or not expected_models.issubset(model_passed):
                raise RuntimeError("separate unsanitized Loom models did not all execute")
        if address:
            run([args.cargo, "test", "--manifest-path", args.manifest_path, "--locked", *platform,
                 "-p", "mako-engine-sto", "--all-features", "--lib", LSAN_TRIPWIRE,
                 "--", "--exact", "--ignored", "--test-threads=1"], env,
                "lost transaction buffer negative control", "enabled", expect_lsan=True)
        for package, profile in PROFILES.items():
            if separate_models and package == "mako-cache":
                tests_dir = Path(args.manifest_path).resolve().parent / "mako-cache" / "tests"
                suites = sorted(path.stem for path in tests_dir.glob("*.rs") if path.stem not in ADDRESS_MODEL_SUITES)
                profile = [*profile[:-1], "--lib", *[part for suite in suites for part in ("--test", suite)]]
            base = [args.cargo, "test", "--manifest-path", args.manifest_path, "--locked", *platform, "-p", package]
            quarantine = QUARANTINE[package] if address else []
            listing = run([*base, *profile, "--", "--list", "--format", "terse"], env, package + " test inventory", "enabled" if address else "not-address")
            names = [line.removesuffix(": test") for line in listing.splitlines() if line.endswith(": test")]
            if not names:
                raise RuntimeError(f"{package} listed no native tests")
            for _, name in quarantine:
                matches = [candidate for candidate in names if name in candidate]
                if matches != [name]:
                    raise RuntimeError(f"quarantine selection {name!r} matches {matches!r}, expected exactly one test")
            skips = [argument for _, name in quarantine for argument in ("--skip", name)]
            output = run([*base, *profile, "--", "--test-threads=1", *skips], env, package + " ordinary tests", "enabled" if address else "not-address")
            passed, ignored = test_outcomes(output)
            allowed_ignored = {LSAN_TRIPWIRE} if package == "mako-engine-sto" else set()
            unexpected_ignored = ignored - allowed_ignored
            if unexpected_ignored:
                raise RuntimeError(f"{package} has unexpected ignored tests: {sorted(unexpected_ignored)}")
            expected = set(names) - {name for _, name in quarantine}
            missing = expected - passed - ignored
            if missing or not (passed & expected):
                raise RuntimeError(f"{package} ordinary test execution incomplete: missing={sorted(missing)}, passed={len(passed & expected)}")
            for suite, name in quarantine:
                isolated_env = dict(env)
                isolated_env["ASAN_OPTIONS"] = env["ASAN_OPTIONS"].replace("detect_leaks=1", "detect_leaks=0")
                target = ["--lib"] if suite == "--lib" else ["--test", suite]
                features = ["--all-features"] if package != "mako-cache" else ["--no-default-features", "--features", "engine-sto-test-hooks,test-support"]
                output = run([*base, *features, *target, name, "--", "--exact", "--test-threads=1"], isolated_env, package + " " + name, "disabled-exact-intentional-quarantine")
                if f"test {name} ... ok" not in output:
                    raise RuntimeError(f"exact quarantine rerun did not execute {name}")
            if package == "sto-masstree":
                for label, required_profile, exact, required_names in REQUIRED_MASSTREE_SUITES:
                    harness = ["--exact"] if exact else []
                    label = package + " " + label
                    listing = run([*base, *required_profile, "--", *harness, "--list", "--format", "terse"],
                                  env, label + " inventory", "enabled" if address else "not-address")
                    names = [line.removesuffix(": test") for line in listing.splitlines() if line.endswith(": test")]
                    if set(names) != required_names or len(names) != len(required_names):
                        raise RuntimeError(f"{label} inventory mismatch: expected={sorted(required_names)}, got={names}")
                    output = run([*base, *required_profile, "--", *harness, "--test-threads=1"],
                                 env, label, "enabled" if address else "not-address")
                    passed, ignored = test_outcomes(output)
                    if passed != required_names or ignored:
                        raise RuntimeError(f"{label} execution incomplete: expected={sorted(required_names)}, passed={sorted(passed)}, ignored={sorted(ignored)}")
        report["verdict"] = "pass"
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        report["verdict"] = "fail"
        report["error"] = str(error)
        print(error, file=sys.stderr)
    report["seconds"] = round(time.monotonic() - started, 3)
    Path(args.report).write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["verdict"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
