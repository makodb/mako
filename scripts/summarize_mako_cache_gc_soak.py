#!/usr/bin/env python3
"""Read preserved soak reports and emit JSON without modifying their files."""

import argparse
import json
from pathlib import Path
import re


GC_COUNTERS = (
    "retained_records", "retained_bytes", "reclaimed_records",
    "reclaimed_bytes", "errors", "expired_backlog_lower_bound",
)


def events(path):
    if not path.exists():
        return []
    result = []
    lines = path.read_text().splitlines(keepends=True)
    for index, line in enumerate(lines):
        # Native engine diagnostics can share stdout with the JSON events.
        if not line.startswith('{"event":'):
            continue
        try:
            result.append(json.loads(line))
        except json.JSONDecodeError:
            # The workload can be appending its final line while this reader
            # takes a live snapshot. Never hide malformed complete records.
            if index == len(lines) - 1 and not line.endswith("\n"):
                continue
            raise
    return result


def gc_counters(status):
    # Debug status is intentionally used by a single shared benchmark source
    # that also compiles against the pre-GC crate with no .log_gc field.
    if "log_gc: LogGcStatus {" not in status:
        return None
    part = status.split("log_gc: LogGcStatus {", 1)[1]
    found = {}
    for name in GC_COUNTERS:
        match = re.search(r"\b" + name + r": (\d+)", part)
        found[name] = int(match[1]) if match else None
    duration = re.search(r"\btotal_duration: ([\d.]+)(ns|µs|ms|s)", part)
    found["total_duration_seconds"] = (
        float(duration[1]) * {"ns": 1e-9, "µs": 1e-6, "ms": 1e-3, "s": 1}[duration[2]]
        if duration else None
    )
    cutoff = re.search(r"\bobserved_cutoff_us: Some\((\d+)\)", part)
    found["observed_cutoff_us"] = int(cutoff[1]) if cutoff else None
    return found


