#!/usr/bin/env python3
"""Read-only verification of the packaged final capacity performance sweep."""

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
from pathlib import Path, PurePosixPath
import re
import statistics
import sys

sys.dont_write_bytecode = True

BINARY_HASHES = {
    "old": "b2d484ffb6c80fe31cd03e09119e3885300516c1587218984ec5b9aedba8f716",
    "new": "3e32ba7d47a6de1b080d14cdc4955d04ecee0ece32a969a2dcfdf3925a2e31fe",
}
CONTROLLER_HASH = "402af5c22414e52b67f1f1c2710a2f93375b82cdc29c9a4f9be92fb9ff265055"
RUNNER_HASH = "4aca8d7e118bc565286b0b5f87cd0cca4bd24fa2314c85761fa825e8c0ba5e51"
CONFIG_HASH = "3324428f7b8767c74d93041f500126e6b5721f0d7eb05fbc6b907b6a9d9c9a24"
COLLECTOR_HASH = "0711025fc67d4d283a4cce8d28f8ffa3c2668287b50a07e7f0b511224a143b54"
POSTFLIGHT_HASH = "675f0de6531882dcc21d1fa68fde0a0c23b1c5dab371346556c8c124c8cb49d4"
RUST_ARCHIVE_HASH = "7d29c5077d9a62b9bdce4c4b1a7f1c7eecc206fc7cc92138e8b921321a579b87"
RUST_PROFILE_HASH = "c14043bbd29a8f74459bb2ed1bfcb98917313ee508fd4c2a7f2a9412dadea62b"
ENVIRONMENT = {"MAKO_TPCC_ALLOCATOR_MEMORY": "4G", "MAKO_STO_TPCC_REGISTRY_MEMORY": "8G",
               "MAKO_TPCC_WORKLOAD_MIX": "45,43,4,4,4"}
PLAN = [
    (0, 1, 16, "new", ["cpp", "rust"]), (0, 1, 16, "old", ["rust", "cpp"]),
    (0, 2, 1, "old", ["cpp", "rust"]), (0, 2, 1, "new", ["rust", "cpp"]),
    (1, 0, 1, "new", ["cpp", "rust"]), (1, 0, 1, "old", ["rust", "cpp"]),
    (1, 2, 16, "old", ["cpp", "rust"]), (1, 2, 16, "new", ["rust", "cpp"]),
    (2, 0, 1, "old", ["cpp", "rust"]), (2, 0, 1, "new", ["rust", "cpp"]),
    (2, 1, 16, "new", ["cpp", "rust"]), (2, 1, 16, "old", ["rust", "cpp"]),
    (0, 0, 4, "new", ["rust", "cpp"]), (0, 3, 8, "new", ["cpp", "rust"]),
    (0, 4, 2, "new", ["rust", "cpp"]),
]
SCHEDULE = [{"repetition": r, "threads": t, "variant": v, "engines": engines,
             "pair_id": f"r{r:02d}-c{c:02d}-{t}t-{v}"} for r, c, t, v, engines in PLAN]
EXPECTED_ROWS = [(cell["threads"], cell["repetition"], cell["variant"], engine, cell["pair_id"])
                 for cell in SCHEDULE for engine in cell["engines"]]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def equal(actual, expected, label):
    if isinstance(expected, dict):
        require(isinstance(actual, dict) and actual.keys() == expected.keys(), f"{label}: fields differ")
        for key in expected:
            equal(actual[key], expected[key], f"{label}.{key}")
    elif isinstance(expected, list):
        require(isinstance(actual, list) and len(actual) == len(expected), f"{label}: length differs")
        for index, (left, right) in enumerate(zip(actual, expected)):
            equal(left, right, f"{label}[{index}]")
    elif isinstance(expected, float):
        require(finite(actual) and math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-9),
                f"{label}: {actual!r} != {expected!r}")
    else:
        require(type(actual) is type(expected) and actual == expected, f"{label}: {actual!r} != {expected!r}")


def utc(text):
    result = datetime.fromisoformat(text)
    require(result.tzinfo is not None, "timestamp lacks an explicit timezone")
    return result.astimezone(timezone.utc)


def single(items, label):
    require(len(items) == 1, f"expected one {label}, found {len(items)}")
    return items[0]


