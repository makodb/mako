#!/usr/bin/env python3
"""Run Rolis-style worker scaling curves for direct Mako and Redis-over-Mako."""

from __future__ import annotations

import csv
import hashlib
import json
import math
import os
import random
import re
import shutil
import signal
import socket
import statistics
import subprocess
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
COMPAT_DIR = ROOT / "third-party" / "redis" / "compat"


def env_int(name: str, default: int, minimum: int = 0) -> int:
    value = int(os.environ.get(name, str(default)))
    if value < minimum:
        raise SystemExit(f"{name} must be >= {minimum}")
    return value


def parse_ints(raw: str) -> list[int]:
    values = [int(value) for value in re.split(r"[\s,]+", raw.strip()) if value]
    if not values or len(values) != len(set(values)) or any(value < 1 for value in values):
        raise SystemExit(f"invalid positive integer list: {raw!r}")
    return values


def parse_cpu_pool(raw: str) -> list[int]:
    cpus: list[int] = []
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        if "-" in item:
            start_text, end_text = item.split("-", 1)
            start, end = int(start_text), int(end_text)
            if end < start:
                raise SystemExit(f"invalid CPU range: {item}")
            cpus.extend(range(start, end + 1))
        else:
            cpus.append(int(item))
    if not cpus or len(cpus) != len(set(cpus)) or any(cpu < 0 for cpu in cpus):
        raise SystemExit(f"invalid CPU pool: {raw!r}")
    return cpus


def cpu_list(cpus: list[int]) -> str:
    return ",".join(str(cpu) for cpu in cpus)


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise SystemExit(f"missing required tool: {name}")
    return path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


@dataclass(frozen=True)
class Config:
    server_bin: Path
    direct_bin: Path
    resp_bin: Path
    workers: list[int]
    workloads: list[str]
    targets: set[str]
    keys: int
    value_size: int
    read_percent: int
    duration: int
    warmup: int
    repeats: int
    clients_per_worker: int
    preload_threads: int
    preload_max_retries: int
    server_settle_seconds: int
    pipeline_depth: int
    host: str
    port: int
    server_cpu_pool: list[int]
    client_cpu_pool: list[int]
    out_dir: Path
    commit: str
    direct_raw: Path | None
    profile: str
    experiment_seed: int
    max_latency_samples_per_thread: int


