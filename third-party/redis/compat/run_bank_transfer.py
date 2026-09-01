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


ROOT_DIR = Path(__file__).resolve().parents[3]

# Reference workload:
# - CockroachDB runs roachtests named `copy/bank/rows=...,nodes=...,txn=true`
#   for transactional bank-table validation on distributed clusters.
#   Example public issue: https://github.com/cockroachdb/cockroach/issues/134402
# - The reusable part for Mako is the oracle, not the SQL implementation:
#   concurrent transfers must preserve the total account balance exactly.


def run_fixture(action: str) -> None:
    script = ROOT_DIR / "third-party" / "redis" / "compat" / "fixtures" / "makocon_multishard.sh"
    subprocess.run(["bash", str(script), action], check=True)


def run_bank_transfer_once(client: redis.Redis, run_index: int) -> tuple[int, int]:
    prefix = f"g2:bank:{int(time.time() * 1000)}:{run_index}"
    accounts = env_int("MAKO_G2_ACCOUNTS", 100)
    clients = env_int("MAKO_G2_CLIENTS", 16)
    iterations = env_int("MAKO_G2_ITERATIONS", 0)
    duration = env_int("MAKO_G2_DURATION", 30)
    if iterations <= 0 and duration <= 0:
        from harness_common import fail
        fail("MAKO_G2_ITERATIONS or MAKO_G2_DURATION must be positive")
    max_retries = env_int("MAKO_G2_TRANSFER_RETRIES", 1000)
    initial_balance = env_int("MAKO_G2_INITIAL_BALANCE", 10000)
    max_amount = env_int("MAKO_G2_MAX_AMOUNT", 100)
    keys = [f"{prefix}:acct:{i}" for i in range(accounts)]
    total = accounts * initial_balance

    pipe = client.pipeline(transaction=False)
    for key in keys:
        pipe.set(key, initial_balance)
    pipe.execute()

    errors: list[BaseException] = []
    retry_count = 0
    transfer_count = 0
    retry_lock = threading.Lock()
    transfer_lock = threading.Lock()
    barrier = threading.Barrier(clients)
    stop_at = time.monotonic() + duration if duration > 0 else None

    def worker(seed: int) -> None:
        nonlocal retry_count, transfer_count
        rng = random.Random((run_index + 1) * 1_000_003 + seed)
        local = connect()
        try:
            completed = 0
            barrier.wait()
            while (stop_at is None or time.monotonic() < stop_at) and (
                iterations <= 0 or completed < iterations
            ):
                src, dst = rng.sample(keys, 2)
                amount = rng.randint(1, max_amount)
                for attempt in range(max_retries + 1):
                    try:
                        txn = local.pipeline(transaction=True)
                        txn.decrby(src, amount)
                        txn.incrby(dst, amount)
                        txn.execute()
                        with transfer_lock:
                            transfer_count += 1
                        completed += 1
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
    client.delete(*keys)
    if observed != total:
        print(f"bank transfer invariant failed run={run_index + 1} observed={observed} expected={total}")
        raise SystemExit(1)
    return transfer_count, retry_count


def run_local_bank_transfer() -> None:
    client = connect()
    runs = env_int("MAKO_G2_RUNS", 3)
    total_transfers = 0
    total_retries = 0
    try:
        for run_index in range(runs):
            transfers, retries = run_bank_transfer_once(client, run_index)
            total_transfers += transfers
            total_retries += retries
    finally:
        client.close()
    print(f"bank transfer invariant preserved runs={runs} transfers={total_transfers} retries={total_retries}")


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
