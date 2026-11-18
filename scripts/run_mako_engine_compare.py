#!/usr/bin/env python3
"""Run paired fresh-process engine and ACK diagnostics on zoo-002.

The archive fingerprint and source snapshot belong beside these raw results.
This launcher records exact executable hashes and keeps every attempted cell.
It does not measure sustained RocksDB throughput or reopen time.

CPU evidence uses the shared execution-plus-steal policy: I/O wait and true
idle are distinct diagnostics; inclusive sibling ceilings use integer ticks.
"""

import argparse
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

import sto_cpu_accounting as accounting


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def cpu_list(text):
    cpus = set()
    for part in text.split(","):
        bounds = part.split("-")
        if len(bounds) == 1:
            first = last = int(bounds[0])
        elif len(bounds) == 2:
            first, last = map(int, bounds)
        else:
            raise ValueError("expected comma-separated CPU numbers/ranges")
        if first < 0 or last < first:
            raise ValueError("invalid CPU range")
        cpus.update(range(first, last + 1))
    return cpus


def parse_worker_cpus(text):
    parts = text.split(",")
    if any(not part or not part.isascii() or not part.isdecimal() for part in parts):
        raise argparse.ArgumentTypeError("expected comma-separated unsigned CPU IDs")
    cpus = [int(part) for part in parts]
    if any(cpu >= 1024 for cpu in cpus):
        raise argparse.ArgumentTypeError("worker CPU IDs must fit Linux cpu_set_t (0..1023)")
    if len(set(cpus)) != len(cpus):
        raise argparse.ArgumentTypeError("worker CPU IDs must be unique")
    return cpus


def selected_worker_cpus(args, workers):
    cpus = getattr(args, "worker_cpus", None)
    if cpus is not None:
        if len(cpus) < workers:
            raise ValueError("--worker-cpus needs at least one CPU per requested worker")
        return cpus[:workers]
    return list(range(args.worker_cpu_start, args.worker_cpu_start + workers))


def machine():
    root = Path("/sys/devices/system/cpu")
    return {
        "hostname": socket.gethostname(),
        "boost": (root / "cpufreq/boost").read_text().strip(),
        "governors": {
            str(cpu): (root / f"cpu{cpu}/cpufreq/scaling_governor").read_text().strip()
            for cpu in sorted(cpu_list((root / "online").read_text().strip()))
        },
        "load_average": Path("/proc/loadavg").read_text().strip(),
        "processes": subprocess.check_output(
            ["ps", "-eo", "user,pid,pcpu,comm", "--sort=-pcpu"], text=True
        ).splitlines()[:25],
    }


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n")


def add_measurement_arguments(parser):
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", type=int, nargs="+", default=[1, 4])
    parser.add_argument("--modes", nargs="+", default=["engine", "engine-logged", "cache-ack"],
                        choices=["engine", "engine-logged", "cache-ack"])
    parser.add_argument("--iterations", type=int, default=262144)
    parser.add_argument("--warmup", type=int, default=65536)
    parser.add_argument("--repetitions", type=int, default=3,
                        help="required clean comparison blocks per cell")
    parser.add_argument("--perf", type=int, choices=[0, 1], default=1)
    parser.add_argument("--items", type=int, default=1,
                        help="items per transaction; above one requires --modes engine")
    placement = parser.add_mutually_exclusive_group()
    # A distinct unset value keeps explicit zero mutually exclusive on Python 3.12.
    placement.add_argument("--worker-cpu-start", type=int)
    placement.add_argument("--worker-cpus", type=parse_worker_cpus,
                           help="ordered CPU pool; each cell uses its first workers entries")
    parser.add_argument("--writeback-cpu", type=int,
                        help="default 32; default 0 when worker CPU start is 32")
    parser.add_argument("--helper-affinity",
                        help="taskset CPU list; default 33-63, or 1-31 for worker CPU start 32")
    parser.add_argument("--creation-delay-ms", type=int, default=0)
    parser.add_argument("--pmu-min-running-ratio", type=float, default=0.995,
                        help="campaign must explicitly select 0.98 to use that threshold")
    parser.add_argument("--max-worker-off-cpu-fraction", type=float,
                        help="optional per-worker diagnostic-envelope off-CPU ceiling (e.g. .01)")
    parser.add_argument("--max-sibling-contention", type=float,
                        help="inclusive SMT-sibling whole-process execution+steal ceiling (e.g. .01)")
    parser.add_argument("--balanced-blocks", action="store_true",
                        help="run each order and its reverse in one indivisible block")
    parser.add_argument("--max-block-attempts", type=int,
                        help="bounded clean-block retry budget per cell; default repetitions, maximum 60")