def load_config() -> Config:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    profile = os.environ.get("MAKO_SCALING_PROFILE", "development")
    if profile not in {"development", "paper", "paper-soak"}:
        raise SystemExit(
            "MAKO_SCALING_PROFILE must be development, paper, or paper-soak"
        )
    paper = profile == "paper"
    paper_soak = profile == "paper-soak"
    build_dir = Path(os.environ.get("MAKO_SCALING_BUILD_DIR", "/tmp/codex-pr72-build-final"))
    workers = parse_ints(os.environ.get("MAKO_SCALING_WORKERS", "1 2 4 8 16 24 32"))
    workloads = [
        value.strip()
        for value in os.environ.get(
            "MAKO_SCALING_WORKLOADS", "get,set,mixed"
        ).split(",")
        if value.strip()
    ]
    if not workloads or any(value not in {"get", "set", "mixed"} for value in workloads):
        raise SystemExit("MAKO_SCALING_WORKLOADS must contain get, set, or mixed")
    targets = {
        value.strip()
        for value in os.environ.get("MAKO_SCALING_TARGETS", "direct,redis").split(",")
        if value.strip()
    }
    if not targets or not targets <= {"direct", "redis"}:
        raise SystemExit("MAKO_SCALING_TARGETS must contain direct and/or redis")

    commit = subprocess.run(
        [
            "git",
            "-c",
            f"safe.directory={ROOT}",
            "-C",
            str(ROOT),
            "rev-parse",
            "HEAD",
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    out_dir = Path(
        os.environ.get(
            "MAKO_SCALING_OUT_DIR",
            str(COMPAT_DIR / "benchmark_logs" / f"scalability_{stamp}_{commit[:8]}"),
        )
    )

    config = Config(
        server_bin=Path(
            os.environ.get("MAKO_SCALING_SERVER_BIN", str(build_dir / "makoCon"))
        ),
        direct_bin=Path(
            os.environ.get(
                "MAKO_SCALING_DIRECT_BIN", str(build_dir / "makoRedisDirectBench")
            )
        ),
        resp_bin=Path(
            os.environ.get(
                "MAKO_SCALING_RESP_BIN", "/tmp/mako_bench_resp_scalability"
            )
        ),
        workers=workers,
        workloads=workloads,
        targets=targets,
        keys=env_int("MAKO_SCALING_KEYS", 1_000_000, 1),
        value_size=env_int("MAKO_SCALING_VALUE_SIZE", 8),
        read_percent=env_int("MAKO_SCALING_READ_PERCENT", 80),
        duration=env_int(
            "MAKO_SCALING_DURATION", 1800 if paper_soak else (30 if paper else 20), 1
        ),
        warmup=env_int(
            "MAKO_SCALING_WARMUP", 10 if paper or paper_soak else 2
        ),
        repeats=env_int(
            "MAKO_SCALING_REPEATS", 1 if paper_soak else (5 if paper else 3), 1
        ),
        clients_per_worker=env_int("MAKO_SCALING_CLIENTS_PER_WORKER", 2, 1),
        preload_threads=env_int("MAKO_SCALING_PRELOAD_THREADS", 8, 1),
        preload_max_retries=env_int("MAKO_SCALING_PRELOAD_MAX_RETRIES", 100),
        server_settle_seconds=env_int("MAKO_SCALING_SERVER_SETTLE", 5),
        pipeline_depth=env_int("MAKO_SCALING_PIPELINE_DEPTH", 1, 1),
        host=os.environ.get("MAKO_SCALING_HOST", "127.0.0.1"),
        port=env_int("MAKO_SCALING_PORT", 6410, 1),
        server_cpu_pool=parse_cpu_pool(
            os.environ.get("MAKO_SCALING_SERVER_CPUS", "0-31")
        ),
        client_cpu_pool=parse_cpu_pool(
            os.environ.get("MAKO_SCALING_CLIENT_CPUS", "32-63")
        ),
        out_dir=out_dir,
        commit=commit,
        direct_raw=(
            Path(os.environ["MAKO_SCALING_DIRECT_RAW"])
            if os.environ.get("MAKO_SCALING_DIRECT_RAW")
            else None
        ),
        profile=profile,
        experiment_seed=env_int("MAKO_SCALING_SEED", 20260822),
        max_latency_samples_per_thread=env_int(
            "MAKO_SCALING_MAX_LATENCY_SAMPLES_PER_THREAD", 65_536, 1
        ),
    )
    if config.workers != sorted(config.workers):
        raise SystemExit("MAKO_SCALING_WORKERS must be sorted")
    if config.read_percent < 0 or config.read_percent > 100:
        raise SystemExit("MAKO_SCALING_READ_PERCENT must be between 0 and 100")
    if config.pipeline_depth > 4096:
        raise SystemExit("MAKO_SCALING_PIPELINE_DEPTH must be <= 4096")
    if config.max_latency_samples_per_thread > 10_000_000:
        raise SystemExit(
            "MAKO_SCALING_MAX_LATENCY_SAMPLES_PER_THREAD must be <= 10000000"
        )
    if config.profile == "paper" and (
        config.duration < 30 or config.warmup < 10 or config.repeats < 5
    ):
        raise SystemExit(
            "paper profile requires duration >= 30, warmup >= 10, and repeats >= 5"
        )
    if config.profile == "paper-soak" and (
        config.duration < 1800
        or config.warmup < 10
        or config.repeats != 1
        or config.targets != {"redis"}
    ):
        raise SystemExit(
            "paper-soak profile requires Redis only, duration >= 1800, "
            "warmup >= 10, and exactly one repeat"
        )
    if max(config.workers) > len(config.server_cpu_pool):
        raise SystemExit("server CPU pool is smaller than the largest worker count")
    if set(config.server_cpu_pool) & set(config.client_cpu_pool):
        raise SystemExit("server and client CPU pools must not overlap")
    if config.direct_raw is not None:
        if "direct" in config.targets:
            raise SystemExit(
                "MAKO_SCALING_DIRECT_RAW cannot be combined with the direct target"
            )
        if not config.direct_raw.is_file():
            raise SystemExit(
                f"MAKO_SCALING_DIRECT_RAW does not exist: {config.direct_raw}"
            )
    return config


def ordered_workloads(config: Config, workers: int) -> list[str]:
    """Return a deterministic randomized order to avoid fixed-order drift."""
    workloads = list(config.workloads)
    random.Random(config.experiment_seed + workers).shuffle(workloads)
    return workloads


def validate_physical_core_isolation(config: Config) -> None:
    output = subprocess.run(
        ["lscpu", "-p=CPU,CORE"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    core_by_cpu: dict[int, int] = {}
    for line in output.splitlines():
        if not line or line.startswith("#"):
            continue
        cpu_text, core_text = line.split(",", 1)
        core_by_cpu[int(cpu_text)] = int(core_text)
    server_cores = {core_by_cpu[cpu] for cpu in config.server_cpu_pool}
    client_cores = {core_by_cpu[cpu] for cpu in config.client_cpu_pool}
    shared = server_cores & client_cores
    if shared:
        raise SystemExit(
            f"server/client CPU pools share physical cores: {sorted(shared)}"
        )


def compile_resp_benchmark(config: Config) -> None:
    source = COMPAT_DIR / "bench_resp_scalability.cpp"
    config.resp_bin.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            require_tool("g++"),
            "-std=c++20",
            "-O3",
            "-DNDEBUG",
            "-pthread",
            str(source),
            "-o",
            str(config.resp_bin),
        ],
        check=True,
    )


def wait_for_server(process: subprocess.Popen[bytes], host: str, port: int) -> None:
    request = b"*1\r\n$4\r\nPING\r\n"
    deadline = time.monotonic() + 30
    last_error = "not ready"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"makoCon exited during startup: {process.returncode}")
        try:
            with socket.create_connection((host, port), timeout=0.5) as sock:
                sock.sendall(request)
                response = sock.recv(64)
                if response.startswith(b"+PONG"):
                    return
                last_error = f"unexpected PING response: {response!r}"
        except OSError as error:
            last_error = str(error)
        time.sleep(0.1)
    raise RuntimeError(f"makoCon did not become ready: {last_error}")


def stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def request_worker_tids(pid: int) -> set[int]:
    tids_by_worker: dict[int, list[int]] = {}
    for task in Path(f"/proc/{pid}/task").iterdir():
        try:
            name = (task / "comm").read_text().strip()
        except OSError:
            continue
        match = re.fullmatch(r"mako-worker-(\d+)", name)
        if match:
            tids_by_worker.setdefault(int(match.group(1)), []).append(int(task.name))
    if not tids_by_worker:
        raise RuntimeError(f"no mako-worker threads found for PID {pid}")
    # Transport helpers inherit their creator's Linux thread name. The Rust
    # request worker is created first and therefore has the lowest TID for a
    # given mako-worker-N name.
    return {min(tids) for tids in tids_by_worker.values()}


def wait_for_worker_tids(
    process: subprocess.Popen[bytes], expected: int, timeout_seconds: float = 10.0
) -> set[int]:
    deadline = time.monotonic() + timeout_seconds
    last_count = 0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"makoCon exited while waiting for worker threads: {process.returncode}"
            )
        try:
            tids = request_worker_tids(process.pid)
        except (FileNotFoundError, RuntimeError):
            tids = set()
        last_count = len(tids)
        if last_count == expected:
            return tids
        if last_count > expected:
            raise RuntimeError(
                f"expected {expected} Redis workers, found {last_count}"
            )
        time.sleep(0.1)
    raise RuntimeError(
        f"timed out waiting for {expected} Redis workers; found {last_count}"
    )


