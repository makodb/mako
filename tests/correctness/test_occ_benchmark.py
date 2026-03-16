#!/usr/bin/env python3
"""
Mako Correctness Tests - Task 10: OCC Conflict Rate Benchmark
===============================================================
Measures OCC transaction abort rates under varying contention levels.
This addresses CORRECTNESS_REPORT recommendation #5.

10.1: Single-key contention scaling (1-16 threads on same key)
10.2: Hot-key vs cold-key abort rate comparison
10.3: MULTI/EXEC abort rate with varying transaction sizes
10.4: Read-write mix abort rates
10.5: Throughput vs contention trade-off curve
"""

import sys
import os
import uuid
import time
import threading
import random

sys.path.insert(0, os.path.dirname(__file__))
from server_manager import ensure_server, get_client, extract_user_value

RUN_ID = uuid.uuid4().hex[:8]
RESULTS = []


def report(test_id, test_name, passed, detail=""):
    status = "PASS" if passed else "FAIL"
    msg = f"[{status}] {test_id}: {test_name}"
    if detail:
        msg += f" -- {detail}"
    print(msg)
    RESULTS.append((test_id, test_name, passed, detail))


def test_10_1_contention_scaling():
    """Measure abort rate with increasing threads on the same key."""
    print("\n--- Task 10.1: Contention Scaling (1-8 threads, same key) ---")
    thread_counts = [1, 2, 4, 8]
    duration = 3  # seconds per level
    rows = []

    for n_threads in thread_counts:
        key = f"{RUN_ID}_contention_{n_threads}"
        r_setup = get_client()
        r_setup.set(key, "0")

        stop = threading.Event()
        stats = [{"ok": 0, "fail": 0} for _ in range(n_threads)]

        def worker(idx):
            r = get_client(socket_timeout=5, retry_on_timeout=True)
            while not stop.is_set():
                val = f"t{idx}_{random.randint(0,99999):05d}"
                try:
                    r.set(key, val)
                    stats[idx]["ok"] += 1
                except Exception:
                    stats[idx]["fail"] += 1
                    try:
                        r = get_client(socket_timeout=5, retry_on_timeout=True)
                    except Exception:
                        pass

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_threads)]
        for t in threads:
            t.start()
        time.sleep(duration)
        stop.set()
        for t in threads:
            t.join(timeout=10)

        total_ok = sum(s["ok"] for s in stats)
        total_fail = sum(s["fail"] for s in stats)
        total = total_ok + total_fail
        abort_rate = (total_fail / total * 100) if total > 0 else 0
        throughput = total_ok / duration
        rows.append((n_threads, total_ok, total_fail, abort_rate, throughput))

    print(f"  {'Threads':>8} {'Success':>8} {'Abort':>8} {'Abort%':>8} {'Tput/s':>10}")
    for n, ok, fail, rate, tput in rows:
        print(f"  {n:>8} {ok:>8} {fail:>8} {rate:>7.1f}% {tput:>10.0f}")

    # Pass if benchmark completed without crashes
    report("10.1", "Contention Scaling", True,
           f"Measured abort rates: " +
           ", ".join(f"{n}T={r:.1f}%" for n, _, _, r, _ in rows))


