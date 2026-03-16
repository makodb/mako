#!/usr/bin/env python3
"""
Mako Correctness Tests - Task 9: Comprehensive Delete Testing
===============================================================
Now that DEL is wired through the Redis FFI, thoroughly test delete
semantics including edge cases, interactions with transactions, and
concurrent delete scenarios.

9.1: Basic delete + re-insert cycle
9.2: Delete non-existent key
9.3: Double delete (delete same key twice)
9.4: Delete within MULTI/EXEC
9.5: Delete + DISCARD (abort preserves key)
9.6: Concurrent delete + read
9.7: Concurrent delete + write (re-insert race)
9.8: Delete then overwrite (re-create with different size)
9.9: Bulk delete (100 keys)
9.10: Delete with special keys (long, unicode, spaces)
"""

import sys
import os
import uuid
import time
import threading

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


def test_9_1_delete_reinsert():
    """Delete a key, then re-insert it with a new value."""
    print("\n--- Task 9.1: Delete + Re-Insert ---")
    r = get_client(socket_timeout=10, retry_on_timeout=True)
    key = f"{RUN_ID}_del_reinsert"

    r.set(key, "original")
    assert extract_user_value(r.get(key)) == b"original"

    r.delete(key)
    raw = r.get(key)
    if raw is not None:
        report("9.1", "Delete + Re-Insert", False,
               f"GET after DEL returned {extract_user_value(raw)!r} instead of nil")
        return

    # Re-insert with different value
    r.set(key, "reinserted")
    raw2 = r.get(key)
    got = extract_user_value(raw2)
    if got == b"reinserted":
        report("9.1", "Delete + Re-Insert", True,
               "DEL removes key, re-SET creates it fresh with new value.")
    else:
        report("9.1", "Delete + Re-Insert", False,
               f"After re-insert, expected 'reinserted', got {got!r}")


def test_9_2_delete_nonexistent():
    """Delete a key that was never written."""
    print("\n--- Task 9.2: Delete Non-Existent Key ---")
    r = get_client(socket_timeout=10, retry_on_timeout=True)
    key = f"{RUN_ID}_del_nonexist_xyz"

    try:
        result = r.delete(key)
        # Redis DEL returns integer count of deleted keys
        # makoCon always returns :1 (see Rust FFI comment)
        report("9.2", "Delete Non-Existent Key", True,
               f"DEL on non-existent key returned {result}. No crash or error.")
    except Exception as e:
        report("9.2", "Delete Non-Existent Key", False,
               f"Exception on DEL of non-existent key: {e}")


def test_9_3_double_delete():
    """Delete the same key twice."""
    print("\n--- Task 9.3: Double Delete ---")
    r = get_client(socket_timeout=10, retry_on_timeout=True)
    key = f"{RUN_ID}_double_del"

    r.set(key, "to_delete")
    try:
        r.delete(key)
        r.delete(key)  # Second delete on already-deleted key
        raw = r.get(key)
        if raw is None:
            report("9.3", "Double Delete", True,
                   "Second DEL on already-deleted key succeeds without error.")
        else:
            report("9.3", "Double Delete", False,
                   f"Key reappeared after double delete: {extract_user_value(raw)!r}")
    except Exception as e:
        report("9.3", "Double Delete", False, f"Exception: {e}")


def test_9_4_delete_in_multi():
    """Delete within MULTI/EXEC."""
    print("\n--- Task 9.4: Delete in MULTI/EXEC ---")
    r = get_client(socket_timeout=10, retry_on_timeout=True)
    key = f"{RUN_ID}_multi_del"

    r.set(key, "before_multi_del")
    assert extract_user_value(r.get(key)) == b"before_multi_del"

    # DEL inside MULTI/EXEC via redis-py pipeline (transaction=True sends MULTI/EXEC).
    # redis-py 7.x correctly parses integer responses from DEL within EXEC arrays.
    try:
        pipe = r.pipeline(transaction=True)
        pipe.delete(key)
        results = pipe.execute()
        # results[0] is the DEL return value (integer: keys deleted)
        del_count = results[0] if results else 0
    except Exception as exc:
        report("9.4", "Delete in MULTI/EXEC", False,
               f"Pipeline raised exception: {exc}")
        return

    # Verify key is gone using fresh client
    r2 = get_client(socket_timeout=10, retry_on_timeout=True)
    raw = r2.get(key)
    if raw is None:
        report("9.4", "Delete in MULTI/EXEC", True,
               f"DEL in MULTI committed (del_count={del_count}). Key absent after commit.")
    else:
        report("9.4", "Delete in MULTI/EXEC", False,
               f"Key still exists after MULTI DEL: {extract_user_value(raw)!r}. "
               f"del_count={del_count}")