def parse_pidstat(
    output: str, pid: int, worker_tids: set[int]
) -> tuple[int, float, float, float]:
    total_cpu: float | None = None
    cpu_by_tid: dict[int, float] = {}
    for line in output.splitlines():
        fields = line.split()
        if (
            not fields
            or fields[0] != "Average:"
            or len(fields) < 11
            or fields[1] == "UID"
        ):
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
    worker_cpu = sum(cpu_by_tid.get(tid, 0.0) for tid in worker_tids)
    active_workers = sum(
        1 for tid in worker_tids if cpu_by_tid.get(tid, 0.0) >= 5.0
    )
    helper_cpu = max(0.0, total_cpu - worker_cpu)
    return active_workers, worker_cpu / 100.0, helper_cpu / 100.0, total_cpu / 100.0


def parse_process_cpu(output: str, pid: int) -> float:
    for line in output.splitlines():
        fields = line.split()
        if (
            fields
            and fields[0] == "Average:"
            and len(fields) >= 11
            and fields[1] != "UID"
            and fields[2] == str(pid)
            and fields[3] == "-"
        ):
            return float(fields[8]) / 100.0
    raise RuntimeError(f"pidstat aggregate average was not found for PID {pid}")


RAW_FIELDS = [
    "timestamp_utc",
    "commit",
    "benchmark",
    "workload",
    "workers",
    "clients",
    "shards",
    "key_count",
    "value_size",
    "read_percent",
    "repeat",
    "duration_sec",
    "total_ops",
    "ops_per_sec",
    "ops_per_sec_per_worker",
    "active_workers",
    "worker_cpu_cores",
    "helper_cpu_cores",
    "server_cpu_cores",
    "allocated_cpu_util_pct",
    "client_cpu_cores",
    "client_allocated_cpu_util_pct",
    "ops_per_used_core",
    "p50_us",
    "p95_us",
    "p99_us",
    "latency_observations",
    "latency_samples",
    "latency_sampling",
    "aborts",
    "server_cpu_set",
    "client_cpu_set",
]