def validate_arguments(parser, args):
    if socket.gethostname() != "zoo-002":
        parser.error("this CPU-placement protocol is specific to zoo-002")
    args.output = args.output.resolve(strict=True)
    if not args.output.is_dir() or any(args.output.iterdir()):
        parser.error("output must be an existing empty directory")
    if args.worker_cpu_start is None:
        args.worker_cpu_start = 0
    if (any(w < 1 or w > 32 for w in args.workers) or args.iterations < 1
            or args.warmup < 256 or args.repetitions < 1 or not 1 <= args.items <= 256
            or args.worker_cpu_start < 0 or args.creation_delay_ms < 0
            or (args.writeback_cpu is not None and args.writeback_cpu < 0)):
        parser.error("workers 1..32, positive iterations/repetitions, warmup >=256, items 1..256, nonnegative CPU start/delay required")
    if args.worker_cpus is not None and len(args.worker_cpus) < max(args.workers):
        parser.error("--worker-cpus needs at least max(--workers) CPU IDs")
    if args.items != 1 and args.modes != ["engine"]:
        parser.error("multi-item transactions require --modes engine")
    total = args.iterations + args.warmup
    if (total * max(args.items, max(args.workers)) > 2**64 - 1
            or total > 2**63 or ("engine-logged" in args.modes and total >= 2**48)):
        parser.error("iteration/item/count or logged sequence overflow")
    args.max_block_attempts = (args.repetitions if args.max_block_attempts is None
                               else args.max_block_attempts)
    if not args.repetitions <= args.max_block_attempts <= 60:
        parser.error("need repetitions <= max-block-attempts <= 60")
    for name in ("pmu_min_running_ratio", "max_worker_off_cpu_fraction", "max_sibling_contention"):
        value = getattr(args, name)
        if value is not None and (not math.isfinite(value) or not 0 <= value <= 1):
            parser.error(f"{name} must be finite and in [0,1]")
    if args.writeback_cpu is None:
        args.writeback_cpu = 0 if args.worker_cpus is None and args.worker_cpu_start == 32 else 32
    if args.helper_affinity is None:
        args.helper_affinity = "1-31" if args.worker_cpus is None and args.worker_cpu_start == 32 else "33-63"
    try:
        helpers = cpu_list(args.helper_affinity)
        online = cpu_list(Path("/sys/devices/system/cpu/online").read_text().strip())
        allowed = os.sched_getaffinity(0) & online
        workers = set(selected_worker_cpus(args, max(args.workers)))
        worker_pool = set(args.worker_cpus) if args.worker_cpus is not None else workers
        required = worker_pool | helpers | ({args.writeback_cpu} if "cache-ack" in args.modes else set())
        if not required <= allowed:
            parser.error(f"requested CPUs unavailable: {sorted(required - allowed)}")
        if worker_pool & helpers or ("cache-ack" in args.modes and args.writeback_cpu in worker_pool | helpers):
            parser.error("worker, helper and writeback CPU sets must be disjoint")
        siblings = {}
        for cpu in sorted(workers):
            path = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")
            siblings[str(cpu)] = sorted((cpu_list(path.read_text().strip()) - {cpu}) & online)
        if args.max_sibling_contention is not None:
            sibling_cpus = {cpu for values in siblings.values() for cpu in values}
            if sibling_cpus & required:
                parser.error("SMT-sibling quality gate conflicts with requested workload/helper CPUs")
        return siblings
    except (OSError, ValueError) as error:
        parser.error(f"cannot establish CPU topology/placement: {error}")


def finite_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def json_number(text):
    value = float(text)
    if not math.isfinite(value):
        raise ValueError("non-finite JSON number")
    return value