class Bundle:
    def __init__(self, root, partial):
        self.root = root.resolve()
        self.partial = partial
        self.manifest = None

    def path(self, name):
        require(isinstance(name, str) and bool(name), "empty/non-string artifact path")
        relative = PurePosixPath(name)
        require(not relative.is_absolute() and ".." not in relative.parts and "\\" not in name,
                f"unsafe artifact path: {name}")
        require(relative.as_posix() == name and name != ".", f"noncanonical artifact path: {name}")
        result = (self.root / name).resolve(strict=True)
        require(result.is_relative_to(self.root) and result.is_file(), f"artifact leaves bundle or is not a file: {name}")
        if self.manifest is not None:
            require(name in self.manifest or name == "artifact-hashes.json", f"unmanifested input: {name}")
        return result

    def text(self, name):
        return self.path(name).read_text(encoding="utf-8")

    def json(self, name):
        def reject_constant(token):
            raise ValueError(f"nonfinite JSON number in {name}: {token}")
        return json.loads(self.text(name), parse_constant=reject_constant)

    def jsonl(self, name):
        return [json.loads(line) for line in self.text(name).splitlines() if line.strip()]

    def check_manifest(self):
        manifest_path = self.root / "artifact-hashes.json"
        if self.partial and not manifest_path.exists():
            return
        self.manifest = self.json("artifact-hashes.json")
        require(isinstance(self.manifest, dict) and self.manifest, "empty artifact manifest")
        for name, expected in self.manifest.items():
            path = self.path(name)
            require(re.fullmatch(r"[0-9a-f]{64}", expected["sha256"]), f"invalid digest: {name}")
            require(sha(path) == expected["sha256"], f"artifact hash mismatch: {name}")
            require(path.stat().st_size == expected["size_bytes"], f"artifact size mismatch: {name}")

    def csv(self, name):
        return list(csv.DictReader(self.text(name).splitlines()))


def check_csv(bundle, name, expected, six_places=False):
    actual = bundle.csv(name)
    require(len(actual) == len(expected), f"{name}: row count differs")
    for index, (row, wanted) in enumerate(zip(actual, expected)):
        require(row.keys() == wanted.keys(), f"{name}: fields differ")
        for key, value in wanted.items():
            text = row[key]
            label = f"{name}[{index}].{key}"
            if type(value) is float and not six_places:
                equal(float(text), value, label)
            else:
                formatted = "" if value is None else f"{value:.6f}" if type(value) is float and six_places else str(value)
                require(text == formatted, f"{label}: {text!r} != {formatted!r}")


def patch_summary(rows):
    cells = {}
    for row in rows:
        cells.setdefault((row["threads"], row["repetition"]), {})[row["variant"], row["engine"]] = row["throughput_txn_s"]
    output = []
    for threads in [1, 2, 4, 8, 16]:
        complete = [values for (t, _), values in cells.items() if t == threads and len(values) == 4]
        if not complete:
            continue
        result = {"threads": threads, "complete_blocks": len(complete)}
        for label, numerator, denominator in [
            ("rust_new_over_old", ("new", "rust"), ("old", "rust")),
            ("cpp_new_over_old", ("new", "cpp"), ("old", "cpp")),
            ("new_rust_over_cpp", ("new", "rust"), ("new", "cpp")),
            ("old_rust_over_cpp", ("old", "rust"), ("old", "cpp")),
        ]:
            ratios = [values[numerator] / values[denominator] for values in complete]
            result[label] = {"ratios": ratios, "median_percent": statistics.median(ratios) * 100,
                             "min_percent": min(ratios) * 100, "max_percent": max(ratios) * 100}
            if len(ratios) == 3:
                mean, half = log_interval(ratios)
                result[label].update({"approx_log_t_95_percent_interval": [100 * math.exp(mean - half), 100 * math.exp(mean + half)],
                                      "geometric_mean_percent": 100 * math.exp(mean)})
        result["normalized_rust_new_over_old_percent"] = [
            100 * (values["new", "rust"] / values["old", "rust"]) /
            (values["new", "cpp"] / values["old", "cpp"]) for values in complete]
        result["median_throughput"] = {variant + "_" + engine: statistics.median(values[variant, engine] for values in complete)
                                       for variant in ["old", "new"] for engine in ["cpp", "rust"]}
        output.append(result)
    return output


