#!/usr/bin/env python3
"""Reopen a mako-cache database written by one local engine with the other.

`mako-cache` is compiled against exactly one local transaction engine: the
C++ MassTrans engine behind the `mako_local_*` C ABI, or the native Rust STO
engine (`mako-engine-sto`). Both must produce the same durable format, so a
RocksDB directory written by one build has to recover under the other with
the same validation checksum. This driver proves that with the benchmark's
own child roles: `mako-cache-bench __sample` writes and validates a fresh
database and prints its checksum; `mako-cache-bench __recover` reopens the
directory and validates it against that checksum. Each ordered pair of
distinct engine binaries is exercised with CRC32C and unchecked records, and
one deliberately wrong checksum per pair and policy proves the recovery role
fails on a mismatch. Fixed public-API child cases also check mixed puts and
deletes, checkpoint-only empty databases, and new commits after cross-engine
recovery, including single-producer mode. The original writer then reopens
the reader's writes and checks exact values, scans, sequence, and HLC.

Only the Python standard library is used. The report is JSON.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import tempfile
from pathlib import Path
from typing import Sequence

SAMPLE_PROTOCOL = "mako-m1-sample-v3"
RECOVERY_PROTOCOL = "mako-m1-recovery-v2"
STATE_PROTOCOL = "mako-cross-engine-state-v1"
RECORD_CHECKSUM_ENV = "MAKO_CACHE_BENCH_RECORD_CHECKSUM"
WRITEBACK_CPU_ENV = "MAKO_CACHE_BENCH_WRITEBACK_CPU"
REPORT_FORMAT = "mako-cache-cross-engine-reopen-v2"
CHECKSUM_POLICIES = ("crc32c", "none")
FOREGROUND_MODES = ("concurrent", "single_producer")
RETENTION_CASES = ("retained", "checkpoint")

# (workload, transaction_size, contention, workers). Each row keeps a
# different record shape alive: one-put records, multi-op records under low
# contention, and read-modify-write records under high contention.
MATRIX: tuple[tuple[str, int, str, int], ...] = (
    ("write", 1, "low", 1),
    ("write", 4, "low", 4),
    ("rmw", 4, "high", 2),
)


class Failure(Exception):
    """A cross-engine reopen check failed or the harness could not run."""


def parse_protocol(text: str, protocol: str) -> dict[str, str]:
    lines = text.splitlines()
    if len(lines) != 1:
        raise Failure(f"child must print exactly one {protocol} line, got {len(lines)}")
    tokens = lines[0].split(" ")
    if tokens[0] != protocol:
        raise Failure(f"child output is missing protocol {protocol}: {lines[0][:120]!r}")
    fields: dict[str, str] = {}
    for token in tokens[1:]:
        key, separator, value = token.partition("=")
        if not separator or not key or not value or key in fields:
            raise Failure(f"malformed child field {token!r}")
        fields[key] = value
    return fields


def child_environment(checksum_policy: str) -> dict[str, str]:
    env = dict(os.environ)
    env.pop(WRITEBACK_CPU_ENV, None)
    env[RECORD_CHECKSUM_ENV] = checksum_policy
    return env


def run_child(binary: Path, arguments: Sequence[str], timeout: int, checksum_policy: str) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            [str(binary), *arguments],
            capture_output=True,
            text=True,
            timeout=timeout,
            env=child_environment(checksum_policy),
            stdin=subprocess.DEVNULL,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise Failure(f"{binary} {' '.join(arguments)} exceeded {timeout}s") from error
    except OSError as error:
        raise Failure(f"cannot execute {binary}: {error}") from error


def child_arguments(role: str, case: tuple[str, int, str, int], target: int, warmup: int, path: Path) -> list[str]:
    workload, transaction_size, contention, workers = case
    return [
        role,
        "mako",
        workload,
        str(transaction_size),
        contention,
        str(workers),
        str(target),
        str(warmup),
        str(path),
    ]


def sample(binary: Path, case: tuple[str, int, str, int], target: int, warmup: int, path: Path, timeout: int, checksum_policy: str) -> dict[str, str]:
    if path.exists():
        raise Failure(f"fresh sample path already exists: {path}")
    result = run_child(binary, child_arguments("__sample", case, target, warmup, path), timeout, checksum_policy)
    if result.returncode != 0:
        raise Failure(f"sample child {binary} failed ({result.returncode}):\n{result.stderr[-4000:]}")
    return parse_protocol(result.stdout, SAMPLE_PROTOCOL)


def recover(
    binary: Path,
    case: tuple[str, int, str, int],
    target: int,
    warmup: int,
    path: Path,
    checksum: int,
    timeout: int,
    checksum_policy: str,
) -> subprocess.CompletedProcess[str]:
    arguments = child_arguments("__recover", case, target, warmup, path)
    arguments.append(str(checksum))
    return run_child(binary, arguments, timeout, checksum_policy)


def require_engine(engines: dict[str, str], name: str, fields: dict[str, str]) -> str:
    engine = fields.get("engine", "")
    if not engine or not engine.isascii() or not all(c.isalnum() or c in "-_/" for c in engine):
        raise Failure(f"{name} reported a malformed engine identity")
    if engines.setdefault(name, engine) != engine:
        raise Failure(f"{name} reported two engine names")
    return engine


def state_child(
    binary: Path,
    role: str,
    path: Path,
    checksum_policy: str,
    foreground: str,
    retention: str,
    timeout: int,
    expected: dict[str, str] | None = None,
) -> dict[str, str]:
    arguments = ["__cross_engine", role, str(path), checksum_policy, foreground, retention]
    if expected is not None:
        arguments.extend((expected["sequence"], expected["timestamp"]))
    result = run_child(binary, arguments, timeout, checksum_policy)
    if result.returncode != 0:
        raise Failure(f"cross-engine {role} child {binary} failed ({result.returncode}):\n{result.stderr[-4000:]}")
    fields = parse_protocol(result.stdout, STATE_PROTOCOL)
    for key, value in (("role", role), ("checksum_policy", checksum_policy), ("foreground", foreground), ("retention", retention)):
        if fields.get(key) != value:
            raise Failure(f"cross-engine {role} child returned the wrong {key}")
    sequence = positive_integer(fields["sequence"])
    timestamp_parts = fields["timestamp"].split(",")
    if len(timestamp_parts) != 3 or any(not value.isascii() or not value.isdecimal() for value in timestamp_parts):
        raise Failure("cross-engine child returned a malformed timestamp")
    timestamp = tuple(int(value) for value in timestamp_parts)
    if timestamp[2] == 0:
        raise Failure("cross-engine timestamp has no origin")
    retained, reclaimed = int(fields["retained"]), int(fields["reclaimed"])
    if retained < 0 or reclaimed < 0 or retained + reclaimed != sequence:
        raise Failure("cross-engine retained/reclaimed record accounting disagrees")
    if (retention == "checkpoint" and retained != 0) or (retention == "retained" and reclaimed != 0):
        raise Failure("cross-engine child did not reach the required retention state")
    expected_rows = (0 if retention == "checkpoint" else 6) if role == "write" else (4 if retention == "checkpoint" else 8)
    if int(fields["rows"]) != expected_rows:
        raise Failure("cross-engine child returned the wrong exact row count")
    if expected is not None:
        previous_sequence = int(expected["sequence"])
        previous_timestamp = tuple(int(value) for value in expected["timestamp"].split(","))
        if role == "reopen" and (sequence <= previous_sequence or timestamp <= previous_timestamp):
            raise Failure("cross-engine new writes did not advance sequence and HLC")
        if role == "verify" and (sequence != previous_sequence or timestamp != previous_timestamp):
            raise Failure("cross-engine final reopen changed persisted progress")
    return fields


def positive_integer(text: str) -> int:
    try:
        value = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected a positive integer") from error
    if value <= 0:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return value


def parse_binary(argument: str) -> tuple[str, Path]:
    name, separator, path = argument.partition("=")
    if not separator or not name or not path:
        raise Failure(f"--binary expects NAME=PATH, got {argument!r}")
    binary = Path(path)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise Failure(f"benchmark binary {binary} is not executable")
    return name, binary.resolve()


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        action="append",
        default=[],
        help="NAME=PATH of a mako-cache-bench build; give at least two distinct engine builds",
    )
    parser.add_argument("--data-root", required=True, help="directory for the fresh database directories")
    parser.add_argument("--report", default=None, help="JSON report path")
    parser.add_argument("--target", type=positive_integer, default=2000, help="timed commits per worker in the writer")
    parser.add_argument("--warmup", type=positive_integer, default=200, help="warmup commits per worker in the writer")
    parser.add_argument("--timeout", type=positive_integer, default=900, help="seconds allowed per child")
    parser.add_argument("--keep-data", action="store_true", help="retain the database directories")
    args = parser.parse_args(argv)

    started = time.time()
    report: dict[str, object] = {"format": REPORT_FORMAT, "binaries": {}, "cases": [], "state_cases": [], "tripwires": []}
    run_root = None
    try:
        parsed_binaries = [parse_binary(argument) for argument in args.binary]
        binaries = dict(parsed_binaries)
        if len(binaries) != len(parsed_binaries):
            raise Failure("duplicate --binary names are not allowed")
        if len(binaries) < 2:
            raise Failure("at least two --binary builds are required")
        data_root = Path(args.data_root).resolve()
        data_root.mkdir(parents=True, exist_ok=True)
        run_root = Path(tempfile.mkdtemp(prefix="mako-cross-reopen-", dir=data_root))
        report["data_root"] = str(run_root)
        engines: dict[str, str] = {}
        index = 0
        for writer_name, writer in binaries.items():
            for reader_name, reader in binaries.items():
                if writer_name == reader_name:
                    continue
                for case_index, (checksum_policy, case) in enumerate((policy, case) for policy in CHECKSUM_POLICIES for case in MATRIX):
                    path = run_root / str(index)
                    index += 1
                    try:
                        written = sample(writer, case, args.target, args.warmup, path, args.timeout, checksum_policy)
                        checksum = int(written["checksum"])
                        writer_engine = require_engine(engines, writer_name, written)
                        result = recover(reader, case, args.target, args.warmup, path, checksum, args.timeout, checksum_policy)
                        if result.returncode != 0:
                            raise Failure(
                                f"{reader_name} could not recover a database written by {writer_name} "
                                f"for {case}:\n{result.stderr[-4000:]}"
                            )
                        recovered = parse_protocol(result.stdout, RECOVERY_PROTOCOL)
                        reader_engine = require_engine(engines, reader_name, recovered)
                        if reader_engine == writer_engine:
                            raise Failure(
                                f"{writer_name} and {reader_name} embed the same engine {writer_engine!r}"
                            )
                        if int(recovered["checksum"]) != checksum or recovered["keyspace"] != written["keyspace"]:
                            raise Failure(f"{reader_name} recovered different content than {writer_name} wrote")
                        if case_index % len(MATRIX) == 0:
                            wrong = recover(reader, case, args.target, args.warmup, path, checksum + 1, args.timeout, checksum_policy)
                            if wrong.returncode != 2 or "differs from pre-close checksum" not in wrong.stderr:
                                raise Failure(f"{reader_name} did not reject a wrong checksum through the validation check")
                            report["tripwires"].append(  # type: ignore[union-attr]
                                {"writer": writer_name, "reader": reader_name, "checksum_policy": checksum_policy, "rejected_wrong_checksum": True}
                            )
                        report["cases"].append(  # type: ignore[union-attr]
                            {
                                "writer": writer_name,
                                "writer_engine": writer_engine,
                                "reader": reader_name,
                                "reader_engine": reader_engine,
                                "checksum_policy": checksum_policy,
                                "workload": case[0],
                                "transaction_size": case[1],
                                "contention": case[2],
                                "workers": case[3],
                                "keyspace": int(written["keyspace"]),
                                "commit_records": int(written["log_keys"]),
                                "checksum": checksum,
                                "recovery_open_ns": int(recovered["open_ns"]),
                                "recovery_validation_ns": int(recovered["validation_ns"]),
                            }
                        )
                        print(
                            f"[ok] {writer_name} -> {reader_name} {case[0]} t{case[1]} {case[2]} w{case[3]} "
                            f"{checksum_policy} checksum={checksum}",
                            file=sys.stderr,
                            flush=True,
                        )
                    finally:
                        if not args.keep_data:
                            shutil.rmtree(path, ignore_errors=True)
                for checksum_policy in CHECKSUM_POLICIES:
                    for foreground in FOREGROUND_MODES:
                        for retention in RETENTION_CASES:
                            path = run_root / str(index)
                            index += 1
                            written = state_child(writer, "write", path, checksum_policy, foreground, retention, args.timeout)
                            updated = state_child(reader, "reopen", path, checksum_policy, foreground, retention, args.timeout, written)
                            verified = state_child(writer, "verify", path, checksum_policy, foreground, retention, args.timeout, updated)
                            writer_engine = require_engine(engines, writer_name, written)
                            reader_engine = require_engine(engines, reader_name, updated)
                            require_engine(engines, writer_name, verified)
                            if writer_engine == reader_engine:
                                raise Failure("cross-engine state children embed the same engine")
                            report["state_cases"].append({  # type: ignore[union-attr]
                                "writer": writer_name, "reader": reader_name,
                                "checksum_policy": checksum_policy, "foreground": foreground,
                                "retention": retention, "written": written, "updated": updated,
                                "verified": verified,
                            })
                            print(f"[ok] {writer_name} -> {reader_name} -> {writer_name} {checksum_policy} {foreground} {retention}", file=sys.stderr, flush=True)
                            if not args.keep_data:
                                shutil.rmtree(path, ignore_errors=True)
        report["binaries"] = {
            name: {"path": str(binary), "engine": engines[name]} for name, binary in binaries.items()
        }
        if len(set(engines.values())) != len(binaries):
            raise Failure(f"binaries do not embed distinct engines: {engines}")
        report["verdict"] = "pass"
    except (Failure, KeyError, ValueError, OSError, argparse.ArgumentTypeError) as error:
        report["verdict"] = "fail"
        report["error"] = str(error)
        print(f"cross-engine reopen: {error}", file=sys.stderr)
    finally:
        if run_root is not None and not args.keep_data:
            shutil.rmtree(run_root, ignore_errors=True)
    report["seconds"] = round(time.time() - started, 3)
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report:
        Path(args.report).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0 if report["verdict"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