def validate_measurement(measurement, args, mode, workers, expected_engine):
    expected = {
        "engine": expected_engine, "mode": mode, "workers": workers,
        "iterations_per_worker": args.iterations, "warmup_per_worker": args.warmup,
        "commits": args.iterations * workers, "verified_keys": workers * 256,
        "acknowledged": (args.iterations + args.warmup) * workers,
        "keys_per_worker": 256, "value_bytes": 128, "checksum": "none",
        "perf_enabled": bool(args.perf), "items_per_transaction": args.items,
        "worker_cpu_start": None if args.worker_cpus is not None else args.worker_cpu_start,
        "worker_cpus": selected_worker_cpus(args, workers), "writeback_cpu": args.writeback_cpu,
        "creation_delay_ms": args.creation_delay_ms, "verified": True, "drained": False,
        "queue_capacity_per_worker": 1 << (args.iterations + args.warmup - 1).bit_length(),
    }
    errors = [f"{key}: expected {value!r}, got {measurement.get(key)!r}"
              for key, value in expected.items() if key not in measurement or measurement[key] != value]
    cpus = measurement.get("worker_cpus")
    if not isinstance(cpus, list) or any(type(cpu) is not int for cpu in cpus):
        errors.append("invalid worker CPU mapping")
    for key in ("max_worker_nanoseconds", "transactions_per_second"):
        if not finite_number(measurement.get(key)) or measurement[key] <= 0:
            errors.append(f"invalid {key}")
    if args.perf and (not finite_number(measurement.get("pmu_min_running_ratio"))
                      or not 0 <= measurement["pmu_min_running_ratio"] <= 1):
        errors.append("invalid PMU running ratio")
    return errors


def sample_quality(args, measurement, worker_stats, siblings, before, after):
    reasons = []
    pmu_valid = not args.perf or measurement["pmu_min_running_ratio"] >= args.pmu_min_running_ratio
    if not pmu_valid:
        reasons.append("PMU running ratio below configured minimum")
    off_cpu = {str(row["worker"]): max(0.0, 1 - row["cpu_ns"] / row["diagnostic_wall_ns"])
               for row in worker_stats}
    sibling_contention = {}
    sibling_metrics = {}
    accounting_errors = {}
    for cpu in sorted({cpu for values in siblings.values() for cpu in values}):
        key = str(cpu)
        try:
            metrics = accounting.interval(before, after, cpu)
        except RuntimeError as error:
            metrics = None
            accounting_errors[key] = str(error)
        sibling_metrics[key] = metrics
        sibling_contention[key] = metrics["contention_fraction"] if metrics is not None else None
        if metrics is None or (args.max_sibling_contention is not None
                               and not accounting.within_limit(metrics, args.max_sibling_contention)):
            reasons.append(f"sibling CPU {cpu} contention missing or above ceiling")
    if args.max_worker_off_cpu_fraction is not None:
        for worker, value in off_cpu.items():
            if value > args.max_worker_off_cpu_fraction:
                reasons.append(f"worker {worker} off-CPU fraction above ceiling")
    return {"pmu_valid": pmu_valid, "worker_off_cpu_fraction": off_cpu,
            "sibling_cpu_contention_fraction": sibling_contention,
            "cpu_accounting_policy": accounting.POLICY,
            "sibling_cpu_accounting": sibling_metrics,
            "cpu_accounting_errors": accounting_errors,
            "quality_valid": not reasons, "quality_rejections": reasons}