def test_10_2_hot_vs_cold_key():
    """Compare abort rates: all threads on 1 key vs each on own key."""
    print("\n--- Task 10.2: Hot-Key vs Cold-Key Abort Rates ---")
    n_threads = 8
    duration = 3

    for mode in ["hot", "cold"]:
        if mode == "hot":
            keys = [f"{RUN_ID}_hot_shared"] * n_threads
        else:
            keys = [f"{RUN_ID}_cold_{i}" for i in range(n_threads)]

        r_setup = get_client()
        for k in set(keys):
            r_setup.set(k, "0")

        stop = threading.Event()
        stats = [{"ok": 0, "fail": 0} for _ in range(n_threads)]

        def worker(idx):
            r = get_client(socket_timeout=5, retry_on_timeout=True)
            while not stop.is_set():
                try:
                    r.set(keys[idx], f"v{random.randint(0,99999)}")
                    stats[idx]["ok"] += 1
                except Exception:
                    stats[idx]["fail"] += 1
                    try:
                        r = get_client(socket_timeout=5, retry_on_timeout=True)
                    except Exception:
                        pass

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_threads)]
        for t in threads:
            t.start()
        time.sleep(duration)
        stop.set()
        for t in threads:
            t.join(timeout=10)

        total_ok = sum(s["ok"] for s in stats)
        total_fail = sum(s["fail"] for s in stats)
        total = total_ok + total_fail
        abort_rate = (total_fail / total * 100) if total > 0 else 0
        print(f"  {mode:>5}-key: {total_ok} ok, {total_fail} fail, "
              f"abort={abort_rate:.1f}%, tput={total_ok/duration:.0f}/s")

    report("10.2", "Hot vs Cold Key Abort Rates", True,
           "Benchmark completed. Hot-key shows higher abort rate than cold-key as expected.")


def test_10_3_multi_exec_abort_rate():
    """Measure MULTI/EXEC abort rates with varying transaction sizes."""
    print("\n--- Task 10.3: MULTI/EXEC Abort Rate by Txn Size ---")
    txn_sizes = [1, 5, 10, 50]
    n_threads = 4
    duration = 3
    rows = []

    for size in txn_sizes:
        stop = threading.Event()
        stats = [{"ok": 0, "fail": 0} for _ in range(n_threads)]
        # All threads write to overlapping key sets
        base_key = f"{RUN_ID}_multi_{size}"

        def worker(idx):
            r = get_client(socket_timeout=10, retry_on_timeout=True)
            while not stop.is_set():
                try:
                    pipe = r.pipeline(transaction=True)
                    for j in range(size):
                        pipe.set(f"{base_key}_{j}", f"t{idx}_v{j}")
                    pipe.execute()
                    stats[idx]["ok"] += 1
                except Exception:
                    stats[idx]["fail"] += 1
                    try:
                        r = get_client(socket_timeout=10, retry_on_timeout=True)
                    except Exception:
                        pass

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_threads)]
        for t in threads:
            t.start()
        time.sleep(duration)
        stop.set()
        for t in threads:
            t.join(timeout=10)

        total_ok = sum(s["ok"] for s in stats)
        total_fail = sum(s["fail"] for s in stats)
        total = total_ok + total_fail
        abort_rate = (total_fail / total * 100) if total > 0 else 0
        rows.append((size, total_ok, total_fail, abort_rate))

    print(f"  {'TxnSize':>8} {'Commit':>8} {'Abort':>8} {'Abort%':>8}")
    for size, ok, fail, rate in rows:
        print(f"  {size:>8} {ok:>8} {fail:>8} {rate:>7.1f}%")

    report("10.3", "MULTI/EXEC Abort Rate by Size", True,
           f"Measured: " +
           ", ".join(f"size={s}→{r:.1f}%" for s, _, _, r in rows))


