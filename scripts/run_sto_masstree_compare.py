#!/usr/bin/env python3
"""Run and summarize the matched C++/Rust STO Masstree benchmark."""

from __future__ import annotations

import argparse
import csv
import json
import os
import random
import socket
import statistics
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


RESULT_PREFIX = "BENCH_RESULT="
ENGINES = ("cpp-sto-masstree", "rust-sto-masstree")


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


def extract_result(stdout: str) -> dict[str, object]:
    matches = [
        line[len(RESULT_PREFIX) :]
        for line in stdout.splitlines()
        if line.startswith(RESULT_PREFIX)
    ]
    if len(matches) != 1:
        raise RuntimeError(f"expected one {RESULT_PREFIX} line, found {len(matches)}")
    result = json.loads(matches[0])
    if result.get("engine") not in ENGINES:
        raise RuntimeError(f"unexpected engine in result: {result.get('engine')!r}")
    return result


def affinity(cpus: list[int], threads: int) -> str:
    return ",".join(str(cpu) for cpu in cpus[:threads])


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
    result = extract_result(completed.stdout)
    expected_engine = ENGINES[0] if executable == args.cpp else ENGINES[1]
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
    if raw_path.exists():
        raise RuntimeError(f"refusing to overwrite existing results: {raw_path}")

    metadata = {
        "host": socket.gethostname(),
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "cpp": str(args.cpp.resolve()),
        "rust": str(args.rust.resolve()),
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
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    rng = random.Random(args.schedule_seed)
    results: list[dict[str, object]] = []
    run_order = 0
    with raw_path.open("x", encoding="utf-8") as raw_output:
        for repetition in range(args.repetitions):
            cells = [(scenario, threads) for scenario in args.scenarios for threads in args.threads]
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


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
