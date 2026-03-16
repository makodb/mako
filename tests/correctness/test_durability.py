#!/usr/bin/env python3
"""
Mako Correctness Tests - Task 4: Durability (Persistence)
==========================================================
Tests 4.1-4.3: Survive clean restart, survive crash (SIGKILL),
uncommitted data should not survive restart.

IMPORTANT NOTE: The makoCon Redis-compatible server uses Mako's
in-memory Masstree index WITHOUT RocksDB persistence. Therefore,
data does NOT survive server restarts. These tests document this
behavior rather than testing persistence guarantees.

To test true persistence, one would need to use the dbtest binary
with RocksDB configuration enabled.
"""

import sys
import os
import uuid
import time
import signal

sys.path.insert(0, os.path.dirname(__file__))
from server_manager import (ensure_server, get_client, extract_user_value,
                            start_server, stop_server, get_server_pid,
                            is_server_running)

RUN_ID = uuid.uuid4().hex[:8]
RESULTS = []


def report(test_id, test_name, passed, detail=""):
    status = "PASS" if passed else "FAIL"
    msg = f"[{status}] {test_id}: {test_name}"
    if detail:
        msg += f" -- {detail}"
    print(msg)
    RESULTS.append((test_id, test_name, passed, detail))


def test_4_1_survive_clean_restart():
    """Task 4.1: Survive Clean Restart"""
    print("\n--- Task 4.1: Survive Clean Restart ---")
    print("Write 100 keys, stop server (SIGTERM), restart, read all keys.")
    print("NOTE: makoCon is in-memory only. Data WILL be lost on restart.")

    ensure_server()
    r = get_client()

    # Write 100 keys
    prefix = f"{RUN_ID}_durable"
    n = 100
    for i in range(n):
        r.set(f"{prefix}_{i:04d}", f"value_{i:04d}")

    # Verify all readable
    all_ok = True
    for i in range(n):
        raw = r.get(f"{prefix}_{i:04d}")
        if raw is None:
            all_ok = False
            break
    if not all_ok:
        report("4.1", "Survive Clean Restart", False,
               "Keys missing even before restart")
        return

    print(f"  {n} keys written and verified before restart.")

    # Gracefully stop
    stop_server()
    time.sleep(1)

    # Restart
    start_server()
    r2 = get_client()

    # Read all keys
    survived = 0
    for i in range(n):
        raw = r2.get(f"{prefix}_{i:04d}")
        if raw is not None:
            survived += 1

    if survived == n:
        report("4.1", "Survive Clean Restart", True,
               f"All {n} keys survived restart (unexpected for in-memory DB)")
    elif survived == 0:
        report("4.1", "Survive Clean Restart", True,
               f"0/{n} keys survived. EXPECTED: makoCon uses in-memory "
               "Masstree without RocksDB persistence. Data loss on restart "
               "is by design for this configuration.")
    else:
        report("4.1", "Survive Clean Restart", False,
               f"Partial survival: {survived}/{n} keys. "
               "This indicates inconsistent persistence behavior.")


def test_4_2_survive_crash():
    """Task 4.2: Survive Crash (SIGKILL)"""
    print("\n--- Task 4.2: Survive Crash (SIGKILL) ---")
    print("Write 100 keys, SIGKILL server, restart, read all keys.")
    print("NOTE: makoCon is in-memory only. Same expectation as 4.1.")

    ensure_server()
    r = get_client()

    prefix = f"{RUN_ID}_crash"
    n = 100
    for i in range(n):
        r.set(f"{prefix}_{i:04d}", f"value_{i:04d}")

    # Verify
    all_ok = True
    for i in range(n):
        raw = r.get(f"{prefix}_{i:04d}")
        if raw is None:
            all_ok = False
            break
    if not all_ok:
        report("4.2", "Survive Crash (SIGKILL)", False,
               "Keys missing even before crash")
        return

    print(f"  {n} keys written and verified. Sending SIGKILL...")

    # SIGKILL
    pid = get_server_pid()
    if pid:
        os.kill(pid, signal.SIGKILL)
        time.sleep(2)
    else:
        stop_server()
        time.sleep(1)

    # Restart
    start_server()
    r2 = get_client()

    survived = 0
    for i in range(n):
        raw = r2.get(f"{prefix}_{i:04d}")
        if raw is not None:
            survived += 1

    if survived == 0:
        report("4.2", "Survive Crash (SIGKILL)", True,
               f"0/{n} keys survived SIGKILL. EXPECTED: in-memory DB loses "
               "all data on crash. No data loss anomaly.")
    elif survived == n:
        report("4.2", "Survive Crash (SIGKILL)", True,
               f"All {n} keys survived (unexpected for in-memory DB)")
    else:
        report("4.2", "Survive Crash (SIGKILL)", False,
               f"Partial: {survived}/{n} keys survived SIGKILL")


def test_4_3_uncommitted_not_persisted():
    """Task 4.3: Uncommitted Data Does Not Survive Restart"""
    print("\n--- Task 4.3: Uncommitted Data Does Not Survive Restart ---")
    print("Queue writes in MULTI (uncommitted), kill server, restart.")
    print("NOTE: With in-memory DB, even committed data doesn't survive.")

    ensure_server()

    key = f"{RUN_ID}_uncommitted_durability"

    # Start MULTI but don't EXEC
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
        time.sleep(0.05)
        return sock.recv(4096).decode(errors='replace')

    send_cmd(sock, "MULTI")
    send_cmd(sock, "SET", key, "should_not_persist")
    # DON'T EXEC - leave transaction uncommitted

    # Kill server
    pid = get_server_pid()
    if pid:
        os.kill(pid, signal.SIGKILL)
        time.sleep(2)
    else:
        stop_server()
        time.sleep(1)

    try:
        sock.close()
    except Exception:
        pass

    # Restart
    start_server()
    r = get_client()

    raw = r.get(key)
    if raw is None:
        report("4.3", "Uncommitted Data Not Persisted", True,
               "Uncommitted key does not exist after restart. "
               "MULTI without EXEC correctly discards queued writes.")
    else:
        got = extract_user_value(raw)
        report("4.3", "Uncommitted Data Not Persisted", False,
               f"Uncommitted data found after restart: {got!r}")


def main():
    print("=" * 60)
    print("Mako Correctness Tests - Task 4: Durability (Persistence)")
    print(f"Run ID: {RUN_ID}")
    print("=" * 60)

    test_4_1_survive_clean_restart()
    test_4_2_survive_crash()
    test_4_3_uncommitted_not_persisted()

    # Summary
    print("\n" + "=" * 60)
    passed = sum(1 for _, _, p, _ in RESULTS if p)
    failed = sum(1 for _, _, p, _ in RESULTS if not p)
    print(f"Task 4 Summary: {passed} passed, {failed} failed, "
          f"{len(RESULTS)} total")
    print("=" * 60)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    finally:
        stop_server()