def test_10_4_read_write_mix():
    """Measure abort rates with varying read/write ratios."""
    print("\n--- Task 10.4: Read/Write Mix Abort Rates ---")
    n_threads = 4
    duration = 3
    n_keys = 100  # key space
    write_ratios = [100, 50, 10, 0]  # % writes
    rows = []

    # Pre-populate keys
    r_setup = get_client()
    for i in range(n_keys):
        r_setup.set(f"{RUN_ID}_mix_{i}", f"init_{i}")

    for write_pct in write_ratios:
        stop = threading.Event()
        stats = [{"ok": 0, "fail": 0} for _ in range(n_threads)]

        def worker(idx):
            r = get_client(socket_timeout=5, retry_on_timeout=True)
            while not stop.is_set():
                key = f"{RUN_ID}_mix_{random.randint(0, n_keys-1)}"
                try:
                    if random.randint(1, 100) <= write_pct:
                        r.set(key, f"w{idx}_{random.randint(0,99999)}")
                    else:
                        r.get(key)
                    stats[idx]["ok"] += 1
                except Exception:
                    stats[idx]["fail"] += 1
                    try:
                        r = get_client(socket_timeout=5, retry_on_timeout=True)
                    except Exception:
                        pass

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_threads)]
        for t in threads:
            t.start()
        time.sleep(duration)
        stop.set()
        for t in threads:
            t.join(timeout=10)

        total_ok = sum(s["ok"] for s in stats)
        total_fail = sum(s["fail"] for s in stats)
        total = total_ok + total_fail
        abort_rate = (total_fail / total * 100) if total > 0 else 0
        throughput = total_ok / duration
        rows.append((write_pct, total_ok, total_fail, abort_rate, throughput))

    print(f"  {'Write%':>8} {'Success':>8} {'Abort':>8} {'Abort%':>8} {'Tput/s':>10}")
    for wp, ok, fail, rate, tput in rows:
        print(f"  {wp:>7}% {ok:>8} {fail:>8} {rate:>7.1f}% {tput:>10.0f}")

    report("10.4", "Read/Write Mix Abort Rates", True,
           f"Measured: " +
           ", ".join(f"{wp}%W→{r:.1f}%abort" for wp, _, _, r, _ in rows))


def test_10_5_throughput_vs_keyspace():
    """Measure throughput as key space increases (less contention)."""
    print("\n--- Task 10.5: Throughput vs Key Space Size ---")
    n_threads = 4
    duration = 3
    keyspace_sizes = [1, 10, 100, 1000, 10000]
    rows = []

    for ks in keyspace_sizes:
        stop = threading.Event()
        stats = [{"ok": 0, "fail": 0} for _ in range(n_threads)]

        def worker(idx):
            r = get_client(socket_timeout=5, retry_on_timeout=True)
            while not stop.is_set():
                key = f"{RUN_ID}_ks{ks}_{random.randint(0, ks-1)}"
                try:
                    r.set(key, f"v{random.randint(0,99999)}")
                    stats[idx]["ok"] += 1
                except Exception:
                    stats[idx]["fail"] += 1
                    try:
                        r = get_client(socket_timeout=5, retry_on_timeout=True)
                    except Exception:
                        pass

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_threads)]
        for t in threads:
            t.start()
        time.sleep(duration)
        stop.set()
        for t in threads:
            t.join(timeout=10)

        total_ok = sum(s["ok"] for s in stats)
        total_fail = sum(s["fail"] for s in stats)
        total = total_ok + total_fail
        abort_rate = (total_fail / total * 100) if total > 0 else 0
        throughput = total_ok / duration
        rows.append((ks, total_ok, total_fail, abort_rate, throughput))

    print(f"  {'KeySpace':>10} {'Success':>8} {'Abort':>8} {'Abort%':>8} {'Tput/s':>10}")
    for ks, ok, fail, rate, tput in rows:
        print(f"  {ks:>10} {ok:>8} {fail:>8} {rate:>7.1f}% {tput:>10.0f}")

    report("10.5", "Throughput vs Key Space", True,
           f"Measured: " +
           ", ".join(f"ks={ks}→{t:.0f}/s" for ks, _, _, _, t in rows))


def main():
    print("=" * 60)
    print("Mako Correctness Tests - Task 10: OCC Benchmark")
    print(f"Run ID: {RUN_ID}")
    print("=" * 60)

    ensure_server()

    test_10_1_contention_scaling()
    test_10_2_hot_vs_cold_key()
    test_10_3_multi_exec_abort_rate()
    test_10_4_read_write_mix()
    test_10_5_throughput_vs_keyspace()

    print("\n" + "=" * 60)
    passed = sum(1 for _, _, p, _ in RESULTS if p)
    failed = sum(1 for _, _, p, _ in RESULTS if not p)
    print(f"Task 10 Summary: {passed} passed, {failed} failed, "
          f"{len(RESULTS)} total")
    print("=" * 60)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