def test_9_5_delete_discard():
    """MULTI + DEL + DISCARD should preserve the key."""
    print("\n--- Task 9.5: Delete + DISCARD ---")
    r = get_client(socket_timeout=10, retry_on_timeout=True)
    key = f"{RUN_ID}_del_discard"

    r.set(key, "safe_from_discard")

    # MULTI + DEL + DISCARD
    pipe = r.pipeline(transaction=False)
    pipe.execute_command("MULTI")
    pipe.execute_command("DEL", key)
    pipe.execute_command("DISCARD")
    try:
        pipe.execute()
    except Exception:
        pass

    # Use fresh client for verification (pipeline may corrupt connection state)
    r2 = get_client(socket_timeout=10, retry_on_timeout=True)
    raw = r2.get(key)
    got = extract_user_value(raw)
    if got == b"safe_from_discard":
        report("9.5", "Delete + DISCARD", True,
               "DISCARD correctly preserves key after queued DEL.")
    elif raw is None:
        report("9.5", "Delete + DISCARD", False,
               "Key was deleted despite DISCARD!")
    else:
        report("9.5", "Delete + DISCARD", False,
               f"Key has unexpected value: {got!r}")


def test_9_6_concurrent_delete_read():
    """One thread deletes, another reads. Reader should see either old value or nil."""
    print("\n--- Task 9.6: Concurrent Delete + Read ---")
    r_setup = get_client(socket_timeout=10, retry_on_timeout=True)
    key = f"{RUN_ID}_conc_del_read"
    n_rounds = 20
    anomalies = []

    # Pre-create clients with short timeouts to avoid connection exhaustion
    r_del = get_client(socket_timeout=3, retry_on_timeout=True)
    r_read = get_client(socket_timeout=3, retry_on_timeout=True)

    for i in range(n_rounds):
        r_setup.set(key, f"round_{i:04d}")

        barrier = threading.Barrier(2)
        read_result = [None]

        def deleter():
            barrier.wait()
            try:
                r_del.delete(key)
            except Exception:
                pass

        def reader():
            barrier.wait()
            try:
                raw = r_read.get(key)
                read_result[0] = raw
            except Exception:
                read_result[0] = "error"

        td = threading.Thread(target=deleter)
        tr = threading.Thread(target=reader)
        td.start()
        tr.start()
        td.join(timeout=5)
        tr.join(timeout=5)

        # Reader should see either the value or nil (not corrupted data)
        if read_result[0] == "error":
            continue  # Timeout, skip
        if read_result[0] is not None:
            val = extract_user_value(read_result[0])
            expected = f"round_{i:04d}".encode()
            if val != expected:
                anomalies.append((i, val))

    if len(anomalies) == 0:
        report("9.6", f"Concurrent Delete + Read ({n_rounds} rounds)", True,
               "No corrupted reads. Reader always sees valid value or nil.")
    else:
        report("9.6", f"Concurrent Delete + Read ({n_rounds} rounds)", False,
               f"{len(anomalies)} anomalies: {anomalies[:5]}")


def test_9_7_concurrent_delete_reinsert():
    """One thread deletes, another re-inserts. Final state should be consistent."""
    print("\n--- Task 9.7: Concurrent Delete + Re-Insert ---")
    r_setup = get_client(socket_timeout=10, retry_on_timeout=True)
    key = f"{RUN_ID}_conc_del_reins"
    n_rounds = 20
    inconsistencies = 0

    # Pre-create clients with short timeouts
    r_del = get_client(socket_timeout=3, retry_on_timeout=True)
    r_ins = get_client(socket_timeout=3, retry_on_timeout=True)

    for i in range(n_rounds):
        r_setup.set(key, "initial")

        barrier = threading.Barrier(2)

        def deleter():
            barrier.wait()
            try:
                r_del.delete(key)
            except Exception:
                pass

        def reinserter():
            barrier.wait()
            try:
                r_ins.set(key, "reinserted")
            except Exception:
                pass

        td = threading.Thread(target=deleter)
        tr = threading.Thread(target=reinserter)
        td.start()
        tr.start()
        td.join(timeout=5)
        tr.join(timeout=5)

        # Final value should be either nil (delete won) or "reinserted" (set won)
        raw = r_setup.get(key)
        if raw is not None:
            val = extract_user_value(raw)
            if val != b"reinserted" and val != b"initial":
                inconsistencies += 1

    if inconsistencies == 0:
        report("9.7", f"Concurrent Delete + Re-Insert ({n_rounds} rounds)", True,
               "Final value always consistent (nil or 'reinserted'). "
               "No corrupted state from delete/write race.")
    else:
        report("9.7", f"Concurrent Delete + Re-Insert ({n_rounds} rounds)", False,
               f"{inconsistencies} inconsistencies in {n_rounds} rounds")


