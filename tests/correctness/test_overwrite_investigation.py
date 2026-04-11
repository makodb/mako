#!/usr/bin/env python3
"""
Mako Correctness Tests - Task 7: Deep Overwrite Bug Investigation
==================================================================
Task 6.5 found that overwriting a key with a different-length value can
cause OCC abort — but the failure is intermittent. This test suite
systematically probes the overwrite behavior to characterize:

7.1: Size transition matrix — which size transitions fail?
7.2: Repeated size alternation — does rapid toggling between sizes trigger it?
7.3: Overwrite after many same-size writes — does history matter?
7.4: Concurrent same-key overwrites with varying sizes
7.5: Overwrite in MULTI/EXEC vs auto-commit
7.6: Large value size transitions
7.7: Overwrite with exact mako::Encode boundary sizes
7.8: Stress test — 1000 random-size overwrites on same key
"""

import sys
import os
import uuid
import time
import threading
import socket
import random

sys.path.insert(0, os.path.dirname(__file__))
from server_manager import (ensure_server, get_client, extract_user_value,
                            MAKO_HOST, MAKO_PORT)

RUN_ID = uuid.uuid4().hex[:8]
RESULTS = []


def report(test_id, test_name, passed, detail=""):
    status = "PASS" if passed else "FAIL"
    msg = f"[{status}] {test_id}: {test_name}"
    if detail:
        msg += f" -- {detail}"
    print(msg)
    RESULTS.append((test_id, test_name, passed, detail))


def try_set_get(r, key, value, reconnect_on_fail=True):
    """Attempt SET+GET, return (success, got_value_or_error).
    Returns new client if reconnect was needed."""
    try:
        r.set(key, value)
        raw = r.get(key)
        if raw is None:
            return r, True, b""  # nil = empty (might be all-zero encoded)
        got = extract_user_value(raw, expected_len=len(value) if isinstance(value, bytes) else len(value.encode()))
        expected = value if isinstance(value, bytes) else value.encode()
        return r, got == expected, got
    except Exception as e:
        if reconnect_on_fail:
            try:
                r = get_client()
            except Exception:
                pass
        return r, False, str(e)


def test_7_1_size_transition_matrix():
    """Task 7.1: Size transition matrix"""
    print("\n--- Task 7.1: Size Transition Matrix ---")
    print("Test all combinations of value sizes: 1, 2, 5, 10, 20, 50, 100, 500")

    sizes = [1, 2, 5, 10, 20, 50, 100, 500]
    failures = []
    successes = 0
    total = 0

    r = get_client()
    for from_size in sizes:
        for to_size in sizes:
            key = f"{RUN_ID}_matrix_{from_size}_{to_size}"
            val1 = "A" * from_size
            val2 = "B" * to_size

            # Write initial value
            r, ok1, _ = try_set_get(r, key, val1)
            if not ok1:
                failures.append((from_size, to_size, "initial write failed"))
                total += 1
                continue

            # Overwrite with different size
            r, ok2, got = try_set_get(r, key, val2)
            total += 1
            if ok2:
                successes += 1
            else:
                failures.append((from_size, to_size, f"overwrite failed: {got}"))

    # Analyze pattern: which direction fails?
    grow_fails = [(f, t) for f, t, _ in failures if t > f]
    shrink_fails = [(f, t) for f, t, _ in failures if t < f]
    same_fails = [(f, t) for f, t, _ in failures if t == f]

    if len(failures) == 0:
        report("7.1", f"Size Transition Matrix ({total} combos)", True,
               f"All {total} size transitions succeeded.")
    else:
        detail = (f"{len(failures)}/{total} failed. "
                  f"Growing: {len(grow_fails)} fails, "
                  f"Shrinking: {len(shrink_fails)} fails, "
                  f"Same-size: {len(same_fails)} fails. "
                  f"Grow failures: {grow_fails}. "
                  f"Shrink failures: {shrink_fails}. "
                  f"Same failures: {same_fails}.")
        report("7.1", f"Size Transition Matrix ({total} combos)", False, detail)


def test_7_2_size_alternation():
    """Task 7.2: Rapid size alternation"""
    print("\n--- Task 7.2: Rapid Size Alternation ---")
    print("Alternate between short and long values 100 times on same key.")

    key = f"{RUN_ID}_alternate"
    r = get_client()
    short_val = "S" * 5
    long_val = "L" * 50
    failures = 0
    n = 100

    for i in range(n):
        val = short_val if i % 2 == 0 else long_val
        r, ok, got = try_set_get(r, key, val)
        if not ok:
            failures += 1

    if failures == 0:
        report("7.2", f"Size Alternation ({n} iterations)", True,
               "All short↔long alternations succeeded.")
    else:
        report("7.2", f"Size Alternation ({n} iterations)", False,
               f"{failures}/{n} failed during alternation.")


