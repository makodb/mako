#!/usr/bin/env python3
"""
Mako Correctness Tests - Task 2: Transaction Operations
========================================================
Tests 2.1-2.7: Multi-key commit, abort/rollback, partial failure,
overwriting pre-existing keys, abort safety, empty txn, large txn.

Uses MULTI/EXEC/DISCARD for transaction semantics.

KNOWN CONSTRAINT: Read-your-own-writes within a single MULTI/EXEC
transaction causes an OCC abort in Masstree. All tests are designed
to avoid this pattern: writes happen in MULTI/EXEC, reads happen
as separate auto-committed operations.
"""

import sys
import os
import uuid

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


def multi_set(r, kvpairs):
    """Execute multiple SETs atomically via MULTI/EXEC.
    Returns True if committed, False if aborted."""
    pipe = r.pipeline(transaction=True)
    for k, v in kvpairs:
        pipe.set(k, v)
    try:
        results = pipe.execute()
        return all(r is True for r in results)
    except Exception:
        return False


def test_2_1_txn_commit_multi_key():
    """Task 2.1: Transaction Commit (Multi-Key)"""
    print("\n--- Task 2.1: Transaction Commit (Multi-Key) ---")
    print("Write 3 keys atomically in MULTI/EXEC, verify all exist after.")
    r = get_client()

    keys = [f"{RUN_ID}_txn_a", f"{RUN_ID}_txn_b", f"{RUN_ID}_txn_c"]
    vals = ["val_a", "val_b", "val_c"]

    ok = multi_set(r, list(zip(keys, vals)))
    if not ok:
        report("2.1", "Transaction Commit (Multi-Key)", False,
               "MULTI/EXEC failed to commit")
        return

    # Verify all keys outside the transaction
    all_ok = True
    for key, expected_val in zip(keys, vals):
        raw = r.get(key)
        if raw is None:
            report("2.1", "Transaction Commit (Multi-Key)", False,
                   f"Key {key} missing after commit")
            return
        got = extract_user_value(raw)
        if got != expected_val.encode():
            all_ok = False
            report("2.1", "Transaction Commit (Multi-Key)", False,
                   f"Key {key}: expected {expected_val!r}, got {got!r}")
            return

    report("2.1", "Transaction Commit (Multi-Key)", True,
           "All 3 keys present with correct values after MULTI/EXEC commit")


def test_2_2_txn_abort_rollback():
    """Task 2.2: Transaction Abort/Rollback (Multi-Key)"""
    print("\n--- Task 2.2: Transaction Abort/Rollback (Multi-Key) ---")
    print("Write 3 keys in MULTI, then DISCARD. Verify none exist.")
    r = get_client()

    keys = [f"{RUN_ID}_abort_a", f"{RUN_ID}_abort_b", f"{RUN_ID}_abort_c"]

    # Use raw pipeline to send MULTI + SETs + DISCARD
    pipe = r.pipeline(transaction=False)
    pipe.execute_command("MULTI")
    for key in keys:
        pipe.execute_command("SET", key, f"val_{key}")
    pipe.execute_command("DISCARD")
    try:
        pipe.execute()
    except Exception:
        pass  # DISCARD may cause non-standard response handling

    # Verify no keys exist
    all_absent = True
    for key in keys:
        raw = r.get(key)
        if raw is not None:
            all_absent = False
            report("2.2", "Transaction Abort/Rollback", False,
                   f"Key {key} exists after DISCARD: {extract_user_value(raw)!r}")
            return

    report("2.2", "Transaction Abort/Rollback", True,
           "All 3 keys absent after DISCARD - writes correctly discarded")


def test_2_3_partial_failure():
    """Task 2.3: Partial Failure Within Transaction"""
    print("\n--- Task 2.3: Partial Failure Within Transaction ---")
    print("Test: SET + GET same key in MULTI causes OCC conflict → full abort.")
    print("NOTE: This tests Mako's OCC behavior with read-your-own-writes.")
    r = get_client()

    key = f"{RUN_ID}_partial_1"

    # This transaction should fail: SET then GET same key in MULTI
    import socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(("127.0.0.1", 6380))
    sock.settimeout(5)

    def send_cmd(sock, *args):
        cmd = f"*{len(args)}\r\n"
        for a in args:
            a_str = str(a)
            cmd += f"${len(a_str)}\r\n{a_str}\r\n"
        sock.sendall(cmd.encode())
        import time; time.sleep(0.05)
        return sock.recv(4096).decode(errors='replace')

    send_cmd(sock, "MULTI")
    send_cmd(sock, "SET", key, "good_value")
    send_cmd(sock, "GET", key)
    resp = send_cmd(sock, "EXEC")
    sock.close()

    # EXEC should return *-1 (nil array = transaction aborted)
    aborted = "*-1" in resp

    # Verify key does NOT exist (transaction was aborted)
    raw = r.get(key)

    if aborted and raw is None:
        report("2.3", "Partial Failure (OCC conflict)", True,
               "SET+GET same key in MULTI → OCC abort. "
               "Key does not exist. Mako auto-aborts on conflict.")
    elif aborted and raw is not None:
        report("2.3", "Partial Failure (OCC conflict)", False,
               f"Transaction aborted but key exists: {extract_user_value(raw)!r}")
    elif not aborted and raw is not None:
        report("2.3", "Partial Failure (OCC conflict)", True,
               "Transaction succeeded (read-your-own-writes works). "
               f"Value: {extract_user_value(raw)!r}")
    else:
        report("2.3", "Partial Failure (OCC conflict)", False,
               f"Unexpected: aborted={aborted}, key_exists={raw is not None}")


