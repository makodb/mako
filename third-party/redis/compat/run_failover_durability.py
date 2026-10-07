#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import time
from pathlib import Path

from harness_common import RedisTarget, connect, env_int, fail, main_guard, require_env


ROOT_DIR = Path(__file__).resolve().parents[3]

# Reference workload:
# - Jepsen's Redis-Raft suite validates a Redis-compatible Raft system under
#   process crashes, pauses, partitions, clock skew, and membership changes:
#   https://github.com/jepsen-io/redis
# - The RedisRaft consistency claim is that acknowledged writes are committed
#   and never lost:
#   https://redis.io/blog/redisraft-new-strong-consistency-deployment-option/
# - The reusable oracle for Mako is acknowledged-write survival after a fault.


def run_shell(command: str) -> None:
    subprocess.run(command, shell=True, check=True)


def main() -> None:
    if os.environ.get("MAKO_G3_USE_LOCAL_RESTART_FIXTURE") == "1":
        if os.environ.get("MAKO_G3_ALLOW_RESTART_SMOKE") != "1":
            from harness_common import na
            na("local restart fixture is smoke only; set MAKO_G3_ALLOW_RESTART_SMOKE=1 to run it")
        script = ROOT_DIR / "third-party" / "redis" / "compat" / "fixtures" / "makocon_g3_local_restart.sh"
        os.environ.setdefault("MAKO_G3_START_CMD", f"bash {script} start")
        os.environ.setdefault("MAKO_G3_KILL_CMD", f"bash {script} kill")
        os.environ.setdefault("MAKO_G3_RECOVER_CMD", f"bash {script} recover")

    if os.environ.get("MAKO_G3_USE_REPLICATED_FIXTURE") == "1":
        script = ROOT_DIR / "third-party" / "redis" / "compat" / "fixtures" / "makocon_g3_replicated.sh"
        os.environ.setdefault("MAKO_G3_START_CMD", f"bash {script} start")
        os.environ.setdefault("MAKO_G3_KILL_CMD", f"bash {script} kill")
        os.environ.setdefault("MAKO_G3_RECOVER_CMD", f"bash {script} recover")
        os.environ.setdefault("MAKO_G3_RECOVER_HOST", "127.0.0.1")
        os.environ.setdefault("MAKO_G3_RECOVER_PORT", "6393")

    start_cmd = require_env("MAKO_G3_START_CMD")
    kill_cmd = require_env("MAKO_G3_KILL_CMD")
    recover_cmd = require_env("MAKO_G3_RECOVER_CMD")
    count = env_int("MAKO_G3_WRITES", 100)
    prefix = f"g3:failover:{int(time.time() * 1000)}"
    recovery_target = RedisTarget(
        host=os.environ.get("MAKO_G3_RECOVER_HOST", os.environ.get("MAKO_HOST", "127.0.0.1")),
        port=env_int("MAKO_G3_RECOVER_PORT", env_int("MAKO_PORT", 6380)),
    )

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
            client = connect(recovery_target)

    client.close()
    verifier = connect(recovery_target)
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