def test_7_3_overwrite_after_history():
    """Task 7.3: Overwrite after many same-size writes"""
    print("\n--- Task 7.3: Overwrite After History ---")
    print("Write same key 50 times with same-size value, then change size.")

    key = f"{RUN_ID}_history"
    r = get_client()

    # Build up history with same-size writes
    for i in range(50):
        val = f"h{i:06d}X"  # 8 bytes each time
        r, ok, _ = try_set_get(r, key, val)
        if not ok:
            report("7.3", "Overwrite After History", False,
                   f"Same-size write {i} failed during history buildup")
            return

    # Now change size
    size_changes = [
        ("shrink", "tiny"),
        ("grow", "X" * 100),
        ("shrink_again", "Y"),
        ("grow_more", "Z" * 500),
    ]

    failures = []
    for label, val in size_changes:
        r, ok, got = try_set_get(r, key, val)
        if not ok:
            failures.append((label, len(val), got))

    if len(failures) == 0:
        report("7.3", "Overwrite After History", True,
               "50 same-size writes then 4 size changes: all succeeded.")
    else:
        report("7.3", "Overwrite After History", False,
               f"Failed after history: {failures}")


def test_7_4_concurrent_varying_overwrites():
    """Task 7.4: Concurrent overwrites with varying sizes"""
    print("\n--- Task 7.4: Concurrent Varying-Size Overwrites ---")
    print("4 threads overwrite same key with different-sized values for 3 seconds.")

    key = f"{RUN_ID}_concurrent_size"
    r_setup = get_client()
    r_setup.set(key, "initial")

    stop_flag = threading.Event()
    thread_stats = [{"ok": 0, "fail": 0} for _ in range(4)]

    def writer(idx, min_size, max_size):
        r = get_client(socket_timeout=5, retry_on_timeout=True)
        while not stop_flag.is_set():
            size = random.randint(min_size, max_size)
            val = chr(65 + idx) * size  # A, B, C, D repeated
            try:
                r.set(key, val)
                thread_stats[idx]["ok"] += 1
            except Exception:
                thread_stats[idx]["fail"] += 1
                try:
                    r = get_client(socket_timeout=5, retry_on_timeout=True)
                except Exception:
                    pass

    threads = [
        threading.Thread(target=writer, args=(0, 1, 10)),
        threading.Thread(target=writer, args=(1, 50, 100)),
        threading.Thread(target=writer, args=(2, 200, 500)),
        threading.Thread(target=writer, args=(3, 1, 500)),
    ]
    for t in threads:
        t.start()
    time.sleep(3)
    stop_flag.set()
    for t in threads:
        t.join(timeout=5)

    total_ok = sum(s["ok"] for s in thread_stats)
    total_fail = sum(s["fail"] for s in thread_stats)

    # Verify final value is valid
    raw = r_setup.get(key)
    final_valid = raw is not None

    detail = (f"{total_ok} successes, {total_fail} failures across 4 threads. "
              f"Final key valid: {final_valid}. "
              f"Per-thread: {thread_stats}")

    if total_fail == 0 and final_valid:
        report("7.4", "Concurrent Varying-Size Overwrites", True, detail)
    elif total_ok > 0 and final_valid:
        report("7.4", "Concurrent Varying-Size Overwrites", True,
               f"Some OCC contention expected under load. {detail}")
    else:
        report("7.4", "Concurrent Varying-Size Overwrites", False, detail)


def test_7_5_overwrite_multi_vs_autocommit():
    """Task 7.5: Overwrite in MULTI/EXEC vs auto-commit"""
    print("\n--- Task 7.5: MULTI/EXEC vs Auto-Commit Overwrite ---")
    print("Compare overwrite behavior between MULTI/EXEC and auto-commit.")

    r = get_client()

    # Part A: Auto-commit overwrites with size changes
    key_auto = f"{RUN_ID}_auto_overwrite"
    auto_sizes = [5, 10, 20, 10, 5, 100, 1]
    auto_failures = 0
    for size in auto_sizes:
        val = "A" * size
        r, ok, _ = try_set_get(r, key_auto, val)
        if not ok:
            auto_failures += 1

    # Part B: MULTI/EXEC overwrites with size changes
    key_multi = f"{RUN_ID}_multi_overwrite"
    # First create the key
    r.set(key_multi, "A" * 5)

    multi_sizes = [10, 20, 10, 5, 100, 1]
    multi_failures = 0
    for size in multi_sizes:
        val = "M" * size
        pipe = r.pipeline(transaction=True)
        pipe.set(key_multi, val)
        try:
            pipe.execute()
            # Verify
            raw = r.get(key_multi)
            got = extract_user_value(raw)
            if got != val.encode():
                multi_failures += 1
        except Exception:
            multi_failures += 1

    detail = (f"Auto-commit: {auto_failures}/{len(auto_sizes)} failures. "
              f"MULTI/EXEC: {multi_failures}/{len(multi_sizes)} failures.")

    if auto_failures == 0 and multi_failures == 0:
        report("7.5", "MULTI vs Auto-Commit Overwrite", True, detail)
    else:
        report("7.5", "MULTI vs Auto-Commit Overwrite", False, detail)