def run_sample(args, binaries, hashes, expected_engines, baseline, topology,
               label_key, name, mode, workers, attempt, position, entries):
    directory = Path(tempfile.mkdtemp(
        prefix=f"r{attempt}-{mode}-w{workers}-p{position}-{name}.", dir=args.output))
    database = directory / "rocks"
    worker_cpus = selected_worker_cpus(args, workers)
    active_siblings = {str(cpu): topology[str(cpu)] for cpu in worker_cpus}
    accounting_cpus = sorted(set(worker_cpus) |
                             {cpu for values in active_siblings.values() for cpu in values})
    command = ["taskset", "-c", args.helper_affinity, str(binaries[name]), mode,
               "--workers", str(workers), "--iterations", str(args.iterations),
               "--warmup", str(args.warmup), "--perf", str(args.perf),
               "--items", str(args.items),
               "--writeback-cpu", str(args.writeback_cpu),
               "--creation-delay-ms", str(args.creation_delay_ms)]
    command += (["--worker-cpus", ",".join(map(str, worker_cpus))]
                if args.worker_cpus is not None else
                ["--worker-cpu-start", str(args.worker_cpu_start)])
    if mode == "cache-ack":
        command += ["--path", str(database)]
    entry = {label_key: name, "mode": mode, "workers": workers, "items": args.items,
             "repetition": attempt, "block_attempt": attempt, "position": position,
             "directory": str(directory), "command": command, "completed": False,
             "binary_sha256": hashes[name], "expected_engine": expected_engines[name],
             "block_accepted": False, "quality_valid": False}
    entry["cpu_accounting_policy"] = accounting.POLICY
    # Register before execution so interrupted samples remain in their partial block.
    entries.append(entry)
    write_json(directory / "result.json", entry)
    try:
        before = machine()
        write_json(directory / "before.json", before)
        if before["boost"] != "0" or before["governors"] != baseline["governors"]:
            raise RuntimeError("boost/governor changed before sample")
        if digest(binaries[name]) != hashes[name]:
            raise RuntimeError("executable hash changed since protocol capture")
        started = time.monotonic()
        ticks_before = accounting.snapshot(accounting_cpus)
        write_json(directory / "cpu-before.json", ticks_before)
        try:
            with (directory / "stdout.txt").open("w") as stdout, (directory / "stderr.txt").open("w") as stderr:
                process = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=600)
            entry["returncode"] = process.returncode
        finally:
            ticks_after = accounting.snapshot(accounting_cpus)
            write_json(directory / "cpu-after.json", ticks_after)
            entry["elapsed_seconds"] = time.monotonic() - started
        after = machine()
        write_json(directory / "after.json", after)
        if process.returncode:
            raise RuntimeError(f"workload exited {process.returncode}")
        records = [json.loads(line, parse_constant=json_number, parse_float=json_number)
                   for line in (directory / "stdout.txt").read_text().splitlines()
                   if line.startswith("{")]
        if len(records) != 1 or not isinstance(records[0], dict):
            raise RuntimeError("expected exactly one JSON result object")
        measurement = records[0]
        entry["measurement"] = measurement
        errors = validate_measurement(measurement, args, mode, workers, expected_engines[name])
        if after["boost"] != "0" or after["governors"] != baseline["governors"]:
            errors.append("boost/governor changed during sample")
        if digest(binaries[name]) != hashes[name]:
            errors.append("executable hash changed during sample")
        entry["environment_and_counts_valid"] = not errors
        if errors:
            raise RuntimeError("; ".join(errors))
        stats = [json.loads(line[7:], parse_constant=json_number, parse_float=json_number)
                 for line in (directory / "stderr.txt").read_text().splitlines()
                 if line.startswith("WORKER ")]
        entry["worker_stats"] = stats
        if (len(stats) != workers
                or any(not isinstance(row, dict) or type(row.get("worker")) is not int
                       or type(row.get("cpu")) is not int for row in stats)
                or {row["worker"] for row in stats} != set(range(workers))
                or any(row["cpu"] != worker_cpus[row["worker"]]
                       or any(not finite_number(row.get(key)) or row[key] <= 0
                              for key in ("wall_ns", "cpu_ns", "diagnostic_wall_ns"))
                       or row["diagnostic_wall_ns"] < row["wall_ns"] for row in stats)):
            raise RuntimeError("invalid or missing per-worker timing/affinity diagnostics")
        entry.update(sample_quality(args, measurement, stats, active_siblings, ticks_before, ticks_after))
        entry["completed"] = True
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError) as error:
        entry["fatal_error"] = str(error)
    finally:
        write_json(directory / "result.json", entry)
    return entry


