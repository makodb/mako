#!/usr/bin/env python3
from __future__ import annotations

import os
import json
import random
import subprocess
import threading
import time
from pathlib import Path

from harness_common import connect, env_int, main_guard, na


def run_builtin_rmw_smoke() -> None:
    client = connect()
    prefix = f"g4:rmw:{int(time.time() * 1000)}"
    keys = [f"{prefix}:{i}" for i in range(env_int("MAKO_G4_KEYS", 10))]
    clients = env_int("MAKO_G4_CLIENTS", 8)
    iterations = env_int("MAKO_G4_ITERATIONS", 100)
    history_path = Path(os.environ.get("MAKO_G4_HISTORY_OUT", "tools/redis_compat/g4_history.json"))
    history_lock = threading.Lock()
    history: list[dict[str, object]] = []
    for key in keys:
        client.set(key, 0)

    errors: list[BaseException] = []

    def worker(seed: int) -> None:
        rng = random.Random(seed)
        local = connect()
        try:
            for _ in range(iterations):
                key = rng.choice(keys)
                txn = local.pipeline(transaction=True)
                txn.incrby(key, 1)
                txn.execute()
                with history_lock:
                    history.append(
                        {
                            "type": "ok",
                            "process": seed,
                            "f": "txn",
                            "value": [["append", key, 1]],
                            "time": time.time_ns(),
                        }
                    )
        except BaseException as exc:  # noqa: BLE001
            errors.append(exc)
            with history_lock:
                history.append(
                    {
                        "type": "fail",
                        "process": seed,
                        "f": "txn",
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

    values = [int(value) for value in client.mget(keys)]
    observed = sum(values)
    expected = clients * iterations
    history_path.parent.mkdir(parents=True, exist_ok=True)
    history_path.write_text(json.dumps(history, indent=2, sort_keys=True) + "\n")
    client.delete(*keys)
    client.close()
    if observed != expected:
        print(f"G4 built-in RMW smoke failed observed={observed} expected={expected} history={history_path}")
        raise SystemExit(1)
    print(f"G4 built-in RMW smoke passed operations={expected} history={history_path}")


def main() -> None:
    elle_jar = os.environ.get("ELLE_JAR", "tools/redis_compat/elle.jar")
    if not Path(elle_jar).exists():
        if os.environ.get("MAKO_G4_ALLOW_BUILTIN") == "1":
            run_builtin_rmw_smoke()
            return
        na(f"missing Elle jar at {elle_jar}")
    history = os.environ.get("MAKO_G4_HISTORY")
    if not history:
        na("missing MAKO_G4_HISTORY history file for Elle analysis")
    if not Path(history).exists():
        na(f"missing MAKO_G4_HISTORY file at {history}")
    result = subprocess.run(["java", "-jar", elle_jar, history], check=False)
    if result.returncode != 0:
        raise SystemExit(result.returncode)
    print(f"Elle isolation check passed history={history}")


if __name__ == "__main__":
    main_guard(main)
