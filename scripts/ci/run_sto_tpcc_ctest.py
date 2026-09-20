#!/usr/bin/env python3
"""Run one TPC-C CTest process and validate its machine-readable result."""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
from collections.abc import Sequence


RESULT_PREFIX = "TPCC_BENCH_RESULT "
MIX_KEYS = ("NewOrder", "Payment", "Delivery", "OrderStatus", "StockLevel")


def positive_int(text: str) -> int:
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return value


def process_exit_code(text: str) -> int:
    value = int(text)
    if value <= 0 or value > 255:
        raise argparse.ArgumentTypeError("exit code must be between 1 and 255")
    return value


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--expected-engine")
    parser.add_argument("--expected-threads", type=positive_int)
    parser.add_argument("--expected-warehouses", type=positive_int)
    parser.add_argument("--expected-seconds", type=positive_int)
    parser.add_argument("--expected-result-count", type=positive_int)
    parser.add_argument("--expected-failure-text")
    parser.add_argument("--expected-exit-code", type=process_exit_code)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.command[:1] == ["--"]:
        args.command = args.command[1:]
    if not args.command:
        parser.error("a benchmark command is required after --")
    success_expectations = (
        args.expected_engine,
        args.expected_threads,
        args.expected_warehouses,
        args.expected_seconds,
    )
    if args.expected_failure_text is None and any(
        value is None for value in success_expectations
    ):
        parser.error(
            "successful runs require --expected-engine, --expected-threads, "
            "--expected-warehouses, and --expected-seconds"
        )
    if args.expected_failure_text is not None and any(
        value is not None for value in success_expectations
    ):
        parser.error(
            "--expected-failure-text cannot be combined with successful-run expectations"
        )
    if args.expected_failure_text is not None and args.expected_result_count is not None:
        parser.error(
            "--expected-result-count cannot be combined with failure expectations"
        )
    if (args.expected_failure_text is None) != (args.expected_exit_code is None):
        parser.error(
            "--expected-failure-text and --expected-exit-code must be specified together"
        )
    if args.expected_failure_text is None and args.expected_result_count is None:
        args.expected_result_count = 1
    return args


def require_nonnegative_integer(result: dict[str, object], field: str) -> int:
    value = result.get(field)
    if type(value) is not int or value < 0:
        raise RuntimeError(f"result field {field!r} must be a nonnegative integer")
    return value


def validate_result(result: object, args: argparse.Namespace) -> None:
    if not isinstance(result, dict):
        raise RuntimeError("TPC-C result must be a JSON object")
    if result.get("schema_version") != 1:
        raise RuntimeError(f"unsupported result schema: {result.get('schema_version')!r}")
    if result.get("engine") != args.expected_engine:
        raise RuntimeError(
            f"unexpected engine {result.get('engine')!r}; expected {args.expected_engine!r}"
        )

    threads = require_nonnegative_integer(result, "threads")
    warehouses = require_nonnegative_integer(result, "warehouses")
    configured_seconds = require_nonnegative_integer(result, "configured_seconds")
    commits = require_nonnegative_integer(result, "commits")
    aborts = require_nonnegative_integer(result, "aborts")
    attempts = require_nonnegative_integer(result, "attempts")
    if threads != args.expected_threads:
        raise RuntimeError(f"result reports {threads} threads; expected {args.expected_threads}")
    if warehouses != args.expected_warehouses:
        raise RuntimeError(
            f"result reports {warehouses} warehouses; expected {args.expected_warehouses}"
        )
    if configured_seconds != args.expected_seconds:
        raise RuntimeError(
            f"result reports {configured_seconds} configured seconds; "
            f"expected {args.expected_seconds}"
        )
    if commits <= 0:
        raise RuntimeError("TPC-C gate completed without a committed transaction")
    if attempts != commits + aborts:
        raise RuntimeError("result violates attempts == commits + aborts")

    measured = result.get("measured_seconds")
    throughput = result.get("throughput_txn_s")
    if (
        not isinstance(measured, (int, float))
        or isinstance(measured, bool)
        or not math.isfinite(measured)
        or measured <= 0
    ):
        raise RuntimeError("measured_seconds must be finite and positive")
    if (
        not isinstance(throughput, (int, float))
        or isinstance(throughput, bool)
        or not math.isfinite(throughput)
        or throughput <= 0
    ):
        raise RuntimeError("throughput_txn_s must be finite and positive")

    mix = result.get("mix")
    if not isinstance(mix, dict) or set(mix) != set(MIX_KEYS):
        raise RuntimeError(f"mix must contain exactly {', '.join(MIX_KEYS)}")
    if any(type(mix[key]) is not int or mix[key] < 0 for key in MIX_KEYS):
        raise RuntimeError("mix counters must be nonnegative integers")
    if sum(mix.values()) != commits:
        raise RuntimeError("result violates sum(mix counters) == commits")


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    records: list[str] = []
    found_expected_failure = False
    process = subprocess.Popen(
        args.command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    assert process.stdout is not None
    for line in process.stdout:
        print(line, end="", flush=True)
        if (
            args.expected_failure_text is not None
            and args.expected_failure_text in line
        ):
            found_expected_failure = True
        if line.startswith(RESULT_PREFIX):
            records.append(line[len(RESULT_PREFIX) :].strip())
    return_code = process.wait()
    if args.expected_failure_text is not None:
        if return_code != args.expected_exit_code:
            raise RuntimeError(
                f"rejected benchmark exited with status {return_code}; "
                f"expected {args.expected_exit_code}"
            )
        if records:
            raise RuntimeError(
                "rejected benchmark emitted a TPC-C result before exiting"
            )
        if not found_expected_failure:
            raise RuntimeError(
                f"benchmark output lacks expected failure text "
                f"{args.expected_failure_text!r}"
            )
        print(
            "validated TPC-C argument rejection: "
            f"status={return_code} text={args.expected_failure_text!r}"
        )
        return 0
    if return_code != 0:
        raise RuntimeError(f"benchmark exited with status {return_code}")
    if len(records) != args.expected_result_count:
        raise RuntimeError(
            f"expected {args.expected_result_count} {RESULT_PREFIX!r} record(s), "
            f"found {len(records)}"
        )
    results: list[dict[str, object]] = []
    for index, record in enumerate(records, start=1):
        try:
            result = json.loads(record)
        except json.JSONDecodeError as error:
            raise RuntimeError(
                f"invalid TPC-C result JSON in record {index}: {error}"
            ) from error
        validate_result(result, args)
        results.append(result)
    print(
        "validated TPC-C result(s): "
        f"count={len(results)} engine={args.expected_engine} "
        f"threads={args.expected_threads} "
        f"commits={sum(result['commits'] for result in results)} "
        f"aborts={sum(result['aborts'] for result in results)}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"TPC-C CTest validation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