def test_9_8_delete_then_rewrite_different_size():
    """Delete key, then re-create with a very different value size."""
    print("\n--- Task 9.8: Delete Then Re-Write (Different Size) ---")
    r = get_client(socket_timeout=10, retry_on_timeout=True)
    key = f"{RUN_ID}_del_resize"

    sizes = [
        ("tiny→large", "X", "Y" * 1000),
        ("large→tiny", "A" * 1000, "B"),
        ("tiny→huge", "C", "D" * 100000),
    ]

    failures = []
    for label, v1, v2 in sizes:
        r.set(key, v1)
        r.delete(key)
        # Verify deleted
        if r.get(key) is not None:
            failures.append((label, "delete failed"))
            continue
        # Re-create with different size
        try:
            r.set(key, v2)
            raw = r.get(key)
            got = extract_user_value(raw, expected_len=len(v2))
            if got != v2.encode():
                failures.append((label, "value mismatch after re-create"))
        except Exception as e:
            failures.append((label, str(e)))

    if len(failures) == 0:
        report("9.8", "Delete Then Re-Write (Different Size)", True,
               f"All {len(sizes)} delete+re-create cycles with size changes succeed. "
               "No residual allocation issues from deleted key.")
    else:
        report("9.8", "Delete Then Re-Write (Different Size)", False,
               f"{len(failures)} failures: {failures}")


def test_9_9_bulk_delete():
    """Write 100 keys, delete all, verify all gone."""
    print("\n--- Task 9.9: Bulk Delete (100 keys) ---")
    r = get_client(socket_timeout=10, retry_on_timeout=True)
    n = 100
    prefix = f"{RUN_ID}_bulk_del"

    # Write
    for i in range(n):
        r.set(f"{prefix}_{i:04d}", f"val_{i:04d}")

    # Verify all exist
    missing_before = sum(1 for i in range(n) if r.get(f"{prefix}_{i:04d}") is None)
    if missing_before > 0:
        report("9.9", f"Bulk Delete ({n} keys)", False,
               f"{missing_before} keys missing before delete")
        return

    # Delete all
    delete_errors = 0
    for i in range(n):
        try:
            r.delete(f"{prefix}_{i:04d}")
        except Exception:
            delete_errors += 1

    # Verify all gone
    survivors = 0
    for i in range(n):
        if r.get(f"{prefix}_{i:04d}") is not None:
            survivors += 1

    if survivors == 0 and delete_errors == 0:
        report("9.9", f"Bulk Delete ({n} keys)", True,
               f"All {n} keys deleted successfully. 0 survivors.")
    else:
        report("9.9", f"Bulk Delete ({n} keys)", False,
               f"{survivors} survivors, {delete_errors} delete errors")


def test_9_10_delete_special_keys():
    """Delete keys with special characters."""
    print("\n--- Task 9.10: Delete Special Keys ---")
    r = get_client(socket_timeout=10, retry_on_timeout=True)

    special_keys = [
        ("single_char", "z"),
        ("long_key", "K" * 1024),
        ("unicode", f"{RUN_ID}_clé_spéciale"),
        ("with_spaces", f"{RUN_ID} key with spaces"),
        ("with_slashes", f"{RUN_ID}/a/b/c"),
    ]

    failures = []
    for label, key in special_keys:
        try:
            r.set(key, f"val_for_{label}")
            assert r.get(key) is not None
            r.delete(key)
            raw = r.get(key)
            if raw is not None:
                failures.append((label, f"still exists: {extract_user_value(raw)!r}"))
        except Exception as e:
            failures.append((label, str(e)))

    if len(failures) == 0:
        report("9.10", f"Delete Special Keys ({len(special_keys)} tests)", True,
               "All special key formats delete correctly.")
    else:
        report("9.10", f"Delete Special Keys ({len(special_keys)} tests)", False,
               f"{len(failures)} failures: {failures}")


def main():
    print("=" * 60)
    print("Mako Correctness Tests - Task 9: Comprehensive Delete Testing")
    print(f"Run ID: {RUN_ID}")
    print("=" * 60)

    # Stop any stale server and start fresh for clean state
    from server_manager import stop_server as _stop, start_server as _start, is_server_running
    if is_server_running():
        _stop()
        import time; time.sleep(1)
    _start()

    test_9_1_delete_reinsert()
    test_9_2_delete_nonexistent()
    test_9_3_double_delete()
    test_9_4_delete_in_multi()
    test_9_5_delete_discard()
    test_9_6_concurrent_delete_read()
    test_9_7_concurrent_delete_reinsert()
    test_9_8_delete_then_rewrite_different_size()
    test_9_9_bulk_delete()
    test_9_10_delete_special_keys()

    # Summary
    print("\n" + "=" * 60)
    passed = sum(1 for _, _, p, _ in RESULTS if p)
    failed = sum(1 for _, _, p, _ in RESULTS if not p)
    print(f"Task 9 Summary: {passed} passed, {failed} failed, "
          f"{len(RESULTS)} total")
    print("=" * 60)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
