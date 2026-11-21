#!/usr/bin/env python3
"""Audit retained 2d3504a capacity evidence, before the diagnostic-only fix.

Run with Python 3.10+ and Git. Recorded absolute paths are provenance only;
all evidence reads are relative to --evidence-dir. Sources come from the pinned
commit, never the current working tree. This checks retained records, not
unretained binaries or a fresh execution of the original tests. It does not
verify the later a3ad9a1 relink, repeated growth run, or performance sweep.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


COMMIT = "2d3504a277b3ee66c8fea10099accd2290884f31"
HERE = Path(__file__).resolve().parent
PINS = """
5c4ed72ddf4701e6d92f9d2dde3d58e98ca34af1a04bb3904a6d32a28cb988f6 final-build/ctest-release.log
cd3c99c415052cea7666e236f48e34e6f08cf4713305ef08c475c625631effd4 final-build/growth-180s.log
9c772fe5e4ab1ddf9740132e97d1b94ea1e759cc5dc98295f71963e652562d87 final-build/miri-artifacts.sha256
90d0603c57bdd7cdfb8cefca541088e2432b5c07bd997c3f53aad68c049dc29c final-build/miri-full.log
588732cd938269b0657f774b7c2b88df47ef5b60f12db986f40fe773b9a6431e final-build/miri-source.sha256
a2af9a3f44ae876d5089d2b40eb67cb72777a5d74cf0639c6dc9d62d41bf0287 final-build/pgo-artifacts-sha256.txt
27021b3dbe61c983895d37bba75f996647747c9ddd411bd644048907c6e91d70 final-build/pgo-provenance.txt
97231e076237877c6b870713a025e129bb45d58ccf12054dbce3ee1f84df909f asan-v4/rust-sto-sanitizer-manifest.txt
dc87a948e1e1916df9302bf59a47b9fcc8243b7e6a87bba05b4be85f960cc5d0 asan-v4/ctest-masstree-c11-header.log
5397e550c6d76c731d56e48ece919e4770def76a3534774d421641f67e770f9c asan-v4/ctest-native-process-lifetime-lifecycle.log
c2d167ccbc96d94db82620df1c2765b35b4ea981890c7999c12cec73c6a0d61e asan-v4/ctest-rust-boundary.log
aa6386aa1811b981621060269bd71a5389bc5ba7f38b3921d48a2c336cdf74bb asan-v4/ctest-silo-critical-default-profile.log
d8f8519f212fa6c527d7de63cddf50125e9d7feebb5c43a51d62ce62ea64dacd asan-v4/ctest-silo-runtime-concurrency-lifecycle.log
7de0ab19fe9580d5791cf6ff6cce6c7c05798d4499e437a52fcd4ce51a82d9e9 asan-v4/ctest-sto-rmw-insert-delete-address.log
1121b9d4cc564712daaf2b245d9b6aa6afc2791ba6f1e28676005ae8bbc449b7 asan-v4/ctest-sto-rmw-resurrection-delete-address.log
"""
QUALIFIED = (
    ("test_sto_tpcc_rust_slow_exit", "ctest-rust-slow-exit-lsan.log", 4, 1280),
    ("test_sto_tpcc_rust_concurrent", "ctest-rust-concurrent-lsan.log", 16, 5120),
    ("test_sto_tpcc_rust_run_resource_exhausted", "ctest-rust-run-exhausted-lsan.log", 4, 1280),
    ("test_sto_tpcc_rust_concurrent_resource_exhausted", "ctest-rust-concurrent-exhausted-lsan.log", 16, 5120),
)


class AuditError(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AuditError(message)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fields(text: str) -> dict[str, str]:
    result = {}
    for line in text.splitlines():
        if not line:
            continue
        key, separator, value = line.partition("=")
        require(bool(separator) and key not in result, f"invalid/duplicate manifest field: {line}")
        result[key] = value
    return result


def ctest_pass(text: str, count: int) -> set[str]:
    summaries = re.findall(r"^100% tests passed, 0 tests failed out of (\d+)$", text, re.M)
    rows = re.findall(r"^\s*\d+/\d+ Test\s+#\d+:\s+(\S+).*?\bPassed\b", text, re.M)
    require(summaries == [str(count)], f"expected one passing {count}-test CTest summary")
    require(len(rows) == len(set(rows)) == count, f"expected {count} distinct passing CTests")
    return set(rows)


def lsan_table(text: str, count: int, byte_count: int) -> None:
    lines = [re.sub(r"^\s*\d+:\s?", "", line).strip() for line in text.splitlines()]
    starts = [i for i, line in enumerate(lines) if line == "Suppressions used:"]
    require(len(starts) == 1, "expected exactly one LSan suppression table")
    rows = []
    for line in lines[starts[0] + 1 :]:
        if re.fullmatch(r"-{5,}", line):
            break
        if not line or re.fullmatch(r"count\s+bytes\s+template", line):
            continue
        match = re.fullmatch(r"(\d+)\s+(\d+)\s+(\S+)", line)
        require(match is not None, f"unexpected LSan row: {line}")
        rows.append((int(match[1]), int(match[2]), match[3]))
    else:
        raise AuditError("unterminated LSan suppression table")
    require(rows == [(count, byte_count, "mt_tree::mt_tree")], f"unexpected LSan allocations: {rows}")


def audit(evidence: Path, repo: Path) -> None:
    texts: dict[str, str] = {}

    def checked(relative: str, expected: str) -> str:
        data = (evidence / relative).read_bytes()
        require(digest(data) == expected, f"SHA-256 mismatch: {relative}")
        texts[relative] = data.decode("utf-8")
        return texts[relative]

    for line in PINS.splitlines():
        if line:
            expected, relative = line.split()
            checked(relative, expected)

    manifest = fields(texts["asan-v4/rust-sto-sanitizer-manifest.txt"])
    sources = {}
    for line in texts["final-build/miri-source.sha256"].splitlines():
        expected, relative = line.split()
        require(relative not in sources, f"duplicate source hash: {relative}")
        sources[relative] = expected
    require(len(sources) == 5, "expected five final Miri source hashes")
    sources.update({
        "scripts/ci/run_rust_sto_sanitizer.sh": "66bcbc6222e0360be82125adb422fc64b4aa23020c2348147b8b74faeb7448c3",
        "src/masstree/lsan_suppressions.txt": manifest["suppression_sha256"],
    })
    require(sources["scripts/ci/rust_sto_quarantine_tests.txt"] ==
            manifest["asan_quarantine_allowlist_sha256"], "quarantine source hashes disagree")
    for relative, expected in sources.items():
        content = subprocess.check_output(
            ["git", "-C", str(repo), "show", f"{COMMIT}:{relative}"],
            stderr=subprocess.PIPE,
        )
        require(digest(content) == expected, f"committed source hash mismatch: {relative}")

    miri = re.findall(r"^test result: (ok|FAILED)\. (\d+) passed; (\d+) failed;",
                      texts["final-build/miri-full.log"], re.M)
    require(len(miri) == 37 and sum(int(row[1]) for row in miri) == 481 and
            all(row[0] == "ok" and int(row[2]) == 0 for row in miri),
            "final Miri must report 481 passes, 37 groups, and zero failures")
    release = ctest_pass(texts["final-build/ctest-release.log"], 30)
    capacity_tests = {
        f"test_sto_tpcc_rust_{phase}_resource_exhausted"
        for phase in ("startup", "load", "run", "concurrent")
    }
    require(capacity_tests <= release, "release CTests omit capacity cases")

    for key, expected in {
        "gate_status": "passed", "sanitizer": "address", "rust_build_std": "ON",
        "sto_rmw_profile_gate": "passed", "sto_rmw_compile_definition": "READ_MY_WRITES=1",
        "sto_rmw_sanitizer": "-fsanitize=address", "lsan_suppression_rule": "leak:mt_tree::mt_tree",
        "asan_ctest_rust_label_leak_detection": "enabled",
        "asan_ctest_remaining_rust_label_suppressions": "none",
        "lsan_expected_total": "allocations:40,bytes:12800",
        "lsan_observed_total": "allocations:40,bytes:12800",
    }.items():
        require(manifest.get(key) == expected, f"ASan manifest mismatch: {key}")
    require(manifest["lsan_suppression_scope"].split(";") == [row[0] for row in QUALIFIED],
            "ASan suppression scope is not exactly the four qualified tests")
    for name, filename, count, byte_count in QUALIFIED:
        expected = f"allocations:{count},bytes:{byte_count}"
        require(manifest[f"lsan_expected_{name}"] == manifest[f"lsan_observed_{name}"] == expected,
                f"ASan allocation counters disagree: {name}")
        text = checked(f"asan-v4/{filename}", manifest[f"lsan_evidence_{name}_sha256"])
        require(ctest_pass(text, 1) == {name}, f"wrong qualified CTest: {name}")
        lsan_table(text, count, byte_count)
    require(sum(row[2] for row in QUALIFIED) == 40 and sum(row[3] for row in QUALIFIED) == 12800,
            "internal LSan expected-total mismatch")
    remaining = ctest_pass(texts["asan-v4/ctest-rust-boundary.log"], 14)
    require({"test_sto_tpcc_rust_startup_resource_exhausted",
             "test_sto_tpcc_rust_load_resource_exhausted"} <= remaining,
            "unsuppressed ASan CTests omit startup/load exhaustion")
    for filename, count in (
        ("ctest-masstree-c11-header.log", 1),
        ("ctest-native-process-lifetime-lifecycle.log", 6),
        ("ctest-silo-critical-default-profile.log", 4),
        ("ctest-silo-runtime-concurrency-lifecycle.log", 1),
        ("ctest-sto-rmw-insert-delete-address.log", 1),
        ("ctest-sto-rmw-resurrection-delete-address.log", 1),
    ):
        ctest_pass(texts[f"asan-v4/{filename}"], count)

    growth = texts["final-build/growth-180s.log"]
    records = re.findall(r"^TPCC_BENCH_RESULT (.+)$", growth, re.M)
    require(len(records) == 1, "growth log must contain one benchmark result")
    result = json.loads(records[0])
    for key, expected in {"schema_version": 1, "engine": "rust", "threads": 1,
                          "warehouses": 1, "configured_seconds": 180,
                          "commits": 10221020, "aborts": 0, "attempts": 10221020}.items():
        require(type(result.get(key)) is type(expected) and result[key] == expected,
                f"growth result mismatch: {key}")
    require(sum(result["mix"].values()) == result["commits"], "growth mix does not sum to commits")
    require(re.findall(r"^\s*Exit status:\s*(\d+)\s*$", growth, re.M) == ["0"],
            "growth process did not report exactly one successful exit")
    require("TPCC_RESOURCE_EXHAUSTED" not in growth, "growth run exhausted a resource")
    usage = [dict(token.split("=", 1) for token in line.split()[1:])
             for line in growth.splitlines()
             if line.startswith("STO_TPCC_CAPACITY phase=run_complete ")]
    databases = [row for row in usage if row.get("scope") == "database"]
    order_lines = [row for row in usage if row.get("table") == "order_line_0"]
    require(len(databases) == len(order_lines) == 1, "growth final usage snapshots missing/duplicated")
    database, order_line = databases[0], order_lines[0]
    for key, expected in {"retained_records": 46293881, "consumed_record_ids": 46293881,
                          "retained_key_bytes": 740702096}.items():
        require(int(order_line[key]) == expected, f"growth order-line mismatch: {key}")
    allocated, maximum = int(database["allocated_registry_bytes"]), int(database["max_registry_bytes"])
    require(maximum == 8 * 1024**3 and 0 <= allocated <= maximum and
            int(database["registry_headroom_bytes"]) == maximum - allocated,
            "growth structural registry usage exceeds or misreports the 8 GiB budget")
    print(f"PASS: {len(texts)} core evidence hashes; {len(sources)} sources at {COMMIT}")
    print("PASS: Miri 481/37; release CTests 30; ASan gate and exact LSan 40/12800; 180s growth")
    print("Scope: retained 2d3504a records; existing sanitizer qualifications remain. No performance verdict.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-dir", type=Path, default=HERE)
    parser.add_argument("--repo", type=Path, default=HERE.parents[2])
    args = parser.parse_args()
    try:
        audit(args.evidence_dir, args.repo)
    except (AuditError, OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
