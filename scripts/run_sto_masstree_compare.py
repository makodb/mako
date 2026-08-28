#!/usr/bin/env python3
"""Run and summarize the matched C++/Rust STO Masstree benchmark."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import random
import socket
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import TextIO


RESULT_PREFIX = "BENCH_RESULT="
CPP_ENGINE = "cpp-sto-masstree"
RUST_ENGINES = {
    "binary": "rust-sto-masstree",
    "fixed-u64": "rust-sto-masstree-fixed-u64",
}
GUARD_SCHEMA_VERSION = 1
GUARD_MIN_STABILITY_SECONDS = 2.0
COMPETING_COMMAND_MARKERS = (
    "sto_masstree_compare",
    "sto_masstree_cpp_bench",
    "sto-masstree",
)
RUNNER_PROCESS_NAME = "run_sto_masstree_compare.py"
BENCHMARK_PROCESS_NAMES = ("sto_masstree_compare", "sto_masstree_cpp_bench")
BUILD_PROCESS_NAMES = ("cargo", "rustc", "cc", "c++", "gcc", "g++", "clang", "clang++")


@dataclass(frozen=True)
class Scenario:
    name: str
    ops_per_txn: int
    write_percent: int


def positive_int(text: str) -> int:
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return value


def nonnegative_int(text: str) -> int:
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError("value must be nonnegative")
    return value


def positive_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return value


def guard_stability_seconds(text: str) -> float:
    value = positive_float(text)
    if value < GUARD_MIN_STABILITY_SECONDS:
        raise argparse.ArgumentTypeError(
            f"guard stability must be at least {GUARD_MIN_STABILITY_SECONDS:g} seconds"
        )
    return value


def percent(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or not 0.0 <= value <= 100.0:
        raise argparse.ArgumentTypeError("percentage must be in 0..=100")
    return value


def parse_csv_ints(text: str) -> list[int]:
    values = [positive_int(part.strip()) for part in text.split(",") if part.strip()]
    if not values:
        raise argparse.ArgumentTypeError("list must not be empty")
    if len(values) != len(set(values)):
        raise argparse.ArgumentTypeError("list values must be unique")
    return values


def parse_csv_nonnegative_ints(text: str) -> list[int]:
    values = [nonnegative_int(part.strip()) for part in text.split(",") if part.strip()]
    if not values:
        raise argparse.ArgumentTypeError("list must not be empty")
    if len(values) != len(set(values)):
        raise argparse.ArgumentTypeError("list values must be unique")
    return values


def parse_scenario(text: str) -> Scenario:
    try:
        name, ops_text, writes_text = text.split(":", 2)
        ops = positive_int(ops_text)
        writes = nonnegative_int(writes_text)
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError(
            "scenario must have the form NAME:OPS_PER_TXN:WRITE_PERCENT"
        ) from error
    if not name or writes > 100:
        raise argparse.ArgumentTypeError("scenario name must be set and writes must be 0..100")
    return Scenario(name, ops, writes)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp", type=Path, required=True)
    parser.add_argument("--rust", type=Path, required=True)
    parser.add_argument(
        "--rust-value-mode",
        choices=tuple(RUST_ENGINES),
        default="binary",
        help="Rust table representation to select (default: binary)",
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--threads", type=parse_csv_ints, default=parse_csv_ints("1,2,4,8,16,32,64"))
    parser.add_argument(
        "--scenario",
        action="append",
        type=parse_scenario,
        dest="scenarios",
        help="NAME:OPS_PER_TXN:WRITE_PERCENT; may be repeated",
    )
    parser.add_argument("--repetitions", type=positive_int, default=3)
    parser.add_argument("--keyspace", type=positive_int, default=100_000)
    parser.add_argument("--warmup-ms", type=nonnegative_int, default=1_000)
    parser.add_argument("--duration-ms", type=positive_int, default=3_000)
    parser.add_argument("--seed", type=nonnegative_int, default=1)
    parser.add_argument("--schedule-seed", type=nonnegative_int, default=0x5EED)
    parser.add_argument(
        "--physical-cpus",
        type=parse_csv_nonnegative_ints,
        default=parse_csv_nonnegative_ints(",".join(str(i) for i in range(64))),
    )
    parser.add_argument("--taskset", default="taskset")
    parser.add_argument("--timeout-seconds", type=positive_int, default=600)
    guard = parser.add_argument_group("optional shared-zoo host guard")
    guard.add_argument(
        "--guard-zoo-lxd",
        action="store_true",
        help=(
            "gate the run on an idle host and stable LXD restart counter, then "
            "reject and retry adjacent engine pairs crossed by an LXD restart"
        ),
    )
    guard.add_argument(
        "--guard-lxd-unit",
        default="snap.lxd.daemon.service",
        help=(
            "systemd unit whose NRestarts counter guards each pair "
            "(default: snap.lxd.daemon.service)"
        ),
    )
    guard.add_argument(
        "--guard-systemctl",
        default="systemctl",
        help="systemctl executable used by the zoo guard (default: systemctl)",
    )
    guard.add_argument(
        "--guard-stability-seconds",
        type=guard_stability_seconds,
        default=GUARD_MIN_STABILITY_SECONDS,
        help="minimum unchanged NRestarts observation window (default: 2.0)",
    )
    guard.add_argument(
        "--guard-load1-max",
        type=positive_float,
        default=4.0,
        help="initial preflight requires load1 strictly below this value (default: 4.0)",
    )
    guard.add_argument(
        "--guard-cpu-idle-min",
        type=percent,
        default=95.0,
        help="initial preflight idle minimum for the first selected CPU (default: 95.0)",
    )
    guard.add_argument(
        "--guard-max-attempts",
        type=positive_int,
        default=20,
        help="maximum attempts for preflight, pair windows, and one accepted pair (default: 20)",
    )
    guard.add_argument(
        "--guard-retry-delay-ms",
        type=nonnegative_int,
        default=250,
        help="delay between rejected guard attempts (default: 250)",
    )
    args = parser.parse_args()
    if args.scenarios is None:
        args.scenarios = [
            Scenario("read10", 10, 0),
            Scenario("rw95_10", 10, 5),
            Scenario("rw50_10", 10, 50),
        ]
    if max(args.threads) > len(args.physical_cpus):
        parser.error("thread count exceeds the supplied physical CPU list")
    if max(scenario.ops_per_txn for scenario in args.scenarios) > args.keyspace:
        parser.error("keyspace must be at least the largest operations-per-transaction value")
    for executable in (args.cpp, args.rust):
        if not executable.is_file() or not os.access(executable, os.X_OK):
            parser.error(f"benchmark is not executable: {executable}")
    return args


def extract_result(stdout: str, expected_engine: str) -> dict[str, object]:
    matches = [
        line[len(RESULT_PREFIX) :]
        for line in stdout.splitlines()
        if line.startswith(RESULT_PREFIX)
    ]
    if len(matches) != 1:
        raise RuntimeError(f"expected one {RESULT_PREFIX} line, found {len(matches)}")
    result = json.loads(matches[0])
    if result.get("engine") != expected_engine:
        raise RuntimeError(
            f"unexpected engine in result: {result.get('engine')!r}; "
            f"expected {expected_engine!r}"
        )
    return result


def affinity(cpus: list[int], threads: int) -> str:
    return ",".join(str(cpu) for cpu in cpus[:threads])


@dataclass(frozen=True)
class CpuTimes:
    total: int
    idle: int


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def read_cpu_times(cpu: int) -> CpuTimes:
    label = f"cpu{cpu}"
    with Path("/proc/stat").open(encoding="utf-8") as cpu_stat:
        for line in cpu_stat:
            fields = line.split()
            if not fields or fields[0] != label:
                continue
            counters = [int(field) for field in fields[1:]]
            if len(counters) < 8:
                raise RuntimeError(f"{label} has an incomplete /proc/stat record")
            # Linux's guest counters are already included in user/nice. Sum the
            # first eight counters so they are not counted twice. mpstat reports
            # iowait separately from %idle, so only the fourth counter is idle.
            return CpuTimes(total=sum(counters[:8]), idle=counters[3])
    raise RuntimeError(f"selected guard CPU is absent from /proc/stat: {cpu}")


def cpu_idle_percent(before: CpuTimes, after: CpuTimes) -> float:
    total = after.total - before.total
    idle = after.idle - before.idle
    if total <= 0 or idle < 0 or idle > total:
        raise RuntimeError("selected CPU counters did not advance monotonically")
    return 100.0 * idle / total


def read_lxd_restart_count(args: argparse.Namespace) -> int:
    command = [
        args.guard_systemctl,
        "show",
        args.guard_lxd_unit,
        "--property=NRestarts",
        "--value",
    ]
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=min(args.timeout_seconds, 30),
    )
    text = completed.stdout.strip()
    if completed.returncode != 0 or not text.isdecimal():
        raise RuntimeError(
            f"cannot read {args.guard_lxd_unit} NRestarts with {' '.join(command)}: "
            f"returncode={completed.returncode}, stdout={completed.stdout!r}, "
            f"stderr={completed.stderr!r}"
        )
    return int(text)


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
    target_executables = {str(args.cpp.resolve()), str(args.rust.resolve())}
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
            executable = os.readlink(entry / "exe")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            executable = ""
        is_perf = comm == "perf" or comm.startswith("perf-")
        is_target = executable.removesuffix(" (deleted)") in target_executables
        argv0_name = Path(argv[0]).name if argv else ""
        is_runner = any(Path(argument).name == RUNNER_PROCESS_NAME for argument in argv)
        is_benchmark = argv0_name in BENCHMARK_PROCESS_NAMES
        is_sto_build = (
            comm in BUILD_PROCESS_NAMES or argv0_name in BUILD_PROCESS_NAMES
        ) and any(marker in command for marker in COMPETING_COMMAND_MARKERS)
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


def host_gate_violations(sample: dict[str, object], args: argparse.Namespace) -> list[str]:
    violations: list[str] = []
    if sample["restart_count_before"] != sample["restart_count_after"]:
        violations.append("lxd NRestarts changed during the stability window")
    if max(float(sample["load1_before"]), float(sample["load1_after"])) >= args.guard_load1_max:
        violations.append(f"load1 was not below {args.guard_load1_max:g}")
    if float(sample["cpu_idle_percent"]) < args.guard_cpu_idle_min:
        violations.append(
            f"CPU {sample['cpu']} idle was below {args.guard_cpu_idle_min:g}%"
        )
    if sample["competing_before"] or sample["competing_after"]:
        violations.append("a competing STO/perf job was present")
    if float(sample["observed_seconds"]) < args.guard_stability_seconds:
        violations.append("the LXD observation window was too short")
    return violations


def capture_initial_host_gate(args: argparse.Namespace) -> dict[str, object]:
    cpu = args.physical_cpus[0]
    started_at = utc_now()
    started = time.monotonic()
    restart_before = read_lxd_restart_count(args)
    load_before = os.getloadavg()[0]
    competing_before = find_competing_processes(args)
    cpu_before = read_cpu_times(cpu)
    time.sleep(args.guard_stability_seconds)
    cpu_after = read_cpu_times(cpu)
    competing_after = find_competing_processes(args)
    load_after = os.getloadavg()[0]
    restart_after = read_lxd_restart_count(args)
    sample: dict[str, object] = {
        "started_at_utc": started_at,
        "finished_at_utc": utc_now(),
        "observed_seconds": time.monotonic() - started,
        "restart_count_before": restart_before,
        "restart_count_after": restart_after,
        "load1_before": load_before,
        "load1_after": load_after,
        "cpu": cpu,
        "cpu_total_ticks": cpu_after.total - cpu_before.total,
        "cpu_idle_ticks": cpu_after.idle - cpu_before.idle,
        "cpu_idle_percent": cpu_idle_percent(cpu_before, cpu_after),
        "competing_before": competing_before,
        "competing_after": competing_after,
    }
    sample["violations"] = host_gate_violations(sample, args)
    sample["accepted"] = not sample["violations"]
    return sample


def capture_pair_window(args: argparse.Namespace) -> dict[str, object]:
    # Reapply the complete idle-host gate immediately before every adjacent
    # pair; an initial pass alone cannot establish that later samples are idle.
    return capture_initial_host_gate(args)


def guard_retry_delay(args: argparse.Namespace) -> None:
    if args.guard_retry_delay_ms:
        time.sleep(args.guard_retry_delay_ms / 1_000.0)


def write_guard_metadata(path: Path, metadata: dict[str, object]) -> None:
    path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def wait_for_initial_host_gate(
    args: argparse.Namespace,
    metadata: dict[str, object],
    metadata_path: Path,
) -> dict[str, object]:
    attempts = metadata["initial_preflight"]
    assert isinstance(attempts, list)
    for attempt in range(1, args.guard_max_attempts + 1):
        print(f"[guard] initial host preflight attempt {attempt}", flush=True)
        sample = capture_initial_host_gate(args)
        sample["attempt"] = attempt
        attempts.append(sample)
        write_guard_metadata(metadata_path, metadata)
        if sample["accepted"]:
            return sample
        print(f"[guard] preflight rejected: {', '.join(sample['violations'])}", flush=True)
        guard_retry_delay(args)
    raise RuntimeError(
        f"zoo host preflight did not pass after {args.guard_max_attempts} attempts"
    )


def wait_for_pair_window(
    args: argparse.Namespace,
    pair_id: str,
    pair_attempt: int,
    metadata: dict[str, object],
    metadata_path: Path,
) -> dict[str, object]:
    windows = metadata["pair_windows"]
    assert isinstance(windows, list)
    for window_attempt in range(1, args.guard_max_attempts + 1):
        sample = capture_pair_window(args)
        sample.update(
            {
                "pair_id": pair_id,
                "pair_attempt": pair_attempt,
                "window_attempt": window_attempt,
            }
        )
        windows.append(sample)
        write_guard_metadata(metadata_path, metadata)
        if sample["accepted"]:
            return sample
        print(f"[guard] pair window rejected: {', '.join(sample['violations'])}", flush=True)
        guard_retry_delay(args)
    raise RuntimeError(
        f"pair {pair_id} could not obtain a stable LXD window after "
        f"{args.guard_max_attempts} attempts"
    )


def run_one(
    args: argparse.Namespace,
    executable: Path,
    scenario: Scenario,
    threads: int,
    repetition: int,
    run_order: int,
) -> dict[str, object]:
    seed = args.seed + repetition
    command = [
        args.taskset,
        "-c",
        affinity(args.physical_cpus, threads),
        str(executable),
        "--threads",
        str(threads),
        "--keyspace",
        str(args.keyspace),
        "--ops-per-txn",
        str(scenario.ops_per_txn),
        "--write-percent",
        str(scenario.write_percent),
        "--warmup-ms",
        str(args.warmup_ms),
        "--duration-ms",
        str(args.duration_ms),
        "--seed",
        str(seed),
    ]
    expected_engine = CPP_ENGINE
    if executable == args.rust:
        expected_engine = RUST_ENGINES[args.rust_value_mode]
        if args.rust_value_mode != "binary":
            command.extend(["--value-mode", args.rust_value_mode])
    started = datetime.now(timezone.utc)
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=args.timeout_seconds,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"benchmark failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    result = extract_result(completed.stdout, expected_engine)
    expected = {
        "engine": expected_engine,
        "threads": threads,
        "keyspace": args.keyspace,
        "ops_per_txn": scenario.ops_per_txn,
        "write_percent": scenario.write_percent,
        "warmup_ms": args.warmup_ms,
        "duration_ms": args.duration_ms,
        "seed": seed,
    }
    for field, value in expected.items():
        if result.get(field) != value:
            raise RuntimeError(f"result field {field}={result.get(field)!r}, expected {value!r}")
    commits = int(result["commits"])
    attempts = int(result["attempts"])
    aborts = int(result["aborts"])
    logical_ops = int(result["logical_ops"])
    if attempts != commits + aborts:
        raise RuntimeError("result violates attempts == commits + aborts")
    if logical_ops != commits * scenario.ops_per_txn:
        raise RuntimeError("result violates logical_ops == commits * ops_per_txn")
    if int(result["elapsed_ns"]) <= 0:
        raise RuntimeError("result elapsed_ns must be positive")
    result.update(
        {
            "scenario": scenario.name,
            "repetition": repetition,
            "run_order": run_order,
            "cpu_affinity": affinity(args.physical_cpus, threads),
            "host": socket.gethostname(),
            "started_at_utc": started.isoformat(),
            "command": command,
        }
    )
    return result


def new_guard_metadata(args: argparse.Namespace, rejected_path: Path) -> dict[str, object]:
    return {
        "schema_version": GUARD_SCHEMA_VERSION,
        "host": socket.gethostname(),
        "started_at_utc": utc_now(),
        "status": "running",
        "policy": {
            "mode": "zoo-lxd",
            "lxd_unit": args.guard_lxd_unit,
            "systemctl_command": [
                args.guard_systemctl,
                "show",
                args.guard_lxd_unit,
                "--property=NRestarts",
                "--value",
            ],
            "stability_seconds": args.guard_stability_seconds,
            "initial_load1_strict_max": args.guard_load1_max,
            "initial_cpu": args.physical_cpus[0],
            "initial_cpu_idle_min_percent": args.guard_cpu_idle_min,
            "cpu_idle_definition": (
                "delta idle / delta sum(user,nice,system,idle,iowait,irq,softirq,steal)"
            ),
            "competing_command_markers": list(COMPETING_COMMAND_MARKERS),
            "competing_runner_name": RUNNER_PROCESS_NAME,
            "competing_benchmark_names": list(BENCHMARK_PROCESS_NAMES),
            "competing_build_names": list(BUILD_PROCESS_NAMES),
            "competing_perf_rule": "comm == perf or comm starts with perf-",
            "max_attempts": args.guard_max_attempts,
            "retry_delay_ms": args.guard_retry_delay_ms,
            "pair_policy": (
                "before every adjacent pair, require stable NRestarts, load1, selected-CPU "
                "idle, and no competing job; reject both samples if the post-pair "
                "NRestarts check changes or a competing job appears"
            ),
        },
        "artifacts": {"rejected_jsonl": rejected_path.name},
        "initial_preflight": [],
        "pair_windows": [],
        "pair_attempts": [],
        "accepted_pairs": 0,
        "rejected_pairs": 0,
    }


def run_guarded_pair(
    args: argparse.Namespace,
    executables: list[Path],
    scenario: Scenario,
    threads: int,
    repetition: int,
    pair_id: str,
    first_run_order: int,
    execution_order: int,
    guard_metadata: dict[str, object],
    guard_metadata_path: Path,
    rejected_output: TextIO,
) -> tuple[list[dict[str, object]], int]:
    pair_attempts = guard_metadata["pair_attempts"]
    assert isinstance(pair_attempts, list)
    for pair_attempt in range(1, args.guard_max_attempts + 1):
        window = wait_for_pair_window(
            args,
            pair_id,
            pair_attempt,
            guard_metadata,
            guard_metadata_path,
        )
        restart_before = int(window["restart_count_after"])
        pair_started = utc_now()
        attempted_results: list[dict[str, object]] = []
        execution_error: Exception | None = None
        for pair_position, executable in enumerate(executables):
            execution_order += 1
            logical_run_order = first_run_order + pair_position
            print(
                f"[execution {execution_order}; accepted slot {logical_run_order}] "
                f"{executable.name} {scenario.name} threads={threads} "
                f"repetition={repetition} pair_attempt={pair_attempt}",
                flush=True,
            )
            try:
                result = run_one(
                    args,
                    executable,
                    scenario,
                    threads,
                    repetition,
                    logical_run_order,
                )
            except Exception as error:
                execution_error = error
                break
            result.update(
                {
                    "pair_id": pair_id,
                    "pair_attempt": pair_attempt,
                    "pair_position": pair_position,
                    "execution_order": execution_order,
                    "guard_restart_count_before": restart_before,
                    "guard_pair_window": window,
                }
            )
            attempted_results.append(result)

        restart_check_error: Exception | None = None
        try:
            restart_after: int | None = read_lxd_restart_count(args)
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            restart_after = None
            restart_check_error = error
        competing_after = find_competing_processes(args)
        rejection_reasons = []
        if restart_check_error is not None:
            rejection_reasons.append("lxd_restart_count_unavailable")
        elif restart_before != restart_after:
            rejection_reasons.append("lxd_restart_count_changed")
        if competing_after:
            rejection_reasons.append("competing_sto_or_perf_job_started")
        if execution_error is not None and not rejection_reasons:
            raise execution_error
        if execution_error is not None:
            rejection_reasons.append("benchmark_failed_during_rejected_pair")
        accepted = not rejection_reasons
        attempt_record = {
            "pair_id": pair_id,
            "pair_attempt": pair_attempt,
            "scenario": scenario.name,
            "threads": threads,
            "repetition": repetition,
            "engine_order": [
                str(result["engine"]) for result in attempted_results
            ],
            "started_at_utc": pair_started,
            "finished_at_utc": utc_now(),
            "restart_count_before": restart_before,
            "restart_count_after": restart_after,
            "restart_check_error": (
                None
                if restart_check_error is None
                else f"{type(restart_check_error).__name__}: {restart_check_error}"
            ),
            "competing_after": competing_after,
            "execution_orders": [
                int(result["execution_order"]) for result in attempted_results
            ],
            "accepted": accepted,
            "reasons": rejection_reasons,
            "execution_error": (
                None
                if execution_error is None
                else f"{type(execution_error).__name__}: {execution_error}"
            ),
        }
        pair_attempts.append(attempt_record)
        for result in attempted_results:
            result["guard_restart_count_after"] = restart_after
            result["guard_competing_after"] = competing_after

        if accepted:
            guard_metadata["accepted_pairs"] = int(guard_metadata["accepted_pairs"]) + 1
            write_guard_metadata(guard_metadata_path, guard_metadata)
            return attempted_results, execution_order

        guard_metadata["rejected_pairs"] = int(guard_metadata["rejected_pairs"]) + 1
        rejected_record = dict(attempt_record)
        rejected_record["results"] = attempted_results
        rejected_output.write(json.dumps(rejected_record, sort_keys=True) + "\n")
        rejected_output.flush()
        write_guard_metadata(guard_metadata_path, guard_metadata)
        print(
            f"[guard] rejected both {pair_id} samples: "
            f"{', '.join(rejection_reasons)}",
            flush=True,
        )
        guard_retry_delay(args)

    raise RuntimeError(
        f"pair {pair_id} did not produce a verifiably uncontaminated result "
        f"after {args.guard_max_attempts} attempts"
    )


def write_summary(path: Path, results: list[dict[str, object]]) -> None:
    groups: dict[tuple[object, ...], list[dict[str, object]]] = defaultdict(list)
    for result in results:
        key = (result["engine"], result["scenario"], result["threads"])
        groups[key].append(result)

    columns = [
        "engine",
        "scenario",
        "threads",
        "repetitions",
        "median_txn_per_sec",
        "median_ops_per_sec",
        "min_ops_per_sec",
        "max_ops_per_sec",
        "median_abort_percent",
    ]
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        for key in sorted(groups, key=lambda item: (str(item[1]), int(item[2]), str(item[0]))):
            samples = groups[key]
            ops = [float(sample["ops_per_sec"]) for sample in samples]
            txns = [float(sample["txn_per_sec"]) for sample in samples]
            aborts = [
                100.0 * int(sample["aborts"]) / max(1, int(sample["attempts"]))
                for sample in samples
            ]
            writer.writerow(
                {
                    "engine": key[0],
                    "scenario": key[1],
                    "threads": key[2],
                    "repetitions": len(samples),
                    "median_txn_per_sec": f"{statistics.median(txns):.3f}",
                    "median_ops_per_sec": f"{statistics.median(ops):.3f}",
                    "min_ops_per_sec": f"{min(ops):.3f}",
                    "max_ops_per_sec": f"{max(ops):.3f}",
                    "median_abort_percent": f"{statistics.median(aborts):.6f}",
                }
            )


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    raw_path = args.output_dir / "raw.jsonl"
    summary_path = args.output_dir / "summary.csv"
    metadata_path = args.output_dir / "run.json"
    rejected_path = args.output_dir / "rejected.jsonl"
    guard_metadata_path = args.output_dir / "guard.json"
    if raw_path.exists():
        raise RuntimeError(f"refusing to overwrite existing results: {raw_path}")
    if args.guard_zoo_lxd:
        for guard_path in (rejected_path, guard_metadata_path):
            if guard_path.exists():
                raise RuntimeError(f"refusing to overwrite existing guard artifact: {guard_path}")

    metadata = {
        "host": socket.gethostname(),
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "cpp": str(args.cpp.resolve()),
        "rust": str(args.rust.resolve()),
        "rust_value_mode": args.rust_value_mode,
        "threads": args.threads,
        "scenarios": [scenario.__dict__ for scenario in args.scenarios],
        "repetitions": args.repetitions,
        "keyspace": args.keyspace,
        "warmup_ms": args.warmup_ms,
        "duration_ms": args.duration_ms,
        "seed": args.seed,
        "schedule_seed": args.schedule_seed,
        "physical_cpus": args.physical_cpus,
    }
    if args.guard_zoo_lxd:
        metadata["guard"] = {
            "mode": "zoo-lxd",
            "metadata": guard_metadata_path.name,
            "rejected_results": rejected_path.name,
            "lxd_unit": args.guard_lxd_unit,
            "stability_seconds": args.guard_stability_seconds,
            "initial_load1_strict_max": args.guard_load1_max,
            "initial_cpu_idle_min_percent": args.guard_cpu_idle_min,
            "max_attempts": args.guard_max_attempts,
            "retry_delay_ms": args.guard_retry_delay_ms,
        }
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    rng = random.Random(args.schedule_seed)
    results: list[dict[str, object]] = []
    run_order = 0
    if not args.guard_zoo_lxd:
        with raw_path.open("x", encoding="utf-8") as raw_output:
            for repetition in range(args.repetitions):
                cells = [
                    (scenario, threads)
                    for scenario in args.scenarios
                    for threads in args.threads
                ]
                rng.shuffle(cells)
                for cell_index, (scenario, threads) in enumerate(cells):
                    executables = [args.cpp, args.rust]
                    if (repetition + cell_index) % 2:
                        executables.reverse()
                    for executable in executables:
                        run_order += 1
                        print(
                            f"[{run_order}] {executable.name} {scenario.name} "
                            f"threads={threads} repetition={repetition}",
                            flush=True,
                        )
                        result = run_one(
                            args,
                            executable,
                            scenario,
                            threads,
                            repetition,
                            run_order,
                        )
                        raw_output.write(json.dumps(result, sort_keys=True) + "\n")
                        raw_output.flush()
                        results.append(result)
                        write_summary(summary_path, results)

        print(f"raw results: {raw_path}")
        print(f"summary: {summary_path}")
        return 0

    guard_metadata = new_guard_metadata(args, rejected_path)
    write_guard_metadata(guard_metadata_path, guard_metadata)
    execution_order = 0
    try:
        with (
            raw_path.open("x", encoding="utf-8") as raw_output,
            rejected_path.open("x", encoding="utf-8") as rejected_output,
        ):
            wait_for_initial_host_gate(args, guard_metadata, guard_metadata_path)
            for repetition in range(args.repetitions):
                cells = [
                    (scenario, threads)
                    for scenario in args.scenarios
                    for threads in args.threads
                ]
                rng.shuffle(cells)
                for cell_index, (scenario, threads) in enumerate(cells):
                    executables = [args.cpp, args.rust]
                    if (repetition + cell_index) % 2:
                        executables.reverse()
                    pair_id = (
                        f"r{repetition:03d}-c{cell_index:03d}-"
                        f"{scenario.name}-{threads}t"
                    )
                    pair_results, execution_order = run_guarded_pair(
                        args,
                        executables,
                        scenario,
                        threads,
                        repetition,
                        pair_id,
                        run_order + 1,
                        execution_order,
                        guard_metadata,
                        guard_metadata_path,
                        rejected_output,
                    )
                    for result in pair_results:
                        raw_output.write(json.dumps(result, sort_keys=True) + "\n")
                    raw_output.flush()
                    results.extend(pair_results)
                    run_order += len(pair_results)
                    write_summary(summary_path, results)
    except BaseException as error:
        guard_metadata.update(
            {
                "status": "failed",
                "finished_at_utc": utc_now(),
                "failure": f"{type(error).__name__}: {error}",
                "accepted_results": len(results),
                "executed_results": execution_order,
            }
        )
        write_guard_metadata(guard_metadata_path, guard_metadata)
        raise
    else:
        guard_metadata.update(
            {
                "status": "complete",
                "finished_at_utc": utc_now(),
                "accepted_results": len(results),
                "executed_results": execution_order,
            }
        )
        write_guard_metadata(guard_metadata_path, guard_metadata)

    print(f"raw results: {raw_path}")
    print(f"summary: {summary_path}")
    print(f"rejected results: {rejected_path}")
    print(f"guard metadata: {guard_metadata_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
