#!/usr/bin/env python3
"""Check retained final-relink evidence without reading remote machine paths."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


REVISION = "a3ad9a110727f5ecc937cbeee23fe40152df719f"
MANIFEST_SHA = "a97a4003bfdeb0db19ff1fb883ec27061043b44bd66509f4ee9b08228b8a5753"
BINARY_SHA = "3e32ba7d47a6de1b080d14cdc4955d04ecee0ece32a969a2dcfdf3925a2e31fe"
ARCHIVE_SHA = "7d29c5077d9a62b9bdce4c4b1a7f1c7eecc206fc7cc92138e8b921321a579b87"
PROFILE_SHA = "c14043bbd29a8f74459bb2ed1bfcb98917313ee508fd4c2a7f2a9412dadea62b"
SMOKES = {
    "test_sto_tpcc_rust_startup_resource_exhausted": "startup",
    "test_sto_tpcc_rust_load_resource_exhausted": "load",
    "test_sto_tpcc_rust_run_resource_exhausted": "run",
    "test_sto_tpcc_rust_concurrent_resource_exhausted": "run",
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def exactly_one(items, label):
    require(len(items) == 1, f"expected exactly one {label}, found {len(items)}")
    return items[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-dir", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--repo", type=Path, help="repository containing the pinned source commit")
    args = parser.parse_args()
    root = args.evidence_dir.resolve()
    repo = args.repo
    if repo is None:
        repo = next((path for path in [root, *root.parents] if (path / ".git").exists()), None)
    require(repo is not None, "cannot find repository; pass --repo /path/to/repository")

    def source(path):
        return subprocess.check_output(["git", "-C", str(repo), "show", f"{REVISION}:{path}"])

    def read(name):
        return (root / name).read_text()

    def data(name):
        return json.loads(read(name))

    manifest_bytes = (root / "selected-artifact-hashes.json").read_bytes()
    require(digest(manifest_bytes) == MANIFEST_SHA, "selected-artifact manifest hash mismatch")
    manifest = json.loads(manifest_bytes)
    require(manifest["source_revision"] == REVISION, "unexpected selected source revision")
    require(len(manifest["files"]) == 17, "selected raw artifact inventory changed")
    for name, expected in manifest["files"].items():
        require(Path(name).name == name, f"nonlocal selected artifact path: {name}")
        contents = (root / name).read_bytes()
        require(digest(contents) == expected["sha256"], f"artifact hash mismatch: {name}")
        require(len(contents) == expected["size_bytes"], f"artifact size mismatch: {name}")
    for path, expected in manifest["source_reference_hashes"].items():
        require(digest(source(path)) == expected, f"committed native/CI source mismatch: {path}")

    remote = data("artifact-hashes.json")
    overlap = set(remote) & set(manifest["files"])
    require(len(overlap) == 14, "unexpected overlap with original remote manifest")
    for name in overlap:
        require(remote[name] == manifest["files"][name], f"remote manifest mismatch: {name}")
    provenance = data("provenance.json")
    require(provenance["source_head_at_start"] == provenance["source_head_at_finish"] == REVISION,
            "relink source revision changed")
    require(provenance["mode"] == "reuse-identical-Rust-PGO-archive-rebuild-native-cold-logging",
            "unexpected relink mode")
    require(provenance["binary_sha256"] == remote["sto_tpcc_bench"]["sha256"] == BINARY_SHA,
            "reported final binary identity mismatch")
    require(provenance["rust_pgo_archive_sha256"] == ARCHIVE_SHA, "reported Rust archive identity mismatch")
    require(provenance["rust_profile_sha256"] == PROFILE_SHA, "reported Rust profile identity mismatch")
    require(remote["source.patch"]["sha256"] == digest(b"") and remote["source.patch"]["size_bytes"] == 0,
            "relink reports a nonempty tracked source patch")

    captured = data("rust-source-sha256-captured.json")
    current = data("rust-source-sha256-current.json")
    require(captured == current and len(current) == provenance["rust_subtree_files_verified"] == 123,
            "trained/current Rust subtree inventories differ")
    committed_paths = subprocess.check_output(
        ["git", "-C", str(repo), "ls-tree", "-r", "--name-only", REVISION, "--", "crates"], text=True)
    require(set(committed_paths.splitlines()) == set(current), "committed Rust subtree inventory differs")
    for path, expected in current.items():
        require(digest(source(path)) == expected, f"committed Rust source mismatch: {path}")

    before = data("native-link-inputs-sha256-before.json")
    after = data("native-link-inputs-sha256-after.json")
    require(before == after and len(before) == 27, "native inputs changed during relink/smokes")
    changes = data("native-link-input-changes.json")
    require(changes == provenance["native_input_changes"] and len(changes) == 5,
            "native change inventory mismatch")
    build = provenance["native_build"]
    expected_changes = {f"{build}/{suffix}" for suffix in [
        "CMakeFiles/rules.ninja", "build.ninja", "libmako.a",
        "CMakeFiles/sto_tpcc_bench.dir/src/mako/benchmarks/dbtest.cc.o",
        "CMakeFiles/sto_tpcc_bench.dir/src/mako/storage/rust_sto_tpcc_wrapper.cc.o",
    ]}
    require(set(changes) == expected_changes, "unexpected changed native input")
    for path, change in changes.items():
        require(change["new"] == before[path] and change["old"] != change["new"],
                f"inconsistent native input change: {path}")
    comparison = data("final-native-compile-command-comparison.json")
    require(comparison["old_compile_commands"] == comparison["new_compile_commands"] == 122,
            "native compile command count changed")
    require(comparison["old_file_sha256"] == comparison["new_file_sha256"], "native command hash changed")
    require(not any(comparison[key] for key in ["added_commands", "removed_commands", "changed_commands"]),
            "native compile commands changed")

    commands = data("commands.json")
    require(len(commands) == 19 and all(item["returncode"] == 0 for item in commands),
            "failed or missing relink command")
    by_log = {item["log"]: item for item in commands}
    require(len(by_log) == len(commands), "duplicate relink command log")
    for name in ["native-build.log", "prepare-relink.log", "relink.log", "ctest-boundaries.log"]:
        require(name in by_log, f"missing successful command: {name}")
    binary = provenance["binary"]
    require(binary in by_log["prepare-relink.log"]["command"], "relink did not target reported binary")
    require(provenance["rust_pgo_archive"] in by_log["prepare-relink.log"]["command"],
            "relink did not select trained Rust archive")

    workflow = source(".github/workflows/ci.yml").decode()
    selector = exactly_one(re.findall(r"-R '(\^\(SiloVarintTests[^']+)'", workflow), "committed CI selector")
    names = selector[2:-2].split("|")
    require(len(names) == len(set(names)) == provenance["ctest_boundary_count"] == 31,
            "committed CI boundary count mismatch")
    tests = data("ctest-descriptor.json")["tests"]
    require(len(tests) == 31 and {test["name"] for test in tests} == set(names),
            "retained CTest inventory differs from committed CI")
    require(selector in by_log["ctest-boundaries.log"]["command"], "boundary run used a different selector")
    passed = re.findall(r"^\s*\d+/31 Test\s+#\d+: (\S+)\s+\.+\s+Passed\s", read("ctest-boundaries.log"), re.M)
    require(len(passed) == 31 and set(passed) == set(names), "not all 31 boundaries passed exactly once")
    require("100% tests passed, 0 tests failed out of 31" in read("ctest-boundaries.log"), "missing CTest pass summary")
    require(set(provenance["relinked_capacity_smokes"]) == set(SMOKES), "capacity smoke inventory changed")
    by_name = {test["name"]: test for test in tests}
    for name, phase in SMOKES.items():
        record = by_log[f"{name}-relinked.log"]
        descriptor = by_name[name]
        expected_command = [binary if token == f"{build}/sto_tpcc_bench" else token for token in descriptor["command"]]
        require(record["command"] == expected_command and binary in expected_command, f"wrong relink smoke command: {name}")
        props = {prop["name"]: prop["value"] for prop in descriptor["properties"]}
        expected_env = dict(item.split("=", 1) for item in props.get("ENVIRONMENT", []))
        require(record["extra_environment"] == expected_env, f"wrong smoke limits: {name}")
        text = read(f"{name}-relinked.log")
        markers = re.findall(r"^TPCC_RESOURCE_EXHAUSTED phase=(\w+) error=.+$", text, re.M)
        require(markers == [phase], f"missing or fragmented exhaustion diagnostic: {name}")
        require(f"validated TPC-C argument rejection: status=3 text='TPCC_RESOURCE_EXHAUSTED phase={phase}'" in text,
                f"missing validated exit 3: {name}")
        require("TPCC_BENCH_RESULT" not in text, f"successful throughput on exhausted run: {name}")

    growth = read("growth-180s.log")
    result = json.loads(exactly_one(re.findall(r"^TPCC_BENCH_RESULT (\{.+\})$", growth, re.M), "growth result"))
    expected = {"engine": "rust", "threads": 1, "warehouses": 1, "configured_seconds": 180,
                "commits": 10187161, "aborts": 0, "attempts": 10187161}
    require(all(result[key] == value for key, value in expected.items()), "growth result mismatch")
    require(sum(result["mix"].values()) == result["commits"], "growth mix does not sum to commits")
    require(result["measured_seconds"] >= 180, "growth run shorter than requested")
    require("TPCC_RESOURCE_EXHAUSTED" not in growth, "growth exhausted resources")
    require(re.findall(r"^\s*Exit status: (\d+)$", growth, re.M) == ["0"], "growth exit status mismatch")
    require(f'taskset -c 10 {binary} --num-threads 1' in growth and '--runtime 180 --storage-engine rust --slow-exit' in growth,
            "growth did not run the final binary with expected options")
    require("STO_TPCC_NATIVE_ALLOCATOR configured_bytes=4294967296 scope=native_only rust_heap_excluded=1" in growth,
            "growth native allocator setting mismatch")

    def usage(selector):
        line = exactly_one([line for line in growth.splitlines() if line.startswith(selector + " ")], selector)
        return dict(item.split("=", 1) for item in line.split()[1:])

    db = usage("STO_TPCC_CAPACITY phase=run_complete scope=database")
    require(int(db["allocated_registry_bytes"]) == 4557103264, "growth registry allocation mismatch")
    require(int(db["max_registry_bytes"]) == 8 * 1024**3, "growth registry budget mismatch")
    require(0 <= int(db["allocated_registry_bytes"]) <= int(db["max_registry_bytes"]), "growth registry exceeds budget")
    require(int(db["allocated_registry_bytes"]) + int(db["registry_headroom_bytes"]) == int(db["max_registry_bytes"]),
            "growth registry headroom mismatch")
    order_line = usage("STO_TPCC_CAPACITY phase=run_complete table=order_line_0")
    require(int(order_line["retained_records"]) == int(order_line["consumed_record_ids"]) == 46141818,
            "growth order-line record/ID mismatch")
    require(int(order_line["retained_key_bytes"]) == 738269088, "growth order-line key bytes mismatch")
    for key in ["max_retained_records", "max_consumed_record_ids", "max_retained_key_bytes"]:
        require(int(order_line[key]) == 2**64 - 1, f"growth quota override: {key}")

    print("PASS: 17 retained hashes; a3ad9a1 source; 123 identical Rust files; reported archive/binary identities; "
          "stable native relink inputs; 31 boundaries; 4 exit-3 smokes; 180-second growth within 8 GiB")
    print("Scope: uncopied remote binaries, archive bytes, and other remote artifacts were not locally hashed.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