def append_row(writer: csv.DictWriter[str], row: dict[str, object]) -> None:
    writer.writerow({field: row.get(field, "") for field in RAW_FIELDS})


def import_direct_rows(config: Config, writer: csv.DictWriter[str]) -> None:
    assert config.direct_raw is not None
    with config.direct_raw.open(newline="") as handle:
        source_rows = [
            row
            for row in csv.DictReader(handle)
            if row["benchmark"] == "direct-mako"
            and int(row["workers"]) in config.workers
            and row["workload"] in config.workloads
        ]

    expected_groups = {
        (workload, workers)
        for workload in config.workloads
        for workers in config.workers
    }
    actual_groups: dict[tuple[str, int], int] = {}
    for row in source_rows:
        if (
            int(row["key_count"]) != config.keys
            or int(row["value_size"]) != config.value_size
            or int(row["read_percent"]) != config.read_percent
        ):
            raise RuntimeError(
                f"direct baseline settings do not match: {config.direct_raw}"
            )
        group = (row["workload"], int(row["workers"]))
        actual_groups[group] = actual_groups.get(group, 0) + 1
        writer.writerow({field: row.get(field, "") for field in RAW_FIELDS})

    if set(actual_groups) != expected_groups or any(
        count != config.repeats for count in actual_groups.values()
    ):
        raise RuntimeError(
            "direct baseline does not contain every requested "
            f"workload/worker point with {config.repeats} repeats"
        )
    print(f"Imported direct Mako baseline: {config.direct_raw}", flush=True)


def run_direct(config: Config, raw_writer: csv.DictWriter[str]) -> None:
    for workers in config.workers:
        workloads = ordered_workloads(config, workers)
        cpu_set = cpu_list(config.server_cpu_pool[:workers])
        result_csv = config.out_dir / f"direct_{workers}w.csv"
        log_path = config.out_dir / f"direct_{workers}w.log"
        command = [
            "taskset",
            "-c",
            cpu_set,
            str(config.direct_bin),
            "--keys",
            str(config.keys),
            "--value-size",
            str(config.value_size),
            "--threads",
            str(workers),
            "--duration",
            str(config.duration),
            "--warmup",
            str(config.warmup),
            "--repeats",
            str(config.repeats),
            "--read-percent",
            str(config.read_percent),
            "--workloads",
            ",".join(workloads),
            "--out",
            str(result_csv),
        ]
        print(f"direct Mako workers={workers}", flush=True)
        with log_path.open("wb") as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        with result_csv.open(newline="") as handle:
            for source in csv.DictReader(handle):
                server_cpu = float(source["process_cpu_cores"])
                append_row(
                    raw_writer,
                    {
                        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                        "commit": config.commit,
                        "benchmark": "direct-mako",
                        "workload": source["workload"],
                        "workers": workers,
                        "clients": 0,
                        "shards": 1,
                        "key_count": config.keys,
                        "value_size": config.value_size,
                        "read_percent": config.read_percent,
                        "repeat": source["repeat"],
                        "duration_sec": source["duration_sec"],
                        "total_ops": source["total_ops"],
                        "ops_per_sec": source["ops_per_sec"],
                        "ops_per_sec_per_worker": source[
                            "ops_per_sec_per_worker"
                        ],
                        "active_workers": workers,
                        "worker_cpu_cores": server_cpu,
                        "helper_cpu_cores": 0,
                        "server_cpu_cores": server_cpu,
                        "allocated_cpu_util_pct": server_cpu / workers * 100,
                        "ops_per_used_core": source["ops_per_used_core"],
                        "aborts": source["aborts"],
                        "server_cpu_set": cpu_set,
                    },
                )


def resp_command(
    config: Config,
    clients: int,
    client_cpu_set: str,
    workload: str,
    duration: int,
    out_csv: Path,
) -> list[str]:
    return [
        "taskset",
        "-c",
        client_cpu_set,
        str(config.resp_bin),
        "--name",
        "redis-over-mako",
        "--host",
        config.host,
        "--port",
        str(config.port),
        "--keys",
        str(config.keys),
        "--value-size",
        str(config.value_size),
        "--threads",
        str(clients),
        "--duration",
        str(duration),
        "--read-percent",
        str(config.read_percent),
        "--pipeline-depth",
        str(config.pipeline_depth),
        "--max-latency-samples-per-thread",
        str(config.max_latency_samples_per_thread),
        "--workloads",
        workload,
        "--skip-preload",
        "--out",
        str(out_csv),
    ]