def log_interval(values):
    logs = [math.log(value) for value in values]
    return statistics.mean(logs), 4.302652729911275 * statistics.stdev(logs) / math.sqrt(3)


def paired_summary(rows):
    by_key = {(r["threads"], r["repetition"], r["variant"], r["engine"]): r for r in rows}
    controls, summaries = [], []
    for threads in [1, 16]:
        ratios = {"rust": [], "cpp": [], "normalized_rust": []}
        for repetition in range(3):
            old_rust, new_rust, old_cpp, new_cpp = [by_key[threads, repetition, variant, engine]
                                                   for variant, engine in [("old", "rust"), ("new", "rust"), ("old", "cpp"), ("new", "cpp")]]
            rust = new_rust["throughput_txn_s"] / old_rust["throughput_txn_s"]
            cpp = new_cpp["throughput_txn_s"] / old_cpp["throughput_txn_s"]
            for label, value in [("rust", rust), ("cpp", cpp), ("normalized_rust", rust / cpp)]:
                ratios[label].append(value)
            controls.append({"threads": threads, "repetition": repetition,
                "old_rust_txn_s": old_rust["throughput_txn_s"], "new_rust_txn_s": new_rust["throughput_txn_s"],
                "rust_change_percent": (rust - 1) * 100, "old_cpp_txn_s": old_cpp["throughput_txn_s"],
                "new_cpp_txn_s": new_cpp["throughput_txn_s"], "cpp_change_percent": (cpp - 1) * 100,
                "normalized_rust_change_percent": (rust / cpp - 1) * 100,
                "old_rust_started_at_utc": old_rust["started_at_utc"], "new_rust_started_at_utc": new_rust["started_at_utc"]})
        summary = {"threads": threads, "paired_blocks": 3}
        for label, values in ratios.items():
            mean, half = log_interval(values)
            summary[label] = {"paired_changes_percent": [(value - 1) * 100 for value in values],
                "median_change_percent": (statistics.median(values) - 1) * 100,
                "geometric_mean_change_percent": (math.exp(mean) - 1) * 100,
                "approx_log_t_95_change_percent_interval": [(math.exp(mean - half) - 1) * 100, (math.exp(mean + half) - 1) * 100]}
        summaries.append(summary)
    return controls, summaries


def check_quiet(window, threads):
    require(window["violations"] == [] and finite(window["observed_seconds"]) and window["observed_seconds"] >= 2,
            "accepted quiet window is invalid or too short")
    require(window["competing_before"] == window["competing_after"] == [], "competitor in accepted quiet window")
    cpus = set(range(10, 10 + threads)) | set(range(74, 74 + threads))
    require(set(map(int, window["cpu_idle_percent"])) == cpus, "quiet window omits physical/SMT CPUs")
    require(all(finite(value) and 95 <= value <= 100 for value in window["cpu_idle_percent"].values()), "CPU idle threshold violated")
    alignment = window["lxd_restart_alignment"]
    restart = alignment["restart_count_after"]
    require(type(restart) is int and restart > alignment["restart_count_before"], "alignment did not observe a new restart")
    require(window["restart_count_before"] == window["restart_count_after"] == restart, "quiet window crossed restart")
    require(window["waiting_policy"] == "same-restart-quiet-v2" and window["launch_deadline_seconds"] == 20,
            "quiet-window policy changed")
    require(window["initial_settling_seconds"] == 3 and window["within_restart_attempt"] >= 1,
            "quiet-window initial wait changed")
    require(window["settling_seconds_before_window"] == (3 if window["within_restart_attempt"] == 1 else 0),
            "quiet-window retry wait mismatch")
    matches = alignment["journal_visibility"]["matching_restart_records"]
    require(matches and alignment["journal_visibility"]["activity_count"] >= len(matches), "restart has no journal confirmation")
    require(all(re.search(rf"restart counter is at {restart}\b", record["message"]) for record in matches),
            "journal confirmation names another restart")