def summarize(directory, approved_binaries=(), exclusions=(), expected_seconds=960):
    run = events(directory / "run.jsonl")
    verification = events(directory / "verify.jsonl")
    protocol = next((item for item in run if item["event"] == "protocol"), None)
    drained = next((item for item in run if item["event"] == "drained"), None)
    verified = next((item for item in verification if item["event"] == "verified"), None)
    samples = [item for item in run if item["event"] == "sample"]
    machine = directory / "machine.txt"
    identity = re.search(r"^([0-9a-f]{64})\s+(.+)$", machine.read_text(), re.MULTILINE) if machine.exists() else None
    completion_path = directory / "completed.json"
    completion = json.loads(completion_path.read_text()) if completion_path.exists() else None
    launcher_succeeded = completion is not None and all(
        completion.get(key) == value for key, value in {
            "completed": True, "run_exit": 0, "verify_exit": 0,
            "final_boost": 0, "scratch_cleaned": True,
        }.items()
    )
    result = {
        "directory": str(directory),
        "binary_sha256": identity[1] if identity else None,
        "binary_path": identity[2] if identity else None,
        "drained_and_verified": drained is not None and verified is not None,
        "launcher_succeeded": launcher_succeeded,
        "completion_evidence": completion,
        "protocol": protocol,
        "drained": drained,
        "verified": verified,
        "sample_count": len(samples),
        "samples": samples,
        "measurement_notes": [
            "p99 is a randomized-sample histogram upper bound, about 6.25% precision",
            "queue occupancy is sampled every 10s; individual record queue age is unavailable",
            "mako-writeback CPU includes replay and GC",
            "GC total_duration is completed-attempt wall time, including backend I/O wait",
            "process write bytes include WAL and compaction",
            "retained bytes are logical bytes; physical bytes follow RocksDB compaction",
        ],
    }
    reasons = [reason for prefix, reason in exclusions if directory.name.startswith(prefix)]
    if not result["drained_and_verified"]:
        reasons.append("incomplete drain or verification")
    if not launcher_succeeded:
        reasons.append("missing successful launcher completion marker")
    if result["binary_sha256"] not in approved_binaries:
        reasons.append("binary not explicitly approved for comparison")
    expected_protocol = {
        "version": 1, "seconds": expected_seconds, "keys_per_worker": 256,
        "value_bytes": 128, "rate_per_worker": 0,
        "queue_capacity_per_worker": 1024, "max_batch_records": 64,
        "writeback_cpu": 32, "checksum": "crc32c", "wal": True, "sync": False,
    }
    if not protocol or any(protocol.get(key) != value for key, value in expected_protocol.items()):
        reasons.append("protocol does not match the requested sustained comparison")
    if protocol and protocol.get("workers") not in (1, 4, 8, 16, 32):
        reasons.append("worker count is outside the comparison sweep")
    if drained and drained["ack_seconds"] < expected_seconds:
        reasons.append("foreground duration shorter than requested")
    result["comparative_accepted"] = not reasons
    result["exclusion_reasons"] = reasons
    if drained:
        result["final_log_gc"] = gc_counters(drained["status"])
        result["physical_write_bytes_per_commit"] = drained["process_write_bytes"] / max(1, drained["commits"])
    if samples:
        result["last_sampled_thread_cpu_ticks"] = samples[-1]["thread_cpu_ticks"]
        result["last_cpu_sample_elapsed_seconds"] = samples[-1]["elapsed_seconds"]
    # Retention default is five minutes. This reports measured throughput and
    # physical/logical growth after the first two complete retention windows.
    steady = [item for item in samples if item["elapsed_seconds"] >= 600]
    if len(steady) > 1:
        first, last = steady[0], steady[-1]
        duration = last["elapsed_seconds"] - first["elapsed_seconds"]
        result["after_600s"] = {
            "seconds": duration,
            "ack_tps": (last["acknowledged"] - first["acknowledged"]) / duration,
            "applied_tps": (last["applied"] - first["applied"]) / duration,
            "disk_growth_bytes_per_second": (last["disk_bytes"] - first["disk_bytes"]) / duration,
            "minimum_disk_bytes": min(item["disk_bytes"] for item in steady),
            "maximum_disk_bytes": max(item["disk_bytes"] for item in steady),
            "first_log_gc": gc_counters(first["status"]),
            "last_log_gc": gc_counters(last["status"]),
        }
        history = [gc_counters(item["status"]) for item in steady]
        history = [item for item in history if item is not None]
        if history:
            for counter in ("retained_records", "retained_bytes", "expired_backlog_lower_bound"):
                values = [item[counter] for item in history if item[counter] is not None]
                if values:
                    result["after_600s"]["minimum_" + counter] = min(values)
                    result["after_600s"]["maximum_" + counter] = max(values)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_root", type=Path)
    parser.add_argument("--approve-binary", action="append", default=[], metavar="SHA256",
                        help="allow this exact binary into comparative results (repeatable)")
    parser.add_argument("--exclude", action="append", default=[], metavar="PREFIX=REASON",
                        help="exclude matching directory prefixes from comparison (repeatable)")
    parser.add_argument("--seconds", type=int, default=960,
                        help="required sustained foreground duration (default: 960)")
    args = parser.parse_args()
    if any(not re.fullmatch(r"[0-9a-f]{64}", digest) for digest in args.approve_binary):
        parser.error("--approve-binary requires a lowercase SHA-256 digest")
    if any("=" not in rule or not all(rule.split("=", 1)) for rule in args.exclude):
        parser.error("--exclude requires PREFIX=REASON")
    exclusions = [rule.split("=", 1) for rule in args.exclude]
    reports = sorted(path.parent for path in args.output_root.glob("*/run.jsonl"))
    print(json.dumps({
        "schema": "mako-cache-gc-soak-v2",
        "comparison_policy": {
            "approved_binary_sha256": args.approve_binary,
            "expected_seconds": args.seconds,
            "directory_exclusions": dict(exclusions),
        },
        "runs": [summarize(path, args.approve_binary, exclusions, args.seconds) for path in reports],
    }, indent=2))


if __name__ == "__main__":
    main()
