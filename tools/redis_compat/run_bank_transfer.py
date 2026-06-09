#!/usr/bin/env python3
from __future__ import annotations

import os
import random
import threading
import time

from harness_common import connect, env_int, main_guard, na


def run_local_bank_transfer() -> None:
    client = connect()
    prefix = f"g2:bank:{int(time.time() * 1000)}"
    accounts = env_int("MAKO_G2_ACCOUNTS", 20)
    clients = env_int("MAKO_G2_CLIENTS", 4)
    iterations = env_int("MAKO_G2_ITERATIONS", 200)
    initial_balance = env_int("MAKO_G2_INITIAL_BALANCE", 1000)
    keys = [f"{prefix}:acct:{i}" for i in range(accounts)]
    total = accounts * initial_balance

    pipe = client.pipeline(transaction=False)
    for key in keys:
        pipe.set(key, initial_balance)
    pipe.execute()

    errors: list[BaseException] = []

    def worker(seed: int) -> None:
        rng = random.Random(seed)
        local = connect()
        try:
            for _ in range(iterations):
                src, dst = rng.sample(keys, 2)
                amount = rng.randint(1, 7)
                txn = local.pipeline(transaction=True)
                txn.decrby(src, amount)
                txn.incrby(dst, amount)
                txn.execute()
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
    print(f"local bank transfer invariant preserved transfers={clients * iterations} total={observed}")


def main() -> None:
    if os.environ.get("MAKO_G2_MULTI_SHARD") != "1" and os.environ.get("MAKO_G2_ALLOW_SINGLE_SHARD") != "1":
        na("requires MAKO_G2_MULTI_SHARD=1 fixture; set MAKO_G2_ALLOW_SINGLE_SHARD=1 for local smoke")
    run_local_bank_transfer()


if __name__ == "__main__":
    main_guard(main)
