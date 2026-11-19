#!/usr/bin/env python3
"""Compare observable behavior from native Rust and generated C++ runtimes."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

PREFIX = "SRPC_RUNTIME_PARITY "
EXPECTED = {
    "version": 1,
    "timer_order": ["start", "peer", "resume"],
    "timer_suspended": True,
    "deadline_respected": True,
    "pending_before_wake": True,
    "foreign_thread": True,
    "completion_on_owner": True,
    "wake_value": 7,
    "rpc_reply": 42,
    "rpc_success_error": 0,
    "rpc_missing_error": 2,
}


def read_transcript(output: str, lane: str) -> dict:
    records = [line[len(PREFIX):] for line in output.splitlines() if line.startswith(PREFIX)]
    if len(records) != 1:
        raise ValueError(f"{lane}: expected exactly one runtime transcript, found {len(records)}")
    try:
        transcript = json.loads(records[0])
    except json.JSONDecodeError as error:
        raise ValueError(f"{lane}: malformed runtime transcript: {error}") from error
    # Strict JSON types distinguish true from 1 and reject missing or extra fields.
    canonical = lambda value: json.dumps(value, sort_keys=True, separators=(",", ":"))
    if canonical(transcript) != canonical(EXPECTED):
        raise ValueError(f"{lane}: runtime behavior differs from the contract: {transcript!r}")
    return transcript


def run_lane(command: list[str], lane: str, repo: Path, timeout: float) -> dict:
    try:
        process = subprocess.run(command, cwd=repo, text=True, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise ValueError(f"{lane}: runtime fixture timed out after {timeout}s") from error
    if process.returncode:
        raise ValueError(f"{lane}: fixture exited {process.returncode}\n"
                         f"{process.stdout[-4000:]}\n{process.stderr[-4000:]}")
    return read_transcript(process.stdout, lane)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--cpp-exe", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    try:
        rust = run_lane([args.cargo, "test", "--locked", "--quiet", "--test", "runtime_parity_rust",
                         "--", "--exact", "runtime_parity_transcript", "--nocapture"],
                        "Rust", args.repo.resolve(), args.timeout)
        cpp = run_lane([str(args.cpp_exe.resolve())], "C++", args.repo.resolve(), args.timeout)
        if rust != cpp:
            raise ValueError(f"runtime transcripts differ: Rust={rust!r}, C++={cpp!r}")
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    print("Rust and generated C++ agree on timer suspension/order, owner wake, and TCP RPC results")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
