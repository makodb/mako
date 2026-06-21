#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import time
from pathlib import Path

from harness_common import connect, env_int, main_guard, na


def run_shell(command: str) -> None:
    subprocess.run(command, shell=True, check=True)


def main() -> None:
    stop_cmd = os.environ.get("MAKO_RESTART_STOP_CMD")
    start_cmd = os.environ.get("MAKO_RESTART_START_CMD")
    if os.environ.get("MAKO_RESTART_USE_LOCAL_FIXTURE") == "1":
        fixture = Path("third-party/redis/compat/fixtures/makocon_local.sh")
        stop_cmd = stop_cmd or f"bash {fixture} stop"
        start_cmd = start_cmd or f"bash {fixture} start"
    if not stop_cmd or not start_cmd:
        na("missing MAKO_RESTART_STOP_CMD or MAKO_RESTART_START_CMD")

    client = connect()
    prefix = f"restart:{int(time.time() * 1000)}"
    count = env_int("MAKO_RESTART_KEYS", 100)
    keys = [f"{prefix}:{i}" for i in range(count)]
    pipe = client.pipeline(transaction=False)
    for i, key in enumerate(keys):
        pipe.set(key, f"value:{i}")
    pipe.execute()
    client.close()

    run_shell(stop_cmd)
    run_shell(start_cmd)
    time.sleep(float(os.environ.get("MAKO_RESTART_WAIT_S", "1.0")))

    client = connect()
    values = client.mget(keys)
    missing = sum(1 for value in values if value is None)
    if missing:
        print(f"restart durability failed missing={missing} total={count}")
        raise SystemExit(1)
    client.delete(*keys)
    print(f"restart durability preserved {count} keys")


if __name__ == "__main__":
    main_guard(main)