def run_redis(config: Config, raw_writer: csv.DictWriter[str]) -> None:
    for workers in config.workers:
        clients = workers * config.clients_per_worker
        server_cpu_set = cpu_list(config.server_cpu_pool[:workers])
        client_allocated_cpus = min(clients, len(config.client_cpu_pool))
        client_cpu_set = cpu_list(config.client_cpu_pool[:client_allocated_cpus])
        server_log_path = config.out_dir / f"redis_server_{workers}w.log"
        server_env = os.environ.copy()
        server_env.update(
            {
                "MAKO_HOST": config.host,
                "MAKO_PORT": str(config.port),
                "MAKO_REDIS_THREADS": str(workers),
                "MAKO_REDIS_BACKEND": "mako",
                "MAKO_REPLICATION_ENABLED": "0",
                "MAKO_PAXOS_PROC_NAME": "localhost",
            }
        )
        server_log = server_log_path.open("wb")
        server = subprocess.Popen(
            ["taskset", "-c", server_cpu_set, str(config.server_bin)],
            cwd=ROOT,
            env=server_env,
            stdin=subprocess.DEVNULL,
            stdout=server_log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            wait_for_server(server, config.host, config.port)
            tids = wait_for_worker_tids(server, workers)
            if config.server_settle_seconds:
                time.sleep(config.server_settle_seconds)
            print(
                f"Redis-over-Mako workers={workers} clients={clients}: preload",
                flush=True,
            )
            preload_log = config.out_dir / f"redis_{workers}w_preload.log"
            preload_threads = min(workers, config.preload_threads)
            with preload_log.open("wb") as log:
                subprocess.run(
                    [
                        "taskset",
                        "-c",
                        client_cpu_set,
                        str(config.resp_bin),
                        "--host",
                        config.host,
                        "--port",
                        str(config.port),
                        "--keys",
                        str(config.keys),
                        "--value-size",
                        str(config.value_size),
                        "--threads",
                        str(workers),
                        "--preload-threads",
                        str(preload_threads),
                        "--preload-max-retries",
                        str(config.preload_max_retries),
                        "--preload-only",
                    ],
                    cwd=ROOT,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    check=True,
                )

            for workload in ordered_workloads(config, workers):
                if config.warmup > 0:
                    warmup_csv = config.out_dir / f"redis_{workers}w_{workload}_warmup.csv"
                    warmup_log = config.out_dir / f"redis_{workers}w_{workload}_warmup.log"
                    with warmup_log.open("wb") as log:
                        subprocess.run(
                            resp_command(
                                config,
                                clients,
                                client_cpu_set,
                                workload,
                                config.warmup,
                                warmup_csv,
                            ),
                            cwd=ROOT,
                            stdout=log,
                            stderr=subprocess.STDOUT,
                            check=True,
                        )

                for repeat in range(1, config.repeats + 1):
                    result_csv = config.out_dir / (
                        f"redis_{workers}w_{workload}_r{repeat}.csv"
                    )
                    bench_log_path = config.out_dir / (
                        f"redis_{workers}w_{workload}_r{repeat}.log"
                    )
                    pidstat_path = config.out_dir / (
                        f"redis_{workers}w_{workload}_r{repeat}_pidstat.txt"
                    )
                    print(
                        f"Redis-over-Mako workers={workers} clients={clients} "
                        f"workload={workload} repeat={repeat}",
                        flush=True,
                    )
                    with bench_log_path.open("wb") as bench_log:
                        benchmark = subprocess.Popen(
                            resp_command(
                                config,
                                clients,
                                client_cpu_set,
                                workload,
                                config.duration,
                                result_csv,
                            ),
                            cwd=ROOT,
                            stdout=bench_log,
                            stderr=subprocess.STDOUT,
                            start_new_session=True,
                        )
                        pidstat = subprocess.run(
                            [
                                "pidstat",
                                "-t",
                                "-p",
                                f"{server.pid},{benchmark.pid}",
                                "1",
                                str(config.duration),
                            ],
                            check=True,
                            capture_output=True,
                            text=True,
                        )
                        pidstat_path.write_text(pidstat.stdout)
                        if benchmark.wait() != 0:
                            raise RuntimeError(
                                f"RESP benchmark failed; see {bench_log_path}"
                            )
                    active, worker_cpu, helper_cpu, total_cpu = parse_pidstat(
                        pidstat.stdout, server.pid, tids
                    )
                    client_cpu = parse_process_cpu(pidstat.stdout, benchmark.pid)
                    with result_csv.open(newline="") as handle:
                        rows = list(csv.DictReader(handle))
                    if len(rows) != 1:
                        raise RuntimeError(
                            f"expected one row in {result_csv}, found {len(rows)}"
                        )
                    source = rows[0]
                    throughput = float(source["ops_per_sec"])
                    append_row(
                        raw_writer,
                        {
                            "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                            "commit": config.commit,
                            "benchmark": "redis-over-mako",
                            "workload": workload,
                            "workers": workers,
                            "clients": clients,
                            "shards": 1,
                            "key_count": config.keys,
                            "value_size": config.value_size,
                            "read_percent": config.read_percent,
                            "repeat": repeat,
                            "duration_sec": source["duration_sec"],
                            "total_ops": source["total_ops"],
                            "ops_per_sec": source["ops_per_sec"],
                            "ops_per_sec_per_worker": throughput / workers,
                            "active_workers": active,
                            "worker_cpu_cores": worker_cpu,
                            "helper_cpu_cores": helper_cpu,
                            "server_cpu_cores": total_cpu,
                            "allocated_cpu_util_pct": total_cpu / workers * 100,
                            "client_cpu_cores": client_cpu,
                            "client_allocated_cpu_util_pct": (
                                client_cpu / client_allocated_cpus * 100
                            ),
                            "ops_per_used_core": (
                                throughput / total_cpu if total_cpu > 0 else 0
                            ),
                            "p50_us": source["p50_us"],
                            "p95_us": source["p95_us"],
                            "p99_us": source["p99_us"],
                            "latency_observations": source.get(
                                "latency_observations", ""
                            ),
                            "latency_samples": source.get("latency_samples", ""),
                            "latency_sampling": source.get("latency_sampling", ""),
                            "server_cpu_set": server_cpu_set,
                            "client_cpu_set": client_cpu_set,
                        },
                    )
        finally:
            stop_process(server)
            server_log.close()


SUMMARY_FIELDS = [
    "benchmark",
    "workload",
    "workers",
    "repeats",
    "mean_ops_per_sec",
    "stdev_ops_per_sec",
    "min_ops_per_sec",
    "max_ops_per_sec",
    "ci95_low_ops_per_sec",
    "ci95_high_ops_per_sec",
    "ci95_half_width_ops_per_sec",
    "coefficient_of_variation_pct",
    "throughput_pct_of_direct_mako",
    "speedup_vs_1",
    "scaling_efficiency_pct",
    "mean_ops_per_sec_per_worker",
    "mean_active_workers",
    "mean_worker_cpu_cores",
    "mean_helper_cpu_cores",
    "mean_server_cpu_cores",
    "mean_allocated_cpu_util_pct",
    "mean_client_cpu_cores",
    "mean_client_allocated_cpu_util_pct",
    "mean_ops_per_used_core",
    "mean_p50_us",
    "mean_p95_us",
    "mean_p99_us",
    "mean_latency_observations",
    "mean_latency_samples",
    "total_aborts",
]


T_CRITICAL_975 = {
    1: 12.706,
    2: 4.303,
    3: 3.182,
    4: 2.776,
    5: 2.571,
    6: 2.447,
    7: 2.365,
    8: 2.306,
    9: 2.262,
    10: 2.228,
    11: 2.201,
    12: 2.179,
    13: 2.160,
    14: 2.145,
    15: 2.131,
    16: 2.120,
    17: 2.110,
    18: 2.101,
    19: 2.093,
    20: 2.086,
    21: 2.080,
    22: 2.074,
    23: 2.069,
    24: 2.064,
    25: 2.060,
    26: 2.056,
    27: 2.052,
    28: 2.048,
    29: 2.045,
    30: 2.042,
}


def confidence_interval_95(values: list[float]) -> tuple[float, float, float]:
    """Student-t interval for the arithmetic mean of independent trials."""
    mean = statistics.mean(values)
    if len(values) < 2:
        return mean, mean, 0.0
    degrees = len(values) - 1
    critical = T_CRITICAL_975.get(degrees, 1.96)
    half_width = critical * statistics.stdev(values) / math.sqrt(len(values))
    return mean - half_width, mean + half_width, half_width


def mean_optional(rows: list[dict[str, str]], field: str) -> str:
    values = [float(row[field]) for row in rows if row.get(field, "") != ""]
    return f"{statistics.mean(values):.6f}" if values else ""


def summarize(raw_path: Path, summary_path: Path) -> None:
    with raw_path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    groups: dict[tuple[str, str, int], list[dict[str, str]]] = {}
    for row in rows:
        key = (row["benchmark"], row["workload"], int(row["workers"]))
        groups.setdefault(key, []).append(row)

    means = {
        key: statistics.mean(float(row["ops_per_sec"]) for row in group)
        for key, group in groups.items()
    }
    with summary_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=SUMMARY_FIELDS)
        writer.writeheader()
        for key in sorted(groups, key=lambda value: (value[1], value[0], value[2])):
            benchmark, workload, workers = key
            group = groups[key]
            throughputs = [float(row["ops_per_sec"]) for row in group]
            mean_throughput = statistics.mean(throughputs)
            stdev_throughput = (
                statistics.stdev(throughputs) if len(throughputs) > 1 else 0.0
            )
            ci_low, ci_high, ci_half_width = confidence_interval_95(throughputs)
            one_worker_baseline = means.get((benchmark, workload, 1))
            speedup = (
                mean_throughput / one_worker_baseline
                if one_worker_baseline is not None
                else None
            )
            direct_throughput = means.get(("direct-mako", workload, workers))
            writer.writerow(
                {
                    "benchmark": benchmark,
                    "workload": workload,
                    "workers": workers,
                    "repeats": len(group),
                    "mean_ops_per_sec": f"{mean_throughput:.6f}",
                    "stdev_ops_per_sec": (
                        f"{stdev_throughput:.6f}"
                    ),
                    "min_ops_per_sec": f"{min(throughputs):.6f}",
                    "max_ops_per_sec": f"{max(throughputs):.6f}",
                    "ci95_low_ops_per_sec": f"{ci_low:.6f}",
                    "ci95_high_ops_per_sec": f"{ci_high:.6f}",
                    "ci95_half_width_ops_per_sec": f"{ci_half_width:.6f}",
                    "coefficient_of_variation_pct": (
                        f"{stdev_throughput / mean_throughput * 100:.6f}"
                        if mean_throughput > 0
                        else "0.000000"
                    ),
                    "throughput_pct_of_direct_mako": (
                        f"{mean_throughput / direct_throughput * 100:.6f}"
                        if direct_throughput is not None
                        else ""
                    ),
                    "speedup_vs_1": (
                        f"{speedup:.6f}" if speedup is not None else ""
                    ),
                    "scaling_efficiency_pct": (
                        f"{speedup / workers * 100:.6f}"
                        if speedup is not None
                        else ""
                    ),
                    "mean_ops_per_sec_per_worker": mean_optional(
                        group, "ops_per_sec_per_worker"
                    ),
                    "mean_active_workers": mean_optional(group, "active_workers"),
                    "mean_worker_cpu_cores": mean_optional(
                        group, "worker_cpu_cores"
                    ),
                    "mean_helper_cpu_cores": mean_optional(
                        group, "helper_cpu_cores"
                    ),
                    "mean_server_cpu_cores": mean_optional(
                        group, "server_cpu_cores"
                    ),
                    "mean_allocated_cpu_util_pct": mean_optional(
                        group, "allocated_cpu_util_pct"
                    ),
                    "mean_client_cpu_cores": mean_optional(
                        group, "client_cpu_cores"
                    ),
                    "mean_client_allocated_cpu_util_pct": mean_optional(
                        group, "client_allocated_cpu_util_pct"
                    ),
                    "mean_ops_per_used_core": mean_optional(
                        group, "ops_per_used_core"
                    ),
                    "mean_p50_us": mean_optional(group, "p50_us"),
                    "mean_p95_us": mean_optional(group, "p95_us"),
                    "mean_p99_us": mean_optional(group, "p99_us"),
                    "mean_latency_observations": mean_optional(
                        group, "latency_observations"
                    ),
                    "mean_latency_samples": mean_optional(
                        group, "latency_samples"
                    ),
                    "total_aborts": sum(
                        int(row["aborts"]) for row in group if row.get("aborts")
                    ),
                }
            )


