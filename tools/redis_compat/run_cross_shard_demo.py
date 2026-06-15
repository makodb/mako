#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import time
from pathlib import Path

import redis

from harness_common import main_guard, na


ROOT_DIR = Path(__file__).resolve().parents[2]
DOC_PATH = ROOT_DIR / "docs" / "cross_shard_atomicity_demo.md"


def run_shell(command: list[str], env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=ROOT_DIR,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def redis_cluster_rejection() -> str:
    host = os.environ.get("REDIS_CLUSTER_HOST", "127.0.0.1")
    port = int(os.environ.get("REDIS_CLUSTER_PORT", "7000"))
    try:
        client = redis.RedisCluster(host=host, port=port, decode_responses=True)
        pipe = client.pipeline(transaction=True)
        pipe.decrby("g2:redis:acct:1", 1)
        pipe.incrby("g2:redis:acct:2", 1)
        pipe.execute()
    except Exception as exc:  # noqa: BLE001 - exact class varies across redis-py versions
        name = type(exc).__name__
        message = str(exc)
        if name == "CrossSlotTransactionError" or "same slot" in message or "CROSSSLOT" in message.upper():
            return f"{name}: {message}"
        raise
    raise RuntimeError("Redis Cluster unexpectedly accepted a cross-slot MULTI transaction")


def run_mako_bank_transfer() -> str:
    env = os.environ.copy()
    env.setdefault("MAKO_G2_USE_LOCAL_FIXTURE", "1")
    env.setdefault("MAKO_G2_ACCOUNTS", "20")
    env.setdefault("MAKO_G2_CLIENTS", "4")
    env.setdefault("MAKO_G2_ITERATIONS", "100")
    result = run_shell(["python3", "tools/redis_compat/run_bank_transfer.py"], env=env)
    if result.returncode != 0:
        raise RuntimeError(result.stdout.strip())
    return result.stdout.strip().splitlines()[-1]


def update_doc(redis_result: str, mako_result: str) -> None:
    stamp = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    DOC_PATH.write_text(
        f"""# Cross-Shard Atomicity Demo Status

The Phase 12 side-by-side demo is now executable with:

```bash
tools/redis_compat/run_cross_shard_demo.py
```

## Latest Captured Result

Captured: {stamp}

| System | Result |
|---|---|
| Redis Cluster | Rejects cross-slot transactional transfer: `{redis_result}` |
| Mako Redis fixture | Preserves bank invariant: `{mako_result}` |

## Scope

The Mako side uses `tools/redis_compat/fixtures/makocon_multishard.sh`, which
starts one Redis-facing `makoCon` process with `MAKO_NUM_SHARDS=3` and
`MAKO_LOCAL_SHARDS=0,1,2`. That exercises Redis transactions over Mako's
sharded table routing in one process.

This is not a replicated failover fixture. A future distributed-service demo
should keep backing shard servers alive as separate processes and expose a
Redis endpoint over that deployment.

## Local Smoke

Single-shard smoke remains available:

```bash
MAKO_G2_ALLOW_SINGLE_SHARD=1 python3 tools/redis_compat/run_bank_transfer.py
```
"""
    )


def main() -> None:
    if os.environ.get("G2_DEMO_ASSUME_REDIS_CLUSTER") != "1":
        cluster_script = ROOT_DIR / "tools" / "redis_compat" / "fixtures" / "redis_cluster.sh"
        if os.environ.get("G2_DEMO_START_REDIS_CLUSTER") != "1":
            na("set G2_DEMO_START_REDIS_CLUSTER=1 or G2_DEMO_ASSUME_REDIS_CLUSTER=1")
        start = run_shell(["bash", str(cluster_script), "start"])
        if start.returncode != 0:
            raise RuntimeError(start.stdout.strip())
    else:
        cluster_script = None

    try:
        redis_result = redis_cluster_rejection()
        mako_result = run_mako_bank_transfer()
        update_doc(redis_result, mako_result)
        print(f"cross-shard demo captured: Redis Cluster rejected; {mako_result}")
    finally:
        if os.environ.get("G2_DEMO_START_REDIS_CLUSTER") == "1" and cluster_script is not None:
            run_shell(["bash", str(cluster_script), "stop"])


if __name__ == "__main__":
    main_guard(main)
