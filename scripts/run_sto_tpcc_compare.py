#!/usr/bin/env python3
"""Run matched C++ and Rust STO/Masstree TPC-C trials."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import random
import shlex
import socket
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path


RESULT_PREFIX = "TPCC_BENCH_RESULT "
ENGINES = ("cpp", "rust")
MIX_KEYS = ("NewOrder", "Payment", "Delivery", "OrderStatus", "StockLevel")
# tpcc.cc opens 11 separate per-warehouse trees and one shared item tree.
# The native C++ wrapper reserves 200 table IDs per shard, so 18 warehouses
# (11 * 18 + 1 = 199) is the largest valid paired comparison cell.
CPP_TABLES_PER_SHARD = 200
TPCC_PARTITIONED_TABLES = 11
TPCC_SHARED_TABLES = 1
MAX_PAIRED_WAREHOUSES = (
    CPP_TABLES_PER_SHARD - TPCC_SHARED_TABLES
) // TPCC_PARTITIONED_TABLES
GUARD_LXD_UNIT = "snap.lxd.daemon.service"
GUARD_STABILITY_SECONDS = 2.0
GUARD_CPU_IDLE_MIN_PERCENT = 95.0
GUARD_MAX_ATTEMPTS = 20
GUARD_RETRY_SECONDS = 0.25
LXD_ALIGNMENT_TIMEOUT_SECONDS = 180.0
RUNNER_PROCESS_NAMES = (
    "run_sto_tpcc_compare.py",
    "run_sto_masstree_compare.py",
)
BENCHMARK_PROCESS_NAMES = (
    "sto_masstree_compare",
    "sto_masstree_cpp_bench",
    "sto_tpcc_bench",
)
BUILD_PROCESS_NAMES = (
    "cargo",
    "rustc",
    "cc",
    "c++",
    "gcc",
    "g++",
    "clang",
    "clang++",
)
BUILD_COMMAND_MARKERS = ("mako", "sto", "masstree", "tpcc")


def positive_int(text: str) -> int:
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return value


def parse_csv_ints(text: str) -> list[int]:
    values = [positive_int(part.strip()) for part in text.split(",") if part.strip()]
    if not values:
        raise argparse.ArgumentTypeError("list must not be empty")
    if len(values) != len(set(values)):
        raise argparse.ArgumentTypeError("list values must be unique")
    return values


def parse_thread_counts(text: str) -> list[int]:
    values = parse_csv_ints(text)
    if max(values) > MAX_PAIRED_WAREHOUSES:
        raise argparse.ArgumentTypeError(
            f"paired TPC-C supports at most {MAX_PAIRED_WAREHOUSES} "
            "workers/warehouses: separate-tree mode needs 11 tables per "
            f"warehouse plus one shared table, but C++ reserves only "
            f"{CPP_TABLES_PER_SHARD} table IDs per shard"
        )
    return values


def parse_csv_cpus(text: str) -> list[int]:
    values = [int(part.strip()) for part in text.split(",") if part.strip()]
    if not values or any(value < 0 for value in values):
        raise argparse.ArgumentTypeError("CPU list must contain nonnegative integers")
    if len(values) != len(set(values)):
        raise argparse.ArgumentTypeError("CPU list values must be unique")
    return values


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--site", default="local_s0")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--threads",
        type=parse_thread_counts,
        default=parse_thread_counts("1,4,8,16"),
        help=(
            "comma-separated worker/warehouse counts "
            f"(default: 1,4,8,16; maximum: {MAX_PAIRED_WAREHOUSES})"
        ),
    )
    parser.add_argument("--repetitions", type=positive_int, default=5)
    parser.add_argument("--runtime-seconds", type=positive_int, default=30)
    parser.add_argument("--schedule-seed", type=int, default=0x5EED)
    parser.add_argument(
        "--physical-cpus",
        type=parse_csv_cpus,
        help="comma-separated CPU IDs; defaults to this process's allowed CPUs",
    )
    parser.add_argument("--taskset", default="taskset")
    parser.add_argument("--timeout-seconds", type=positive_int, default=3600)
    parser.add_argument(
        "--align-after-lxd-restart",
        action="store_true",
        help=(
            "before each pair attempt, wait for the next LXD daemon restart; "
            "useful when a known restart loop is shorter than a long pair"
        ),
    )
    args = parser.parse_args()

    if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
        parser.error(f"benchmark is not executable: {args.binary}")
    if not args.config.is_file():
        parser.error(f"configuration does not exist: {args.config}")
    if args.physical_cpus is None:
        try:
            args.physical_cpus = sorted(os.sched_getaffinity(0))
        except AttributeError:
            args.physical_cpus = list(range(os.cpu_count() or 1))
    if max(args.threads) > len(args.physical_cpus):
        parser.error("largest thread count exceeds the supplied/allowed CPU list")
    return args


def extract_result(stdout: str, expected_engine: str) -> dict[str, object]:
    records = [
        line[len(RESULT_PREFIX) :]
        for line in stdout.splitlines()
        if line.startswith(RESULT_PREFIX)
    ]
    if len(records) != 1:
        raise RuntimeError(f"expected one {RESULT_PREFIX!r} record, found {len(records)}")
    try:
        result = json.loads(records[0])
    except json.JSONDecodeError as error:
        raise RuntimeError(f"invalid TPC-C result JSON: {error}") from error
    validate_result(result, expected_engine)
    return result


def validate_result(result: dict[str, object], expected_engine: str) -> None:
    if result.get("schema_version") != 1:
        raise RuntimeError(f"unsupported result schema: {result.get('schema_version')!r}")
    if result.get("engine") != expected_engine:
        raise RuntimeError(
            f"unexpected engine {result.get('engine')!r}; expected {expected_engine!r}"
        )
    for field in ("threads", "warehouses", "configured_seconds", "commits", "aborts", "attempts"):
        value = result.get(field)
        if not isinstance(value, int) or value < 0:
            raise RuntimeError(f"result field {field!r} must be a nonnegative integer")
    if result["threads"] <= 0 or result["warehouses"] <= 0:
        raise RuntimeError("threads and warehouses must be positive")
    if result["attempts"] != result["commits"] + result["aborts"]:
        raise RuntimeError("result violates attempts == commits + aborts")
    measured = result.get("measured_seconds")
    throughput = result.get("throughput_txn_s")
    if not isinstance(measured, (int, float)) or measured <= 0:
        raise RuntimeError("measured_seconds must be positive")
    if not isinstance(throughput, (int, float)) or throughput < 0:
        raise RuntimeError("throughput_txn_s must be nonnegative")
    mix = result.get("mix")
    if not isinstance(mix, dict) or set(mix) != set(MIX_KEYS):
        raise RuntimeError(f"mix must contain exactly {', '.join(MIX_KEYS)}")
    if any(not isinstance(mix[key], int) or mix[key] < 0 for key in MIX_KEYS):
        raise RuntimeError("mix counters must be nonnegative integers")
    if sum(mix.values()) != result["commits"]:
        raise RuntimeError("result violates sum(mix counters) == commits")


def affinity(cpus: list[int], threads: int) -> str:
    return ",".join(str(cpu) for cpu in cpus[:threads])


def read_lxd_restart_count() -> int:
    command = [
        "systemctl",
        "show",
        GUARD_LXD_UNIT,
        "--property=NRestarts",
        "--value",
    ]
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
    )
    value = completed.stdout.strip()
    if completed.returncode != 0 or not value.isdecimal():
        raise RuntimeError(
            f"cannot read {GUARD_LXD_UNIT} NRestarts: "
            f"returncode={completed.returncode}, stdout={completed.stdout!r}, "
            f"stderr={completed.stderr!r}"
        )
    return int(value)


def read_cpu_times(cpus: list[int]) -> dict[int, tuple[int, int]]:
    wanted = {f"cpu{cpu}": cpu for cpu in cpus}
    samples: dict[int, tuple[int, int]] = {}
    with Path("/proc/stat").open(encoding="utf-8") as cpu_stat:
        for line in cpu_stat:
            fields = line.split()
            if not fields or fields[0] not in wanted:
                continue
            counters = [int(field) for field in fields[1:]]
            if len(counters) < 8:
                raise RuntimeError(f"{fields[0]} has an incomplete /proc/stat record")
            # guest and guest_nice are already included in user and nice. Match
            # mpstat's %idle definition by excluding iowait from the idle count.
            samples[wanted[fields[0]]] = (sum(counters[:8]), counters[3])
    missing = sorted(set(cpus) - set(samples))
    if missing:
        raise RuntimeError(f"selected guard CPUs are absent from /proc/stat: {missing}")
    return samples


def cpu_idle_percent(before: tuple[int, int], after: tuple[int, int]) -> float:
    total = after[0] - before[0]
    idle = after[1] - before[1]
    if total <= 0 or idle < 0 or idle > total:
        raise RuntimeError("selected CPU counters did not advance monotonically")
    return 100.0 * idle / total


def ancestor_pids() -> set[int]:
    ancestors: set[int] = set()
    pid = os.getpid()
    while pid > 0 and pid not in ancestors:
        ancestors.add(pid)
        try:
            status = Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            break
        parent = 0
        for line in status.splitlines():
            if line.startswith("PPid:"):
                parent = int(line.split()[1])
                break
        pid = parent
    return ancestors


def find_competing_processes(args: argparse.Namespace) -> list[dict[str, object]]:
    ignored = ancestor_pids()
    target_executable = str(args.binary.resolve())
    matches: list[dict[str, object]] = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        pid = int(entry.name)
        if pid in ignored:
            continue
        try:
            comm = (entry / "comm").read_text(encoding="utf-8").strip()
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        try:
            argv = [
                argument.decode("utf-8", errors="replace")
                for argument in (entry / "cmdline").read_bytes().split(b"\0")
                if argument
            ]
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            argv = []
        command = " ".join(argv)
        try:
            executable = os.readlink(entry / "exe").removesuffix(" (deleted)")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            executable = ""
        argv0_name = Path(argv[0]).name if argv else ""
        is_perf = comm == "perf" or comm.startswith("perf-")
        is_target = executable == target_executable
        is_runner = any(
            Path(argument).name in RUNNER_PROCESS_NAMES for argument in argv
        )
        is_benchmark = argv0_name in BENCHMARK_PROCESS_NAMES
        is_sto_build = (
            comm in BUILD_PROCESS_NAMES or argv0_name in BUILD_PROCESS_NAMES
        ) and any(marker in command.lower() for marker in BUILD_COMMAND_MARKERS)
        reasons = [
            reason
            for condition, reason in (
                (is_perf, "perf"),
                (is_target, "selected_benchmark_executable"),
                (is_runner, "benchmark_runner"),
                (is_benchmark, "benchmark_executable_name"),
                (is_sto_build, "sto_build"),
            )
            if condition
        ]
        if reasons:
            matches.append(
                {
                    "pid": pid,
                    "comm": comm,
                    "executable": executable,
                    "command": command,
                    "reasons": reasons,
                }
            )
    return sorted(matches, key=lambda process: int(process["pid"]))


def quiet_window_violations(sample: dict[str, object]) -> list[str]:
    violations: list[str] = []
    if sample["restart_count_before"] != sample["restart_count_after"]:
        violations.append("lxd NRestarts changed during the quiet window")
    idle = sample["cpu_idle_percent"]
    assert isinstance(idle, dict)
    if any(float(percent) < GUARD_CPU_IDLE_MIN_PERCENT for percent in idle.values()):
        violations.append(
            f"a selected CPU was below {GUARD_CPU_IDLE_MIN_PERCENT:g}% idle"
        )
    if sample["competing_before"] or sample["competing_after"]:
        violations.append("a competing STO/perf job was present")
    if float(sample["observed_seconds"]) < GUARD_STABILITY_SECONDS:
        violations.append("the quiet window was too short")
    return violations


def capture_quiet_window(args: argparse.Namespace, threads: int) -> dict[str, object]:
    cpus = args.physical_cpus[:threads]
    started = time.monotonic()
    restart_before = read_lxd_restart_count()
    competing_before = find_competing_processes(args)
    cpu_before = read_cpu_times(cpus)
    time.sleep(GUARD_STABILITY_SECONDS)
    cpu_after = read_cpu_times(cpus)
    competing_after = find_competing_processes(args)
    restart_after = read_lxd_restart_count()
    sample: dict[str, object] = {
        "observed_seconds": time.monotonic() - started,
        "restart_count_before": restart_before,
        "restart_count_after": restart_after,
        "cpu_idle_percent": {
            cpu: cpu_idle_percent(cpu_before[cpu], cpu_after[cpu]) for cpu in cpus
        },
        "competing_before": competing_before,
        "competing_after": competing_after,
    }
    sample["violations"] = quiet_window_violations(sample)
    return sample


def wait_for_quiet_window(
    args: argparse.Namespace, threads: int, pair_id: str, pair_attempt: int
) -> dict[str, object]:
    for window_attempt in range(1, GUARD_MAX_ATTEMPTS + 1):
        sample = capture_quiet_window(args, threads)
        violations = sample["violations"]
        assert isinstance(violations, list)
        if not violations:
            return sample
        print(
            f"[guard] {pair_id} attempt={pair_attempt} quiet-window "
            f"attempt={window_attempt} rejected: {', '.join(violations)}",
            flush=True,
        )
        time.sleep(GUARD_RETRY_SECONDS)
    raise RuntimeError(
        f"pair {pair_id} could not obtain a quiet host window after "
        f"{GUARD_MAX_ATTEMPTS} attempts"
    )


def wait_for_next_lxd_restart(pair_id: str, pair_attempt: int) -> dict[str, object]:
    before = read_lxd_restart_count()
    started = time.monotonic()
    deadline = started + LXD_ALIGNMENT_TIMEOUT_SECONDS
    print(
        f"[guard] {pair_id} attempt={pair_attempt} waiting for the next "
        f"LXD restart after NRestarts={before}",
        flush=True,
    )
    while time.monotonic() < deadline:
        time.sleep(GUARD_RETRY_SECONDS)
        after = read_lxd_restart_count()
        if after < before:
            raise RuntimeError("LXD NRestarts unexpectedly decreased")
        if after > before:
            waited = time.monotonic() - started
            print(
                f"[guard] {pair_id} attempt={pair_attempt} aligned after "
                f"{waited:.3f}s at NRestarts={after}",
                flush=True,
            )
            return {
                "restart_count_before": before,
                "restart_count_after": after,
                "waited_seconds": waited,
            }
    raise RuntimeError(
        f"pair {pair_id} saw no LXD restart within "
        f"{LXD_ALIGNMENT_TIMEOUT_SECONDS:g} seconds"
    )


def run_one(
    args: argparse.Namespace,
    engine: str,
    threads: int,
    repetition: int,
    run_order: int,
    pair_id: str,
) -> dict[str, object]:
    cpu_affinity = affinity(args.physical_cpus, threads)
    command = [
        args.taskset,
        "-c",
        cpu_affinity,
        str(args.binary),
        "--num-threads",
        str(threads),
        "--shard-config",
        str(args.config),
        "--site-name",
        args.site,
        "--runtime",
        str(args.runtime_seconds),
        "--storage-engine",
        engine,
    ]
    log_stem = f"{run_order:03d}-{pair_id}-{engine}"
    stdout_path = args.output_dir / f"{log_stem}.stdout.log"
    stderr_path = args.output_dir / f"{log_stem}.stderr.log"
    started = datetime.now(timezone.utc)
    started_monotonic = time.monotonic()
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=args.timeout_seconds,
    )
    elapsed_wall = time.monotonic() - started_monotonic
    stdout_path.write_text(completed.stdout, encoding="utf-8")
    stderr_path.write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(
            f"benchmark failed ({completed.returncode}): {shlex.join(command)}; "
            f"see {stdout_path} and {stderr_path}"
        )
    result = extract_result(completed.stdout, engine)
    expected = {
        "threads": threads,
        "warehouses": threads,
        "configured_seconds": args.runtime_seconds,
    }
    for field, value in expected.items():
        if result.get(field) != value:
            raise RuntimeError(
                f"result field {field}={result.get(field)!r}, expected {value!r}"
            )
    result.update(
        {
            "host": socket.gethostname(),
            "repetition": repetition,
            "run_order": run_order,
            "pair_id": pair_id,
            "cpu_affinity": cpu_affinity,
            "started_at_utc": started.isoformat(),
            "wall_seconds_including_load": elapsed_wall,
            "command": command,
            "stdout_log": stdout_path.name,
            "stderr_log": stderr_path.name,
        }
    )
    return result


def run_guarded_pair(
    args: argparse.Namespace,
    engines: list[str],
    threads: int,
    repetition: int,
    pair_id: str,
    first_run_order: int,
) -> list[dict[str, object]]:
    rejected_attempts: list[dict[str, object]] = []
    for pair_attempt in range(1, GUARD_MAX_ATTEMPTS + 1):
        alignment = None
        if getattr(args, "align_after_lxd_restart", False):
            alignment = wait_for_next_lxd_restart(pair_id, pair_attempt)
        window = wait_for_quiet_window(args, threads, pair_id, pair_attempt)
        if alignment is not None:
            window["lxd_restart_alignment"] = alignment
        restart_before = int(window["restart_count_after"])
        attempted_results: list[dict[str, object]] = []
        execution_error: Exception | None = None
        for pair_position, engine in enumerate(engines):
            logical_run_order = first_run_order + pair_position
            print(
                f"[{logical_run_order}] {engine} TPC-C "
                f"threads=warehouses={threads} repetition={repetition} "
                f"pair_attempt={pair_attempt}",
                flush=True,
            )
            try:
                attempted_results.append(
                    run_one(
                        args,
                        engine,
                        threads,
                        repetition,
                        logical_run_order,
                        pair_id,
                    )
                )
            except Exception as error:
                execution_error = error
                break

        rejection_reasons: list[str] = []
        try:
            restart_after: int | None = read_lxd_restart_count()
        except (OSError, RuntimeError, subprocess.TimeoutExpired):
            restart_after = None
            rejection_reasons.append("lxd_restart_count_unavailable")
        if restart_after is not None and restart_after != restart_before:
            rejection_reasons.append("lxd_restart_count_changed")

        try:
            competing_after = find_competing_processes(args)
        except OSError:
            competing_after = []
            rejection_reasons.append("competitor_check_unavailable")
        if competing_after:
            rejection_reasons.append("competing_sto_or_perf_job_started")

        if execution_error is not None and not rejection_reasons:
            raise execution_error
        if execution_error is not None:
            rejection_reasons.append("benchmark_failed_during_rejected_pair")
        if not rejection_reasons:
            if len(attempted_results) != len(engines):
                raise RuntimeError(f"pair {pair_id} completed with the wrong result count")
            guard = {
                "accepted_pair_attempt": pair_attempt,
                "quiet_window": window,
                "restart_count_after_pair": restart_after,
                "competing_after_pair": competing_after,
                "rejected_attempts": rejected_attempts,
            }
            for result in attempted_results:
                result["guard"] = guard
            return attempted_results

        rejected_attempts.append(
            {
                "pair_attempt": pair_attempt,
                "reasons": rejection_reasons,
                "completed_engines": [
                    str(result["engine"]) for result in attempted_results
                ],
                "restart_count_before": restart_before,
                "restart_count_after": restart_after,
                "competing_after_pair": competing_after,
            }
        )
        print(
            f"[guard] rejected both {pair_id} samples from attempt {pair_attempt}: "
            f"{', '.join(rejection_reasons)}",
            flush=True,
        )
        time.sleep(GUARD_RETRY_SECONDS)

    raise RuntimeError(
        f"pair {pair_id} did not produce an uncontaminated result after "
        f"{GUARD_MAX_ATTEMPTS} attempts"
    )


def summarize(results: list[dict[str, object]]) -> list[dict[str, object]]:
    groups: dict[tuple[int, str], list[dict[str, object]]] = defaultdict(list)
    pairs: dict[str, dict[str, dict[str, object]]] = defaultdict(dict)
    for result in results:
        groups[(int(result["threads"]), str(result["engine"]))].append(result)
        pairs[str(result["pair_id"])][str(result["engine"])] = result

    ratios: dict[int, list[float]] = defaultdict(list)
    for pair in pairs.values():
        if set(pair) != set(ENGINES):
            continue
        cpp = float(pair["cpp"]["throughput_txn_s"])
        rust = float(pair["rust"]["throughput_txn_s"])
        if cpp > 0:
            ratios[int(pair["cpp"]["threads"])].append(100.0 * rust / cpp)

    rows: list[dict[str, object]] = []
    for (threads, engine), samples in sorted(groups.items()):
        throughputs = [float(sample["throughput_txn_s"]) for sample in samples]
        abort_percent = [
            100.0 * int(sample["aborts"]) / max(1, int(sample["attempts"]))
            for sample in samples
        ]
        rows.append(
            {
                "engine": engine,
                "threads": threads,
                "warehouses": threads,
                "repetitions": len(samples),
                "median_txn_per_sec": statistics.median(throughputs),
                "min_txn_per_sec": min(throughputs),
                "max_txn_per_sec": max(throughputs),
                "median_abort_percent": statistics.median(abort_percent),
                "median_rust_percent_of_cpp": (
                    statistics.median(ratios[threads]) if ratios[threads] else None
                ),
            }
        )
    return rows


def write_summary(path: Path, results: list[dict[str, object]]) -> None:
    rows = summarize(results)
    columns = [
        "engine",
        "threads",
        "warehouses",
        "repetitions",
        "median_txn_per_sec",
        "min_txn_per_sec",
        "max_txn_per_sec",
        "median_abort_percent",
        "median_rust_percent_of_cpp",
    ]
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        for row in rows:
            formatted = dict(row)
            for field in (
                "median_txn_per_sec",
                "min_txn_per_sec",
                "max_txn_per_sec",
                "median_abort_percent",
                "median_rust_percent_of_cpp",
            ):
                value = formatted[field]
                formatted[field] = "" if value is None else f"{float(value):.6f}"
            writer.writerow(formatted)


def command_output(command: list[str]) -> str | None:
    try:
        completed = subprocess.run(
            command, check=True, capture_output=True, text=True, timeout=10
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return completed.stdout.strip()


def file_fingerprint(path: Path) -> dict[str, object]:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    stat = path.stat()
    return {
        "sha256": digest.hexdigest(),
        "size_bytes": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
    }


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    raw_path = args.output_dir / "raw.jsonl"
    summary_path = args.output_dir / "summary.csv"
    metadata_path = args.output_dir / "run.json"
    if raw_path.exists():
        raise RuntimeError(f"refusing to overwrite existing results: {raw_path}")

    metadata = {
        "schema_version": 1,
        "host": socket.gethostname(),
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "binary": str(args.binary.resolve()),
        "binary_fingerprint": file_fingerprint(args.binary),
        "config": str(args.config.resolve()),
        "config_fingerprint": file_fingerprint(args.config),
        "site": args.site,
        "threads_and_warehouses": args.threads,
        "repetitions": args.repetitions,
        "runtime_seconds": args.runtime_seconds,
        "schedule_seed": args.schedule_seed,
        "physical_cpus": args.physical_cpus,
        "align_after_lxd_restart": args.align_after_lxd_restart,
        "git_commit": command_output(
            [
                "git",
                "-c",
                f"safe.directory={Path.cwd().resolve()}",
                "rev-parse",
                "HEAD",
            ]
        ),
        "git_status": command_output(
            [
                "git",
                "-c",
                f"safe.directory={Path.cwd().resolve()}",
                "status",
                "--short",
            ]
        ),
        "uname": command_output(["uname", "-a"]),
        "lscpu": command_output(["lscpu"]),
        "protocol": (
            "For each repetition, shuffle thread-count cells; run cpp/rust "
            "adjacently and alternate which engine runs first. Each process "
            "loads a fresh database. Throughput is the benchmark's measured "
            "interval, excluding its deliberate shutdown sleep. Before each "
            "pair, require a two-second quiet window with stable LXD restarts, "
            "at least 95% idle on every selected CPU, and no recognized "
            "competing STO/perf job. Reject and retry both samples if the LXD "
            "counter changes or a competitor is present after the pair."
            + (
                " Before every pair attempt, wait for the next LXD restart "
                "before applying the quiet-window gate."
                if args.align_after_lxd_restart
                else ""
            )
        ),
    }
    metadata_path.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    rng = random.Random(args.schedule_seed)
    results: list[dict[str, object]] = []
    run_order = 0
    with raw_path.open("x", encoding="utf-8") as raw_output:
        for repetition in range(args.repetitions):
            cells = list(args.threads)
            rng.shuffle(cells)
            for cell_index, threads in enumerate(cells):
                pair_id = f"r{repetition:02d}-c{cell_index:02d}-{threads}t"
                engines = list(ENGINES)
                if (repetition + cell_index) % 2:
                    engines.reverse()
                pair_results = run_guarded_pair(
                    args,
                    engines,
                    threads,
                    repetition,
                    pair_id,
                    run_order + 1,
                )
                for result in pair_results:
                    raw_output.write(json.dumps(result, sort_keys=True) + "\n")
                    results.append(result)
                    write_summary(summary_path, results)
                raw_output.flush()
                run_order += len(pair_results)

    print(f"raw results: {raw_path}")
    print(f"summary: {summary_path}")
    print(f"metadata: {metadata_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
