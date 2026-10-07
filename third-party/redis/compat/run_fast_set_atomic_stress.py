#!/usr/bin/env python3
"""Persist a repeatable concurrent fast-SET/GET atomicity artifact."""

from __future__ import annotations

import concurrent.futures
import hashlib
import json
import os
import socket
import subprocess
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

import redis


ROOT = Path(__file__).resolve().parents[3]


def env_int(name: str, default: int, minimum: int = 1) -> int:
    value = int(os.environ.get(name, str(default)))
    if value < minimum:
        raise SystemExit(f"{name} must be >= {minimum}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def mako_counters(client: redis.Redis) -> dict[str, int]:
    info = client.info("mako")
    return {
        name: int(info[name])
        for name in ("mako_txn_commits", "mako_txn_aborts", "mako_txn_retries")
    }


def main() -> None:
    host = os.environ.get("MAKO_ATOMIC_HOST", "127.0.0.1")
    port = env_int("MAKO_ATOMIC_PORT", 6380)
    iterations = env_int("MAKO_ATOMIC_ITERATIONS", 100)
    writers = env_int("MAKO_ATOMIC_WRITERS", 4)
    readers = env_int("MAKO_ATOMIC_READERS", 4)
    operations_per_client = env_int("MAKO_ATOMIC_OPERATIONS_PER_CLIENT", 500)
    value_size = env_int("MAKO_ATOMIC_VALUE_SIZE", 4096)
    timeout_seconds = env_int("MAKO_ATOMIC_TIMEOUT_SECONDS", 60)
    server_binary = Path(
        os.environ.get("MAKO_ATOMIC_SERVER_BIN", str(ROOT / "build" / "makoCon"))
    ).resolve()
    if not server_binary.is_file():
        raise SystemExit(f"missing server binary: {server_binary}")
    default_out = (
        ROOT
        / "third-party"
        / "redis"
        / "compat"
        / "atomic_logs"
        / datetime.now(timezone.utc).strftime("fast_set_%Y%m%d_%H%M%S")
    )
    out_dir = Path(os.environ.get("MAKO_ATOMIC_OUT_DIR", str(default_out)))
    out_dir.mkdir(parents=True, exist_ok=False)

    setup = redis.Redis(host=host, port=port, decode_responses=False)
    if not setup.ping():
        raise SystemExit(f"server did not answer PING at {host}:{port}")
    counters_before = mako_counters(setup)
    start = time.monotonic()
    completed_iterations = 0

    for iteration in range(iterations):
        name = f"mako:stress:fast-set:{uuid.uuid4()}:{iteration}".encode()
        seed = b"seed:" + b"S" * value_size
        values = [
            f"writer:{writer}:".encode() + bytes([65 + writer % 26]) * value_size
            for writer in range(writers)
        ]
        allowed = {seed, *values}
        if setup.set(name, seed) is not True:
            raise RuntimeError(f"iteration {iteration}: seed SET failed")
        barrier = threading.Barrier(writers + readers)

        def writer(value: bytes) -> None:
            client = redis.Redis(host=host, port=port, decode_responses=False)
            try:
                barrier.wait(timeout=timeout_seconds)
                for _ in range(operations_per_client):
                    if client.set(name, value) is not True:
                        raise RuntimeError("SET failed")
            finally:
                client.close()

        def reader() -> None:
            client = redis.Redis(host=host, port=port, decode_responses=False)
            try:
                barrier.wait(timeout=timeout_seconds)
                for _ in range(operations_per_client):
                    value = client.get(name)
                    if value not in allowed:
                        raise RuntimeError(
                            f"torn or unexpected value length={len(value) if value else None}"
                        )
            finally:
                client.close()

        with concurrent.futures.ThreadPoolExecutor(
            max_workers=writers + readers
        ) as pool:
            futures = [pool.submit(writer, value) for value in values]
            futures.extend(pool.submit(reader) for _ in range(readers))
            for future in futures:
                future.result(timeout=timeout_seconds)
        if setup.get(name) not in allowed:
            raise RuntimeError(f"iteration {iteration}: invalid final value")
        setup.delete(name)
        completed_iterations += 1
        if completed_iterations % 10 == 0:
            print(f"completed_iterations={completed_iterations}", flush=True)

    elapsed = time.monotonic() - start
    counters_after = mako_counters(setup)
    setup.close()
    counter_delta = {
        name: counters_after[name] - counters_before[name] for name in counters_before
    }
    commit = subprocess.run(
        ["git", "-c", f"safe.directory={ROOT}", "-C", str(ROOT), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    git_status = subprocess.run(
        ["git", "-C", str(ROOT), "status", "--short"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    source = ROOT / "third-party" / "redis" / "cpp" / "makoCon.cc"
    result = {
        "status": "pass",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "host": socket.gethostname(),
        "commit": commit,
        "git_status_short": git_status,
        "server": {"host": host, "port": port},
        "server_binary": str(server_binary),
        "server_binary_sha256": sha256_file(server_binary),
        "makoCon_source_sha256": sha256_file(source),
        "iterations": completed_iterations,
        "writers": writers,
        "readers": readers,
        "operations_per_client_per_iteration": operations_per_client,
        "value_payload_bytes": value_size,
        "set_operations": completed_iterations * writers * operations_per_client,
        "get_operations": completed_iterations * readers * operations_per_client,
        "elapsed_seconds": elapsed,
        "assertion": "every GET returned one complete value written by a writer or the seed",
        "counters_before": counters_before,
        "counters_after": counters_after,
        "counter_delta": counter_delta,
    }
    target = out_dir / "atomic_stress.json"
    target.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(target)


if __name__ == "__main__":
    main()
