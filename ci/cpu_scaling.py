#!/usr/bin/env python3
"""Measure worker duty-cycle scaling using completed native benchmark runs."""
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
previous = None
for percent in [1, 2, 4, 8]:
    command = [sys.executable, str(root / "examples/run_native.py"), "tpcc",
               "--build", os.environ.get("BUILD_DIR", "build"), "--shards", "2", "--colocated",
               "--workers", "3", "--duration-only", "--seconds", os.environ.get("MAKO_RUNTIME_SECONDS", "30"),
               "--cpu-limit", str(percent), "--keep"]
    result = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE, check=True)
    print(result.stdout, end="", flush=True)
    stats = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    if len(stats) != 2 or {s["owner"] for s in stats} != {0, 1}:
        raise RuntimeError("missing per-shard native benchmark results")
    throughput = sum(sum(s["committed"]) / s["elapsed_seconds"] for s in stats)
    if throughput <= 0:
        raise RuntimeError("no committed throughput")
    ratio = throughput / previous if previous else None
    print(json.dumps(dict(cpu_percent=percent, throughput=throughput, ratio=ratio)), flush=True)
    if ratio is not None and not 1.3 <= ratio <= 3.0:
        raise RuntimeError(f"CPU scaling outside 1.3..3.0 at {percent}%: {ratio}")
    previous = throughput