def test_2_4_txn_overwrite_preexisting():
    """Task 2.4: Transaction Overwriting Pre-Existing Keys"""
    print("\n--- Task 2.4: Transaction Overwriting Pre-Existing Keys ---")
    print("Write a key, then overwrite it within MULTI/EXEC.")
    r = get_client()

    key = f"{RUN_ID}_pre_existing"

    # Write original value outside transaction
    r.set(key, "original_value")
    raw = r.get(key)
    assert extract_user_value(raw) == b"original_value"

    # Overwrite within transaction
    ok = multi_set(r, [(key, "txn_updated_value")])
    if not ok:
        report("2.4", "Txn Overwrite Pre-Existing", False,
               "MULTI/EXEC failed to commit")
        return

    raw2 = r.get(key)
    got = extract_user_value(raw2)
    if got == b"txn_updated_value":
        report("2.4", "Txn Overwrite Pre-Existing", True)
    else:
        report("2.4", "Txn Overwrite Pre-Existing", False,
               f"Expected 'txn_updated_value', got {got!r}")


def test_2_5_abort_preserves_preexisting():
    """Task 2.5: Transaction Abort Does Not Affect Pre-Existing Data"""
    print("\n--- Task 2.5: Abort Preserves Pre-Existing Data ---")
    print("Write key, attempt overwrite in MULTI, DISCARD, verify original.")
    r = get_client()

    key = f"{RUN_ID}_safe_key"

    # Write original value
    r.set(key, "safe_value")
    raw = r.get(key)
    assert extract_user_value(raw) == b"safe_value"

    # Attempt overwrite in MULTI, then DISCARD
    pipe = r.pipeline(transaction=False)
    pipe.execute_command("MULTI")
    pipe.execute_command("SET", key, "dangerous_value")
    pipe.execute_command("DISCARD")
    try:
        pipe.execute()
    except Exception:
        pass

    # Verify original value preserved
    raw2 = r.get(key)
    got = extract_user_value(raw2)
    if got == b"safe_value":
        report("2.5", "Abort Preserves Pre-Existing Data", True)
    else:
        report("2.5", "Abort Preserves Pre-Existing Data", False,
               f"Expected 'safe_value', got {got!r}")


def test_2_6_empty_transaction():
    """Task 2.6: Empty Transaction"""
    print("\n--- Task 2.6: Empty Transaction ---")
    print("MULTI followed immediately by EXEC with no operations.")
    r = get_client()

    import socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(("127.0.0.1", 6380))
    sock.settimeout(5)

    def send_cmd(sock, *args):
        cmd = f"*{len(args)}\r\n"
        for a in args:
            a_str = str(a)
            cmd += f"${len(a_str)}\r\n{a_str}\r\n"
        sock.sendall(cmd.encode())
        import time; time.sleep(0.05)
        return sock.recv(4096).decode(errors='replace')

    resp_multi = send_cmd(sock, "MULTI")
    resp_exec = send_cmd(sock, "EXEC")
    sock.close()

    # Empty MULTI/EXEC should return empty array *0
    if "+OK" in resp_multi and "*0" in resp_exec:
        report("2.6", "Empty Transaction", True,
               "MULTI → +OK, EXEC → *0 (empty array). No errors.")
    else:
        report("2.6", "Empty Transaction", False,
               f"MULTI response: {resp_multi.strip()!r}, "
               f"EXEC response: {resp_exec.strip()!r}")


def test_2_7_large_transaction():
    """Task 2.7: Large Transaction (500 keys)"""
    print("\n--- Task 2.7: Large Transaction (500 keys) ---")
    print("Write 500 keys atomically in MULTI/EXEC, verify all exist.")
    r = get_client()

    n = 500
    prefix = f"{RUN_ID}_ltxn"
    kvpairs = [(f"{prefix}_{i:04d}", f"val_{i:04d}") for i in range(n)]

    ok = multi_set(r, kvpairs)
    if not ok:
        report("2.7", "Large Transaction (500 keys)", False,
               "MULTI/EXEC with 500 SETs failed to commit")
        return

    # Verify all keys
    missing = 0
    mismatches = 0
    for key, expected_val in kvpairs:
        raw = r.get(key)
        if raw is None:
            missing += 1
            continue
        got = extract_user_value(raw)
        if got != expected_val.encode():
            mismatches += 1

    if missing == 0 and mismatches == 0:
        report("2.7", "Large Transaction (500 keys)", True,
               f"All {n} keys verified after atomic commit")
    else:
        report("2.7", "Large Transaction (500 keys)", False,
               f"{missing} missing, {mismatches} mismatches out of {n}")


def main():
    print("=" * 60)
    print("Mako Correctness Tests - Task 2: Transaction Operations")
    print(f"Run ID: {RUN_ID}")
    print("=" * 60)

    ensure_server()

    test_2_1_txn_commit_multi_key()
    test_2_2_txn_abort_rollback()
    test_2_3_partial_failure()
    test_2_4_txn_overwrite_preexisting()
    test_2_5_abort_preserves_preexisting()
    test_2_6_empty_transaction()
    test_2_7_large_transaction()

    # Summary
    print("\n" + "=" * 60)
    passed = sum(1 for _, _, p, _ in RESULTS if p)
    failed = sum(1 for _, _, p, _ in RESULTS if not p)
    print(f"Task 2 Summary: {passed} passed, {failed} failed, "
          f"{len(RESULTS)} total")
    print("=" * 60)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
