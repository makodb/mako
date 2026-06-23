#!/usr/bin/env python3
from __future__ import annotations

import os
import json
import random
import subprocess
import threading
import time
from pathlib import Path

from harness_common import connect, env_int, fail, main_guard, na


def redis_int(value: bytes | None) -> int:
    if value is None:
        return 0
    return int(value)


def verify_rmw_history(
    history: list[dict[str, object]],
    final_values: dict[str, int],
    keys: list[str],
) -> list[str]:
    writes_by_key: dict[str, list[int]] = {key: [] for key in keys}
    anomalies: list[str] = []

    for event in history:
        if event.get("type") != "ok" or event.get("f") != "rmw":
            continue
        value = event.get("value")
        if not isinstance(value, dict):
            anomalies.append(f"malformed ok event value: {value!r}")
            continue
        key = value.get("key")
        read = value.get("read")
        write = value.get("write")
        if not isinstance(key, str) or not isinstance(read, int) or not isinstance(write, int):
            anomalies.append(f"malformed rmw event: {value!r}")
            continue
        if write != read + 1:
            anomalies.append(f"{key}: transaction returned read={read} write={write}, expected write={read + 1}")
            continue
        writes_by_key.setdefault(key, []).append(write)

    for key in keys:
        writes = writes_by_key.get(key, [])
        expected = list(range(1, len(writes) + 1))
        observed = sorted(writes)
        if observed != expected:
            anomalies.append(f"{key}: committed writes are not contiguous expected={expected[:5]}... len={len(expected)} observed_len={len(observed)}")
        final = final_values.get(key, 0)
        if final != len(writes):
            anomalies.append(f"{key}: final value {final} does not match committed write count {len(writes)}")

    return anomalies


def run_rmw_isolation_check() -> None:
    client = connect()
    prefix = f"g4:rmw:{int(time.time() * 1000)}"
    keys = [f"{prefix}:{i}" for i in range(env_int("MAKO_G4_KEYS", 10))]
    clients = env_int("MAKO_G4_CLIENTS", 16)
    duration = env_int("MAKO_G4_DURATION", 30)
    iterations = env_int("MAKO_G4_ITERATIONS", 0)
    if duration <= 0 and iterations <= 0:
        fail("MAKO_G4_DURATION or MAKO_G4_ITERATIONS must be positive")
    history_path = Path(os.environ.get("MAKO_G4_HISTORY_OUT", "third-party/redis/compat/g4_history.json"))
    history_lock = threading.Lock()
    history: list[dict[str, object]] = []
    for key in keys:
        client.set(key, 0)

    errors: list[BaseException] = []
    barrier = threading.Barrier(clients)
    stop_at = time.monotonic() + duration if duration > 0 else None
    operation_count = 0
    operation_count_lock = threading.Lock()

    def worker(seed: int) -> None:
        nonlocal operation_count
        rng = random.Random(seed)
        local = connect()
        try:
            completed = 0
            barrier.wait()
            while (stop_at is None or time.monotonic() < stop_at) and (iterations <= 0 or completed < iterations):
                key = rng.choice(keys)
                with history_lock:
                    history.append(
                        {
                            "type": "invoke",
                            "process": seed,
                            "f": "rmw",
                            "value": {"key": key},
                            "time": time.time_ns(),
                        }
                    )
                txn = local.pipeline(transaction=True)
                txn.get(key)
                txn.incrby(key, 1)
                old_raw, new_raw = txn.execute()
                old_value = redis_int(old_raw)
                new_value = int(new_raw)
                with history_lock:
                    history.append(
                        {
                            "type": "ok",
                            "process": seed,
                            "f": "rmw",
                            "value": {"key": key, "read": old_value, "write": new_value},
                            "time": time.time_ns(),
                        }
                    )
                with operation_count_lock:
                    operation_count += 1
                completed += 1
        except BaseException as exc:  # noqa: BLE001
            errors.append(exc)
            with history_lock:
                history.append(
                    {
                        "type": "fail",
                        "process": seed,
                        "f": "rmw",
                        "value": str(exc),
                        "time": time.time_ns(),
                    }
                )
        finally:
            local.close()

    threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(clients)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    if errors:
        raise errors[0]

    final_values = {key: redis_int(value) for key, value in zip(keys, client.mget(keys), strict=True)}
    anomalies = verify_rmw_history(history, final_values, keys)
    history_path.parent.mkdir(parents=True, exist_ok=True)
    history_doc = {
        "workload": "g4-rmw-isolation",
        "clients": clients,
        "duration_seconds": duration,
        "iterations_per_client": iterations,
        "keys": keys,
        "operations": operation_count,
        "final_values": final_values,
        "anomalies": anomalies,
        "history": history,
    }
    history_path.write_text(json.dumps(history_doc, indent=2, sort_keys=True) + "\n")
    client.delete(*keys)
    client.close()
    if anomalies:
        print(f"G4 serializable isolation failed anomalies={len(anomalies)} first={anomalies[0]} history={history_path}")
        raise SystemExit(1)
    print(f"G4 serializable isolation passed operations={operation_count} keys={len(keys)} clients={clients} history={history_path}")


def run_external_elle(elle_jar: str, history: str) -> None:
    try:
        result = subprocess.run(["java", "-jar", elle_jar, history], check=False)
    except FileNotFoundError:
        print("java is required for external Elle analysis")
        raise SystemExit(1) from None
    if result.returncode != 0:
        raise SystemExit(result.returncode)
    print(f"Elle isolation check passed history={history}")


def main() -> None:
    elle_jar = os.environ.get("ELLE_JAR", "third-party/redis/compat/elle.jar")
    history = os.environ.get("MAKO_G4_HISTORY")
    if history or os.environ.get("MAKO_G4_EXTERNAL_ELLE") == "1":
        if not Path(elle_jar).exists():
            na(f"missing Elle jar at {elle_jar}")
        if not history:
            na("missing MAKO_G4_HISTORY history file for Elle analysis")
        if not Path(history).exists():
            na(f"missing MAKO_G4_HISTORY file at {history}")
        run_external_elle(elle_jar, history)
        return
    run_rmw_isolation_check()


if __name__ == "__main__":
    main_guard(main)