def write_manifest(config: Config, path: Path) -> None:
    lscpu = subprocess.run(
        ["lscpu"], check=True, capture_output=True, text=True
    ).stdout
    source_paths = [
        ROOT / "examples" / "makoRedisDirectBench.cc",
        COMPAT_DIR / "bench_resp_scalability.cpp",
        COMPAT_DIR / "run_scalability_benchmark.py",
        ROOT / "third-party" / "redis" / "cpp" / "makoCon.cc",
        ROOT / "third-party" / "redis" / "rust-lib" / "src" / "lib.rs",
    ]
    git_status = subprocess.run(
        ["git", "-C", str(ROOT), "status", "--short"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    manifest = {
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "commit": config.commit,
        "host": socket.gethostname(),
        "server_binary": str(config.server_bin),
        "direct_binary": str(config.direct_bin),
        "resp_binary": str(config.resp_bin),
        "binary_sha256": {
            "makoCon": sha256_file(config.server_bin)
            if config.server_bin.is_file()
            else None,
            "makoRedisDirectBench": sha256_file(config.direct_bin)
            if config.direct_bin.is_file()
            else None,
            "bench_resp_scalability": sha256_file(config.resp_bin)
            if config.resp_bin.is_file()
            else None,
        },
        "source_sha256": {
            str(source.relative_to(ROOT)): sha256_file(source)
            for source in source_paths
        },
        "git_status_short": git_status.splitlines(),
        "workers": config.workers,
        "workloads": config.workloads,
        "targets": sorted(config.targets),
        "keys": config.keys,
        "value_size": config.value_size,
        "read_percent": config.read_percent,
        "duration_seconds": config.duration,
        "warmup_seconds": config.warmup,
        "repeats": config.repeats,
        "profile": config.profile,
        "experiment_seed": config.experiment_seed,
        "clients_per_worker": config.clients_per_worker,
        "server_settle_seconds_before_preload": config.server_settle_seconds,
        "preload_threads_cap": config.preload_threads,
        "preload_max_retries_per_key": config.preload_max_retries,
        "effective_preload_threads": {
            str(workers): min(workers, config.preload_threads)
            for workers in config.workers
        },
        "direct_raw_source": str(config.direct_raw)
        if config.direct_raw is not None
        else None,
        "direct_raw_sha256": sha256_file(config.direct_raw)
        if config.direct_raw is not None
        else None,
        "shards": 1,
        "replication": False,
        "pipeline_depth": config.pipeline_depth,
        "max_latency_samples_per_thread": config.max_latency_samples_per_thread,
        "latency_sampling": "deterministic reservoir sampling per client thread",
        "benchmark_ablation": {
            "skip_ttl": os.environ.get("MAKO_REDIS_BENCH_SKIP_TTL", "0"),
            "no_database": os.environ.get("MAKO_REDIS_BENCH_NO_DB", "0"),
            "skip_locks": os.environ.get("MAKO_REDIS_BENCH_SKIP_LOCKS", "0"),
            "skip_mutex": os.environ.get("MAKO_REDIS_BENCH_SKIP_MUTEX", "0"),
            "value_size": os.environ.get("MAKO_REDIS_BENCH_VALUE_SIZE", "8"),
        },
        "load_model": "closed-loop",
        "measurement_unit": "successful single-key Redis commands per second",
        "comparison_scope": (
            "same-host direct Mako is an in-process upper bound; results are not "
            "equivalent to multi-operation replicated Rolis or Mako transactions"
        ),
        "server_cpu_pool": config.server_cpu_pool,
        "client_cpu_pool": config.client_cpu_pool,
        "lscpu": lscpu,
    }
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


def main() -> None:
    for tool in ("g++", "lscpu", "pidstat", "taskset"):
        require_tool(tool)
    config = load_config()
    validate_physical_core_isolation(config)
    config.out_dir.mkdir(parents=True, exist_ok=True)

    if "redis" in config.targets:
        if not config.server_bin.is_file():
            raise SystemExit(f"missing makoCon binary: {config.server_bin}")
        compile_resp_benchmark(config)
    if "direct" in config.targets and not config.direct_bin.is_file():
        raise SystemExit(f"missing direct Mako binary: {config.direct_bin}")

    manifest_path = config.out_dir / "manifest.json"
    raw_path = config.out_dir / "scalability_raw.csv"
    summary_path = config.out_dir / "scalability_summary.csv"
    write_manifest(config, manifest_path)

    with raw_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=RAW_FIELDS)
        writer.writeheader()
        if config.direct_raw is not None:
            import_direct_rows(config, writer)
            handle.flush()
        if "direct" in config.targets:
            run_direct(config, writer)
            handle.flush()
        if "redis" in config.targets:
            run_redis(config, writer)
            handle.flush()

    summarize(raw_path, summary_path)
    print(f"Raw results: {raw_path}")
    print(f"Summary: {summary_path}")
    print(f"Manifest: {manifest_path}")


if __name__ == "__main__":
    main()