def check_postflight(bundle, metadata, runners, controllers, completed):
    require(sha(bundle.path("identity-postflight.py")) == POSTFLIGHT_HASH, "postflight program identity changed")
    postflight = bundle.json("identity-postflight.json")
    require(postflight["status"] == "passed" and postflight["active_benchmark_processes_at_start"] == [],
            "postflight did not pass with all benchmark processes stopped")
    require(postflight["launch_metadata_count"] == len(metadata), "postflight omitted launch metadata")
    require(utc(completed) <= utc(postflight["started_at_utc"]) <= utc(postflight["finished_at_utc"]),
            "postflight was not recorded after completion")
    relink = bundle.json("new-relink-provenance.json")
    require(relink["binary_sha256"] == BINARY_HASHES["new"] and relink["rust_pgo_archive_sha256"] == RUST_ARCHIVE_HASH
            and relink["rust_profile_sha256"] == RUST_PROFILE_HASH, "relink identity differs from postflight pins")
    expected = {}
    for index, meta in enumerate(metadata):
        for variant in ["old", "new"]:
            expected[f"launch{index}-{variant}-binary"] = (meta["binaries"][variant], BINARY_HASHES[variant])
        expected[f"launch{index}-configuration"] = (meta["config"], CONFIG_HASH)
        # Construct the recorded path as text; never resolve or read it.
        live_runner = str(PurePosixPath(meta["config"]).parent.parent / "scripts/run_sto_tpcc_compare.py")
        expected[f"launch{index}-live-runner"] = (live_runner, RUNNER_HASH)
    expected["unchanged-Rust-PGO-archive"] = (relink["rust_pgo_archive"], RUST_ARCHIVE_HASH)
    expected["unchanged-Rust-merged-profile"] = (relink["rust_profile"], RUST_PROFILE_HASH)
    checks = postflight["checks"]
    for label, (path, fingerprint) in expected.items():
        check = single([check for check in checks if check["label"] == label], label)
        require(check["path"] == path and check["expected_sha256"] == check["actual_sha256"] == fingerprint
                and check["matches"] is True, f"failed/mismatched postflight check: {label}")
    expected_labels = set(expected)
    expected_count = len(expected)
    for index in range(len(metadata)):
        for suffix, paths, fingerprint in [("archived-runner", runners, RUNNER_HASH), ("archived-controller", controllers, CONTROLLER_HASH)]:
            label = f"launch{index}-{suffix}"
            matching = [check for check in checks if check["label"] == label]
            require(len(matching) == len(paths) and {PurePosixPath(check["path"]).name for check in matching} == {path.name for path in paths},
                    f"postflight archived inventory mismatch: {label}")
            require(all(check["matches"] is True and check["expected_sha256"] == check["actual_sha256"] == fingerprint for check in matching),
                    f"postflight archived digest mismatch: {label}")
            expected_labels.add(label)
            expected_count += len(paths)
    require(len(checks) == expected_count and {check["label"] for check in checks} == expected_labels,
            "postflight check inventory has missing/extra entries")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--partial-fixture", action="store_true", help="test an incomplete fixed-plan prefix; never verifies a final result")
    options = parser.parse_args()
    bundle = Bundle(options.directory, options.partial_fixture)
    rows = bundle.jsonl("raw.jsonl")
    if options.partial_fixture:
        require(0 < len(rows) < 30 and len(rows) % 2 == 0, "partial fixture must contain complete pairs but fewer than 30 samples")
    else:
        require(len(rows) == 30, f"final sweep requires exactly 30 samples; found {len(rows)}")
    bundle.check_manifest()
    # Re-read through the manifest membership check in final mode.
    equal(bundle.jsonl("raw.jsonl"), rows, "raw.jsonl")
    require([(r["threads"], r["repetition"], r["variant"], r["engine"], r["pair_id"]) for r in rows] == EXPECTED_ROWS[:len(rows)],
            "sample keys/order do not match the frozen 30-sample plan")
    require([r["run_order"] for r in rows] == list(range(1, len(rows) + 1)), "run order is not consecutive")
    metadata_paths = sorted(bundle.root.glob("run-*.json"))
    require(metadata_paths, "missing run metadata")
    metadata = [bundle.json(path.name) for path in metadata_paths]
    runner_paths = sorted(bundle.root.glob("runner-*.py"))
    controllers = sorted(bundle.root.glob("orchestrator-*.py"))
    require(runner_paths and controllers, "missing archived runner/controller")
    for path in runner_paths:
        require(sha(bundle.path(path.name)) == RUNNER_HASH, "archived runner identity changed")
    for path in controllers:
        require(sha(bundle.path(path.name)) == CONTROLLER_HASH, "archived controller identity changed")
    # This known, hash-pinned module has no execution on import. Only its pure
    # result parser and summary function are called; bytecode writes are off.
    spec = importlib.util.spec_from_file_location("capacity_archived_runner", runner_paths[0])
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    for meta in metadata:
        expected = {"runtime_seconds": 10, "allocator_memory": "4G", "rust_registry_memory": "8G",
                    "workload_mix": [45, 43, 4, 4, 4], "physical_cpus": list(range(10, 26)), "alignment": "engine",
                    "waiting_policy": "same-restart-quiet-v2", "expected_samples": 30, "priority_boundaries": True,
                    "intermediate_repetitions": 1, "schedule_seed": 0x5EED, "host": "zoo-002"}
        for key, value in expected.items():
            equal(meta[key], value, f"metadata.{key}")
        require(meta["settling_seconds_before_quiet_window"] == 3, "initial settling policy changed")
        equal(meta["schedule"], SCHEDULE, "metadata.schedule")
        require(meta["config_fingerprint"]["sha256"] == CONFIG_HASH, "configuration identity changed")
        require(meta["runner_fingerprint"]["sha256"] == RUNNER_HASH and meta["orchestrator_fingerprint"]["sha256"] == CONTROLLER_HASH,
                "recorded controller/runner identity changed")
        require({key: value["sha256"] for key, value in meta["binary_fingerprints"].items()} == BINARY_HASHES,
                "recorded binary identity changed")
        require(meta["fallbacks"] == {key: "unset" for key in runner.TPCC_DIAGNOSTIC_FALLBACK_ENVIRONMENT_KEYS}, "diagnostic fallback enabled")
        require(meta["quotas"] == {key: "unset" for key in runner.TPCC_RECORD_QUOTA_ENVIRONMENT_KEYS}, "numeric quota override enabled")
        require(meta["binaries"] == metadata[0]["binaries"] and meta["config"] == metadata[0]["config"], "resumed paths changed")
    if not options.partial_fixture:
        require(sha(bundle.path("benchmark-config.yml")) == CONFIG_HASH, "copied benchmark config mismatch")
        collectors = [name for name in ["collect-remote-evidence.py", "validate-and-package.py"] if (bundle.root / name).is_file()]
        require(sha(bundle.path(single(collectors, "remote collector snapshot"))) == COLLECTOR_HASH, "collector snapshot identity changed")
    quiet_rows = bundle.jsonl("quiet-windows.jsonl")
    quiet_lookup = {(r["threads"], json.dumps(r["sample"], sort_keys=True)): r for r in quiet_rows}
    previous_start = None
    for index, row in enumerate(rows):
        label = f"run {index + 1}"
        threads, variant, engine = row["threads"], row["variant"], row["engine"]
        require(row["binary_sha256"] == BINARY_HASHES[variant] and row["controller_sha256"] == CONTROLLER_HASH, f"{label}: identity mismatch")
        require(row["configured_seconds"] == 10 and row["warehouses"] == threads and row["host"] == "zoo-002", f"{label}: workload size changed")
        mask = ",".join(str(cpu) for cpu in range(10, 10 + threads))
        require(row["cpu_affinity"] == mask and row["environment_overrides"] == ENVIRONMENT, f"{label}: CPU/memory/mix changed")
        expected_command = ["/usr/bin/taskset", "-c", mask, metadata[-1]["binaries"][variant], "--num-threads", str(threads),
                            "--shard-config", metadata[-1]["config"], "--site-name", "local_s0", "--runtime", "10", "--storage-engine", engine]
        require(row["command"] == expected_command, f"{label}: command changed")
        require(row["alignment"] == "engine" and row["waiting_policy"] == "same-restart-quiet-v2" and row["settling_seconds_before_quiet_window"] == 3,
                f"{label}: guard policy changed")
        stdout, stderr = bundle.text(row["stdout_log"]), bundle.text(row["stderr_log"])
        require("TPCC_RESOURCE_EXHAUSTED" not in stdout + stderr and "TPCC_BENCH_RESULT " not in stderr,
                f"{label}: failure marker or misplaced result")
        if variant == "new" and engine == "rust":
            require("STO_TPCC_NATIVE_ALLOCATOR configured_bytes=4294967296 scope=native_only rust_heap_excluded=1" in stderr,
                    f"{label}: raw native allocator setting differs")
            for phase in ["load_complete", "run_complete"]:
                capacity = single(re.findall(rf"^STO_TPCC_CAPACITY phase={phase} scope=database (.+)$", stderr, re.M), f"{label} {phase} budget")
                values = dict(field.split("=", 1) for field in capacity.split())
                require(int(values["max_registry_bytes"]) == 8 * 1024**3
                        and 0 <= int(values["allocated_registry_bytes"]) <= int(values["max_registry_bytes"])
                        and int(values["allocated_registry_bytes"]) + int(values["registry_headroom_bytes"]) == int(values["max_registry_bytes"]),
                        f"{label}: raw registry budget/headroom differs")
        parsed = runner.extract_result(stdout, engine)
        for key, value in parsed.items():
            equal(row[key], value, f"{label}.raw_result.{key}")
        require(finite(parsed["throughput_txn_s"]) and parsed["throughput_txn_s"] > 0 and finite(parsed["measured_seconds"]),
                f"{label}: invalid throughput/time")
        equal(parsed["throughput_txn_s"], parsed["commits"] / parsed["measured_seconds"], f"{label}.computed_throughput")
        guard = row["guard"]
        require(guard["competing_after_pair"] == [] and guard["accepted_pair_attempt"] == row["pair_attempt"], f"{label}: pair guard failed")
        require(1 <= row["pair_attempt"] <= 20, f"{label}: invalid pair attempt")
        require([r["pair_attempt"] for r in guard["rejected_attempts"]] == list(range(1, row["pair_attempt"])), f"{label}: missing rejected attempts")
        require(all(attempt["reasons"] for attempt in guard["rejected_attempts"]), f"{label}: unreasoned rejection")
        expected_engines = SCHEDULE[index // 2]["engines"]
        require([window["engine"] for window in guard["engine_windows"]] == expected_engines, f"{label}: engine window order mismatch")
        for window in guard["engine_windows"]:
            require(window["competing_after_run"] == [] and window["measurement_lxd_guard"]["journal_activity"] == [], f"{label}: contaminated measurement")
            quiet = window["pre_run_window"]
            check_quiet(quiet, threads)
            require((threads, json.dumps(quiet, sort_keys=True)) in quiet_lookup, f"{label}: accepted quiet window missing from raw log")
            require(window["restart_count_before_run"] == quiet["restart_count_after"] and type(window["restart_count_after_run"]) is int
                    and window["restart_count_after_run"] >= window["restart_count_before_run"], f"{label}: invalid restart counters")
        own_window = single([window for window in guard["engine_windows"] if window["engine"] == engine], f"{label} engine guard")
        equal(row["measurement_lxd_guard"], own_window["measurement_lxd_guard"], f"{label}.measurement_guard")
        equal(guard["quiet_window"], guard["engine_windows"][-1]["pre_run_window"], f"{label}.last_quiet_window")
        require(type(guard["restart_count_after_pair"]) is int and guard["restart_count_after_pair"] >= guard["engine_windows"][-1]["restart_count_after_run"],
                f"{label}: invalid final restart counter")
        start = utc(row["started_at_utc"])
        require(previous_start is None or start > previous_start, "accepted process times are not ordered")
        previous_start = start
        elapsed = (start - utc(own_window["pre_run_window"]["lxd_restart_alignment"]["observed_at_utc"])).total_seconds()
        equal(row["launch_seconds_after_restart"], elapsed, f"{label}.restart_launch")
        require(0 <= elapsed <= 20, f"{label}: launch deadline violated")
        measured = row["measurement_lxd_guard"]
        require(measured["journal_activity"] == [], f"{label}: LXD activity during measurement")
        begin, finish = utc(measured["started_at_utc"]), utc(measured["finished_at_utc"])
        # The recorded zoo-002 logger uses UTC. Do not inherit the reviewer's
        # timezone as the original live parser does.
        for regex, instant in [(runner.BENCHMARK_MEASUREMENT_START_RE, begin), (runner.BENCHMARK_MEASUREMENT_END_RE, finish)]:
            timestamp = single(regex.findall(stderr), f"{label} timestamped measurement marker")
            wall, micros = timestamp.rsplit("-", 1)
            parsed_stamp = datetime.strptime(wall, "%Y%m%d-%H:%M.%S").replace(microsecond=int(micros), tzinfo=timezone.utc)
            require(parsed_stamp == instant, f"{label}: measurement marker differs from guard")
        equal(row["setup_seconds"], (begin - start).total_seconds(), f"{label}.setup_seconds")
        equal(row["measurement_guard_seconds"], (finish - begin).total_seconds(), f"{label}.measurement_guard_seconds")
        equal(row["post_measurement_seconds"], row["wall_seconds_including_load"] - (finish - start).total_seconds(), f"{label}.post_measurement_seconds")
        require(0 <= row["setup_seconds"] and 0 <= row["post_measurement_seconds"] and 10 <= parsed["measured_seconds"] <= row["measurement_guard_seconds"],
                f"{label}: inconsistent measurement interval")
        if index % 2:
            equal(guard, rows[index - 1]["guard"], f"{label}.paired_guard")
        prefix = row.get("guard_logs_directory", "")
        def rejected_references(value):
            if isinstance(value, dict):
                for key, child in value.items():
                    if key in {"stdout_log", "stderr_log", "metadata_log"}:
                        bundle.path(f"{prefix}/{child}" if prefix else child)
                    else:
                        rejected_references(child)
            elif isinstance(value, list):
                for child in value:
                    rejected_references(child)
        rejected_references(guard["rejected_attempts"])
    for variant in ["old", "new"]:
        subset = [row for row in rows if row["variant"] == variant]
        if subset:
            check_csv(bundle, f"{variant}-summary.csv", runner.summarize(subset), six_places=True)
    equal(bundle.json("patch-cost.json"), patch_summary(rows), "patch-cost.json")
    if options.partial_fixture:
        print(f"FIXTURE CHECK ONLY: {len(rows)}/30 fixed-plan samples, guards, raw logs and provisional summaries agree.")
        print("INCOMPLETE: no final acceptance, final package manifest, paired summary or completion claim was verified.")
        return
    complete = bundle.json("complete.json")
    require(complete["accepted_samples"] == 30 and utc(complete["completed_at_utc"]) > utc(rows[-1]["started_at_utc"]), "invalid completion record")
    check_postflight(bundle, metadata, runner_paths, controllers, complete["completed_at_utc"])
    controls, summaries = paired_summary(rows)
    check_csv(bundle, "old-new-paired-controls.csv", controls)
    equal(bundle.json("old-new-paired-summary.json")["cells"], summaries, "paired summary")
    timing_keys = ["threads", "variant", "engine", "repetition", "run_order", "started_at_utc", "throughput_txn_s",
                   "setup_seconds", "measurement_guard_seconds", "post_measurement_seconds", "wall_seconds_including_load"]
    check_csv(bundle, "process-timings.csv", [{**{key: row[key] for key in timing_keys}, "waiting_policy": row["waiting_policy"]} for row in rows])
    validation = bundle.json("evidence-validation.json")
    unique_pairs = {row["pair_id"]: row for row in rows}
    expected_validation = {"accepted_samples": 30, "paired_boundary_blocks": 6, "candidate_only_intermediate_pairs": 3,
        "quiet_window_attempts": len(quiet_rows), "rejected_quiet_windows": sum(bool(row["sample"]["violations"]) for row in quiet_rows),
        "rejected_measurement_pair_attempts": sum(len(row["guard"]["rejected_attempts"]) for row in unique_pairs.values()),
        "waiting_policy_sample_counts": {"fixed-settle-v1": 0, "same-restart-quiet-v2": 30},
        "binary_sha256": BINARY_HASHES, "native_allocator_memory": "4G", "rust_registry_memory": "8G"}
    for key, value in expected_validation.items():
        equal(validation[key], value, f"evidence-validation.{key}")
    print(f"PASS: {len(bundle.manifest)} retained hashes; exact frozen 30-sample plan; identities/settings; quiet and measurement guards;")
    print("raw results and timing markers; C++/Rust summaries; six paired controls and all paired-ratio arithmetic; postflight identities.")
    print("Scope: retained evidence only. No measurements were rerun or remote binaries hashed; three-block intervals do not establish equivalence.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError, IndexError, RuntimeError, ZeroDivisionError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