def run_comparison(parser, args, binaries, expected_engines, label_key):
    topology = validate_arguments(parser, args)
    with open("/var/tmp/mako-zoo2-perf-shuai.lock", "r") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        baseline = machine()
        if baseline["boost"] != "0":
            parser.error("CPU boost must already be disabled")
        for path in binaries.values():
            if not path.is_file() or not os.access(path, os.X_OK):
                parser.error(f"binary must be an executable file: {path}")
        hashes = {name: digest(path) for name, path in binaries.items()}
        write_json(args.output / "protocol.json", {
            "binary_sha256": hashes, "binary_paths": {name: str(path) for name, path in binaries.items()},
            "expected_engines": expected_engines, "source_policy": "common benchmark source; explicit selected engines",
            "workers": args.workers, "modes": args.modes, "items_per_transaction": args.items,
            "iterations_per_worker": args.iterations, "warmup_per_worker": args.warmup,
            "perf": args.perf, "repetitions": args.repetitions, "max_block_attempts": args.max_block_attempts,
            "balanced_blocks": args.balanced_blocks, "complete_block_rejection": True,
            "helper_affinity": args.helper_affinity,
            "worker_cpu_start": None if args.worker_cpus is not None else args.worker_cpu_start,
            "worker_cpus": args.worker_cpus,
            "worker_affinity": ("ordered worker_cpus prefix" if args.worker_cpus is not None else
                                "worker_cpu_start..worker_cpu_start+workers-1"),
            "writeback_cpu": args.writeback_cpu,
            "creation_delay_ms": args.creation_delay_ms, "thread_siblings": topology,
            "pmu_min_running_ratio": args.pmu_min_running_ratio,
            "max_worker_off_cpu_fraction": args.max_worker_off_cpu_fraction,
            "max_sibling_contention": args.max_sibling_contention,
            "cpu_accounting_policy": accounting.POLICY,
            "cpu_accounting_schema": accounting.SCHEMA,
            "off_cpu_scope": "per-worker wall/CPU diagnostic envelope around timed work and PMU controls",
            "sibling_contention_scope": "whole fresh subprocess, including warmup and final verification; execution+steal / first-eight /proc/stat ticks; inclusive rational ceiling",
            "LD_LIBRARY_PATH": os.environ.get("LD_LIBRARY_PATH"), "LD_PRELOAD": os.environ.get("LD_PRELOAD"),
            "initial_machine": baseline, "performance_acceptance": False,
            "scope": "foreground diagnostics; unapplied ACK tail discarded after value verification",
        })
        results, blocks = [], []
        clean = {(mode, workers): 0 for mode in args.modes for workers in args.workers}
        try:
            with (args.output / "blocks.jsonl").open("a") as journal:
                for attempt in range(1, args.max_block_attempts + 1):
                    for mode, workers in clean:
                        if clean[mode, workers] >= args.repetitions:
                            continue
                        order = list(binaries)
                        if attempt % 2 == 0:
                            order.reverse()
                        if args.balanced_blocks:
                            order += list(reversed(order))
                        block = {"mode": mode, "workers": workers, "items": args.items,
                                 "attempt": attempt, "order": order, "accepted": False, "samples": []}
                        entries = []
                        complete = False
                        try:
                            for position, name in enumerate(order):
                                entry = run_sample(args, binaries, hashes, expected_engines, baseline,
                                                   topology, label_key, name, mode, workers, attempt,
                                                   position, entries)
                                if "fatal_error" in entry:
                                    break  # Never retry correctness, configuration or workload failures.
                            complete = True
                        finally:
                            results.extend(entries)
                            block["samples"] = [row["directory"] for row in entries]
                            block["accepted"] = (complete and len(entries) == len(order)
                                                 and all(row["quality_valid"] for row in entries))
                            block["rejections"] = [{"directory": row["directory"],
                                                     "reasons": row.get("quality_rejections", []),
                                                     "fatal_error": row.get("fatal_error")}
                                                    for row in entries if not row["quality_valid"]]
                            if block["accepted"]:
                                clean[mode, workers] += 1
                            block["clean_blocks"] = clean[mode, workers]
                            blocks.append(block)
                            journal.write(json.dumps(block, sort_keys=True, allow_nan=False) + "\n")
                            journal.flush()
                            for entry in entries:
                                entry["block_accepted"] = block["accepted"]
                                write_json(Path(entry["directory"]) / "result.json", entry)
                                if complete:
                                    print(json.dumps(entry, allow_nan=False), flush=True)
                                database = Path(entry["directory"]) / "rocks"
                                if block["accepted"] and database.is_dir():
                                    # Only remove a complete clean block's verified scratch databases.
                                    shutil.rmtree(database)
                        if any("fatal_error" in row for row in entries):
                            raise RuntimeError(f"fatal sample failure; complete block rejected: {block}")
            if any(count < args.repetitions for count in clean.values()):
                raise RuntimeError(f"inconclusive: clean block budget exhausted: {clean}")
        finally:
            write_json(args.output / "results.json", results)
            write_json(args.output / "blocks.json", blocks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--c-abi", type=Path, required=True)
    parser.add_argument("--sto", type=Path, required=True)
    add_measurement_arguments(parser)
    args = parser.parse_args()
    binaries = {"c-abi": args.c_abi.resolve(strict=True), "sto": args.sto.resolve(strict=True)}
    run_comparison(parser, args, binaries, {"c-abi": "mako-local/sto-masstrans", "sto": "sto"}, "arm")


if __name__ == "__main__":
    main()
