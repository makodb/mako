#!/usr/bin/env python3
"""Measure Mako Redis request-worker CPU under persistent GET load."""

from __future__ import annotations

import csv
import os
import re
import shutil
import signal
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
HOST = os.environ.get("MAKO_HOST", "127.0.0.1")
PORT = int(os.environ.get("MAKO_PORT", "6380"))
SAMPLE_SECONDS = int(os.environ.get("MAKO_CPU_SAMPLE_SECONDS", "5"))
CLIENTS = tuple(int(value) for value in os.environ.get("MAKO_CPU_CLIENTS", "1 2 4 8 16 32").split())
OUT_FILE = Path(
    os.environ.get(
        "MAKO_CPU_OUT",
        ROOT / "third-party/redis/compat/worker_cpu_results.csv",
    )
)


def require_tool(name: str) -> None:
    if shutil.which(name) is None:
        raise SystemExit(f"missing required tool: {name}")


def find_server_pid() -> int:
    configured = os.environ.get("MAKO_PID")
    if configured:
        return int(configured)
    result = subprocess.run(
        ["pgrep", "-f", r"(^|/)makoCon$"],
        check=False,
        capture_output=True,
        text=True,
    )
    pids = [int(value) for value in result.stdout.split()]
    if len(pids) != 1:
        raise SystemExit(f"set MAKO_PID: expected one makoCon process, found {pids}")
    return pids[0]


def request_worker_tids(pid: int) -> set[int]:
    workers: dict[int, int] = {}
    task_root = Path(f"/proc/{pid}/task")
    if not task_root.is_dir():
        raise SystemExit(f"makoCon PID {pid} is not running")
    for task in sorted(task_root.iterdir(), key=lambda path: int(path.name)):
        try:
            name = (task / "comm").read_text().strip()
        except OSError:
            continue
        match = re.fullmatch(r"mako-worker-(\d+)", name)
        if match:
            workers.setdefault(int(match.group(1)), int(task.name))
    expected = set(range(max(workers, default=-1) + 1))
    if not workers or set(workers) != expected:
        raise SystemExit(f"could not identify a contiguous request-worker set: {workers}")
    return set(workers.values())


def redis_cli(*args: str, capture: bool = False) -> str:
    result = subprocess.run(
        ["redis-cli", "-h", HOST, "-p", str(PORT), *args],
        check=True,
        capture_output=capture,
        text=True,
    )
    return result.stdout if capture else ""


def command_count() -> int:
    output = redis_cli("--raw", "INFO", "stats", capture=True)
    for line in output.splitlines():
        if line.startswith("total_commands_processed:"):
            return int(line.split(":", 1)[1].strip())
    raise RuntimeError("INFO stats omitted total_commands_processed")


def parse_pidstat(output: str, pid: int, request_tids: set[int]) -> tuple[int, float, float, float]:
    total_cpu: float | None = None
    cpu_by_tid: dict[int, float] = {}
    for line in output.splitlines():
        fields = line.split()
        if not fields or fields[0] != "Average:" or len(fields) < 11 or fields[1] == "UID":
            continue
        if fields[2] == str(pid) and fields[3] == "-":
            total_cpu = float(fields[8])
        elif fields[2] == "-":
            try:
                cpu_by_tid[int(fields[3])] = float(fields[8])
            except ValueError:
                continue
    if total_cpu is None:
        raise RuntimeError("pidstat aggregate average was not found")
    worker_cpu = sum(cpu_by_tid.get(tid, 0.0) for tid in request_tids)
    active_workers = sum(1 for tid in request_tids if cpu_by_tid.get(tid, 0.0) >= 5.0)
    return active_workers, worker_cpu, max(0.0, total_cpu - worker_cpu), total_cpu


def stop_process_group(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def run_sample(
    pid: int,
    request_tids: set[int],
    mode: str,
    clients: int,
    client_threads: int,
) -> list[object]:
    command = [
        "redis-benchmark",
        "-h",
        HOST,
        "-p",
        str(PORT),
        "-c",
        str(clients),
        "--threads",
        str(client_threads),
        "-n",
        "100000000",
        "GET",
        "benchmark:cpu:key",
    ]
    benchmark = subprocess.Popen(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    try:
        time.sleep(1.0)
        if benchmark.poll() is not None:
            raise RuntimeError(f"redis-benchmark exited early with status {benchmark.returncode}")
        redis_cli("CONFIG", "RESETSTAT")
        before = command_count()
        started = time.monotonic()
        sample = subprocess.run(
            ["pidstat", "-t", "-p", str(pid), "1", str(SAMPLE_SECONDS)],
            check=True,
            capture_output=True,
            text=True,
        )
        elapsed = time.monotonic() - started
        after = command_count()
    finally:
        stop_process_group(benchmark)

    active, worker_cpu, helper_cpu, total_cpu = parse_pidstat(sample.stdout, pid, request_tids)
    return [
        mode,
        clients,
        client_threads,
        active,
        f"{worker_cpu:.2f}",
        f"{helper_cpu:.2f}",
        f"{total_cpu:.2f}",
        f"{total_cpu / 100:.4f}",
        round((after - before) / elapsed),
    ]


def main() -> None:
    for tool in ("pgrep", "pidstat", "redis-benchmark", "redis-cli"):
        require_tool(tool)
    pid = find_server_pid()
    workers = request_worker_tids(pid)
    redis_cli("PING")
    redis_cli("SET", "benchmark:cpu:key", "value")

    rows: list[list[object]] = []
    for mode in ("single", "matching"):
        for clients in CLIENTS:
            client_threads = 1 if mode == "single" else clients
            row = run_sample(pid, workers, mode, clients, client_threads)
            rows.append(row)
            print(",".join(str(value) for value in row), flush=True)
            time.sleep(0.25)

    OUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    with OUT_FILE.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "mode",
                "clients",
                "client_threads",
                "active_workers",
                "worker_cpu_pct",
                "helper_cpu_pct",
                "total_cpu_pct",
                "total_cores",
                "ops_per_sec",
            ]
        )
        writer.writerows(rows)
    print(f"wrote {OUT_FILE}")


if __name__ == "__main__":
    main()
