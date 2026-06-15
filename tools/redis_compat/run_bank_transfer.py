#!/usr/bin/env python3
from __future__ import annotations

import os
import random
import subprocess
import threading
import time
from pathlib import Path

import redis

from harness_common import connect, env_int, main_guard, na


ROOT_DIR = Path(__file__).resolve().parents[2]

# Reference workload:
# - CockroachDB runs roachtests named `copy/bank/rows=...,nodes=...,txn=true`
#   for transactional bank-table validation on distributed clusters.
#   Example public issue: https://github.com/cockroachdb/cockroach/issues/134402
# - The reusable part for Mako is the oracle, not the SQL implementation:
#   concurrent transfers must preserve the total account balance exactly.


def run_fixture(action: str) -> None:
    script = ROOT_DIR / "tools" / "redis_compat" / "fixtures" / "makocon_multishard.sh"
    subprocess.run(["bash", str(script), action], check=True)


def run_local_bank_transfer() -> None:
    client = connect()
    prefix = f"g2:bank:{int(time.time() * 1000)}"
    accounts = env_int("MAKO_G2_ACCOUNTS", 20)
    clients = env_int("MAKO_G2_CLIENTS", 4)
    iterations = env_int("MAKO_G2_ITERATIONS", 200)
    max_retries = env_int("MAKO_G2_TRANSFER_RETRIES", 1000)
    initial_balance = env_int("MAKO_G2_INITIAL_BALANCE", 1000)
    keys = [f"{prefix}:acct:{i}" for i in range(accounts)]
    total = accounts * initial_balance

    pipe = client.pipeline(transaction=False)
    for key in keys:
        pipe.set(key, initial_balance)
    pipe.execute()

    errors: list[BaseException] = []
    retry_count = 0
    retry_lock = threading.Lock()

    def worker(seed: int) -> None:
        nonlocal retry_count
        rng = random.Random(seed)
        local = connect()
        try:
            for _ in range(iterations):
                src, dst = rng.sample(keys, 2)
                amount = rng.randint(1, 7)
                for attempt in range(max_retries + 1):
                    try:
                        txn = local.pipeline(transaction=True)
                        txn.decrby(src, amount)
                        txn.incrby(dst, amount)
                        txn.execute()
                        break
                    except redis.WatchError:
                        if attempt == max_retries:
                            raise
                        with retry_lock:
                            retry_count += 1
                        time.sleep(0.001 * (attempt + 1))
        except BaseException as exc:  # noqa: BLE001
            errors.append(exc)
        finally:
            local.close()

    threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(clients)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    if errors:
        raise errors[0]

    balances = [int(value) for value in client.mget(keys)]
    observed = sum(balances)
    if observed != total:
        print(f"bank transfer invariant failed: observed={observed} expected={total}")
        raise SystemExit(1)

    client.delete(*keys)
    print(f"bank transfer invariant preserved transfers={clients * iterations} total={observed} retries={retry_count}")


def main() -> None:
    started_fixture = False
    if os.environ.get("MAKO_G2_USE_LOCAL_FIXTURE") == "1":
        run_fixture("start")
        started_fixture = True
        os.environ["MAKO_G2_MULTI_SHARD"] = "1"

    if os.environ.get("MAKO_G2_MULTI_SHARD") != "1" and os.environ.get("MAKO_G2_ALLOW_SINGLE_SHARD") != "1":
        na("requires MAKO_G2_MULTI_SHARD=1 fixture; set MAKO_G2_ALLOW_SINGLE_SHARD=1 for local smoke")
    try:
        run_local_bank_transfer()
    finally:
        if started_fixture and os.environ.get("MAKO_G2_KEEP_FIXTURE") != "1":
            run_fixture("stop")


if __name__ == "__main__":
    main_guard(main)