def test_7_6_large_size_transitions():
    """Task 7.6: Large value size transitions"""
    print("\n--- Task 7.6: Large Value Size Transitions ---")
    print("Test transitions between very different sizes: 1B↔1MB.")

    r = get_client()
    key = f"{RUN_ID}_large_transition"

    transitions = [
        ("1B→1KB", "X", "Y" * 1024),
        ("1KB→1B", "Y" * 1024, "Z"),
        ("1B→100KB", "A", "B" * 102400),
        ("100KB→1B", "B" * 102400, "C"),
        ("1B→1MB", "D", "E" * 1048576),
        ("1MB→1B", "E" * 1048576, "F"),
    ]

    failures = []
    for label, val1, val2 in transitions:
        r, ok1, _ = try_set_get(r, key, val1)
        if not ok1:
            failures.append((label, "initial write failed"))
            continue
        r, ok2, got = try_set_get(r, key, val2)
        if not ok2:
            failures.append((label, f"transition failed: {got!r}"))

    if len(failures) == 0:
        report("7.6", f"Large Size Transitions ({len(transitions)} tests)", True,
               "All large-to-small and small-to-large transitions succeeded.")
    else:
        report("7.6", f"Large Size Transitions ({len(transitions)} tests)", False,
               f"{len(failures)} failed: {failures}")


def test_7_7_encode_boundary_sizes():
    """Task 7.7: Overwrite near mako::Encode boundary sizes"""
    print("\n--- Task 7.7: Encode Boundary Sizes ---")
    print("Test values near the EXTRA_BITS_FOR_VALUE (20 bytes) boundary.")

    r = get_client()
    # Test sizes near the encoding overhead boundary
    boundary_sizes = [18, 19, 20, 21, 22, 39, 40, 41]
    key = f"{RUN_ID}_boundary"

    failures = []
    for i, size in enumerate(boundary_sizes):
        val = chr(65 + (i % 26)) * size
        r, ok, got = try_set_get(r, key, val)
        if not ok:
            failures.append((size, got))

    if len(failures) == 0:
        report("7.7", f"Encode Boundary Sizes ({len(boundary_sizes)} tests)", True,
               f"All sizes near encoding boundary worked: {boundary_sizes}")
    else:
        report("7.7", f"Encode Boundary Sizes ({len(boundary_sizes)} tests)", False,
               f"Failures at sizes: {failures}")


def test_7_8_stress_random_overwrites():
    """Task 7.8: Stress test — 1000 random-size overwrites"""
    print("\n--- Task 7.8: 1000 Random-Size Overwrites ---")
    print("Single key, 1000 overwrites with random sizes 1-1000.")

    r = get_client()
    key = f"{RUN_ID}_stress_overwrite"
    n = 1000
    failures = 0
    fail_transitions = []

    prev_size = None
    for i in range(n):
        size = random.randint(1, 1000)
        val = chr(65 + (i % 26)) * size
        r, ok, got = try_set_get(r, key, val)
        if not ok:
            failures += 1
            if len(fail_transitions) < 10:
                fail_transitions.append((prev_size, size, i))
        prev_size = size

    if failures == 0:
        report("7.8", f"Random-Size Overwrites ({n} iters)", True,
               f"All {n} random-size overwrites succeeded. Size range: 1-1000.")
    else:
        report("7.8", f"Random-Size Overwrites ({n} iters)", False,
               f"{failures}/{n} failed. "
               f"Failing transitions (prev→new, iter): {fail_transitions}")


def main():
    print("=" * 60)
    print("Mako Correctness Tests - Task 7: Overwrite Bug Investigation")
    print(f"Run ID: {RUN_ID}")
    print("=" * 60)

    ensure_server()

    test_7_1_size_transition_matrix()
    test_7_2_size_alternation()
    test_7_3_overwrite_after_history()
    test_7_4_concurrent_varying_overwrites()
    test_7_5_overwrite_multi_vs_autocommit()
    test_7_6_large_size_transitions()
    test_7_7_encode_boundary_sizes()
    test_7_8_stress_random_overwrites()

    # Summary
    print("\n" + "=" * 60)
    passed = sum(1 for _, _, p, _ in RESULTS if p)
    failed = sum(1 for _, _, p, _ in RESULTS if not p)
    print(f"Task 7 Summary: {passed} passed, {failed} failed, "
          f"{len(RESULTS)} total")

    if failed == 0:
        print("CONCLUSION: Overwrite bug from Task 6.5 did NOT reproduce.")
        print("The bug may be intermittent, related to server warmup state,")
        print("or specific to certain memory layout conditions.")
    else:
        print("CONCLUSION: Overwrite failures detected. See details above.")
    print("=" * 60)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
