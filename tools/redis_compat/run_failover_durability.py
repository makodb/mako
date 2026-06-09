#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import time

from harness_common import connect, env_int, fail, main_guard, require_env


def run_shell(command: str) -> None:
    subprocess.run(command, shell=True, check=True)


def main() -> None:
    start_cmd = require_env("MAKO_G3_START_CMD")
    kill_cmd = require_env("MAKO_G3_KILL_CMD")
    recover_cmd = require_env("MAKO_G3_RECOVER_CMD")
    count = env_int("MAKO_G3_WRITES", 100)
    prefix = f"g3:failover:{int(time.time() * 1000)}"

    run_shell(start_cmd)
    client = connect()
    acked: list[tuple[str, bytes]] = []
    for i in range(count):
        key = f"{prefix}:{i}"
        value = f"value:{i}".encode()
        if client.set(key, value):
            acked.append((key, value))
        if i == count // 2:
            run_shell(kill_cmd)
            time.sleep(float(os.environ.get("MAKO_G3_FAULT_WAIT_S", "0.5")))
            run_shell(recover_cmd)
            time.sleep(float(os.environ.get("MAKO_G3_RECOVER_WAIT_S", "1.0")))
            client.close()
            client = connect()

    client.close()
    verifier = connect()
    missing: list[str] = []
    wrong: list[str] = []
    for key, expected in acked:
        observed = verifier.get(key)
        if observed is None:
            missing.append(key)
        elif observed != expected:
            wrong.append(key)
    if acked:
        verifier.delete(*(key for key, _ in acked))
    verifier.close()
    if missing or wrong:
        fail(f"G3 durability failed acked={len(acked)} missing={len(missing)} wrong={len(wrong)}")
    print(f"G3 durability preserved acked={len(acked)} writes")


if __name__ == "__main__":
    main_guard(main)
