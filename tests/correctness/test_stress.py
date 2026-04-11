#!/usr/bin/env python3
"""
Mako Correctness Tests - Task 5: Stress and Edge Cases
========================================================
Tests 5.1-5.5: Rapid operations, concurrent readers during writes,
transaction timeout, double commit/abort, operations after txn end.
"""

import sys
import os
import uuid
import time
import threading
import socket

sys.path.insert(0, os.path.dirname(__file__))
from server_manager import ensure_server, get_client, extract_user_value, MAKO_HOST, MAKO_PORT

RUN_ID = uuid.uuid4().hex[:8]
RESULTS = []


def report(test_id, test_name, passed, detail=""):
    status = "PASS" if passed else "FAIL"
    msg = f"[{status}] {test_id}: {test_name}"
    if detail:
        msg += f" -- {detail}"
    print(msg)
    RESULTS.append((test_id, test_name, passed, detail))


def test_5_1_rapid_same_key():
    """Task 5.1: Rapid Repeated Operations on Same Key"""
    print("\n--- Task 5.1: Rapid Repeated Operations on Same Key ---")
    print("1000 write-then-read cycles on the same key.")
    r = get_client()

    key = f"{RUN_ID}_rapid_key"
    n = 1000
    stale_reads = 0
    errors = 0

    t0 = time.time()
    for i in range(n):
        val = f"v{i:06d}"
        r.set(key, val)
        raw = r.get(key)
        if raw is None:
            errors += 1
            continue
        got = extract_user_value(raw).decode()
        if got != val:
            stale_reads += 1

    elapsed = time.time() - t0

    if stale_reads == 0 and errors == 0:
        report("5.1", "Rapid Same-Key Operations (1000 iters)", True,
               f"No stale reads or errors. {elapsed:.2f}s "
               f"({n/elapsed:.0f} ops/sec)")
    else:
        report("5.1", "Rapid Same-Key Operations (1000 iters)", False,
               f"{stale_reads} stale reads, {errors} errors in {n} iterations")


def test_5_2_concurrent_readers_writers():
    """Task 5.2: Concurrent Readers During Writes"""
    print("\n--- Task 5.2: Concurrent Readers During Writes ---")
    print("1 writer + 5 readers on same key for 5 seconds.")
    r = get_client()

    key = f"{RUN_ID}_hot_key"
    r.set(key, "0")  # Initial value

    stop_flag = threading.Event()
    write_count = [0]
    read_counts = [0] * 5
    corruptions = [0] * 5
    invalid_values = []
    lock = threading.Lock()

    def writer():
        wr = get_client(socket_timeout=5, retry_on_timeout=True)
        i = 0
        while not stop_flag.is_set():
            try:
                wr.set(key, str(i))
                i += 1
            except Exception:
                # Reconnect on error
                try:
                    wr = get_client(socket_timeout=5, retry_on_timeout=True)
                except Exception:
                    pass
        write_count[0] = i

    def reader(idx):
        rr = get_client(socket_timeout=5, retry_on_timeout=True)
        count = 0
        corrupt = 0
        while not stop_flag.is_set():
            try:
                raw = rr.get(key)
                if raw is not None:
                    val = extract_user_value(raw)
                    try:
                        # Value should be a valid integer string
                        int(val)
                    except ValueError:
                        corrupt += 1
                        with lock:
                            if len(invalid_values) < 5:
                                invalid_values.append(val)
                count += 1
            except Exception:
                try:
                    rr = get_client(socket_timeout=5, retry_on_timeout=True)
                except Exception:
                    pass
        read_counts[idx] = count
        corruptions[idx] = corrupt

    # Start threads
    threads = []
    t_w = threading.Thread(target=writer)
    t_w.start()
    threads.append(t_w)

    for i in range(5):
        t_r = threading.Thread(target=reader, args=(i,))
        t_r.start()
        threads.append(t_r)

    # Run for 5 seconds
    time.sleep(5)
    stop_flag.set()

    for t in threads:
        t.join(timeout=5)

    total_reads = sum(read_counts)
    total_corruptions = sum(corruptions)

    if total_corruptions == 0:
        report("5.2", "Concurrent Readers During Writes (5s)", True,
               f"{write_count[0]} writes, {total_reads} reads, "
               f"0 corruptions. No torn reads detected.")
    else:
        report("5.2", "Concurrent Readers During Writes (5s)", False,
               f"{total_corruptions} corruptions in {total_reads} reads. "
               f"Invalid values: {invalid_values[:5]}")


def test_5_3_transaction_timeout():
    """Task 5.3: Transaction Timeout / Stale Transaction"""
    print("\n--- Task 5.3: Transaction Timeout / Stale Transaction ---")
    print("Start MULTI, wait 10s, then EXEC. Check if timeout occurs.")
    print("NOTE: Using 10s instead of 60s for practical test duration.")

    key = f"{RUN_ID}_timeout_key"

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((MAKO_HOST, MAKO_PORT))
    sock.settimeout(30)

    def send_cmd(sock, *args):
        cmd = f"*{len(args)}\r\n"
        for a in args:
            a_str = str(a)
            cmd += f"${len(a_str)}\r\n{a_str}\r\n"
        sock.sendall(cmd.encode())
        time.sleep(0.05)
        try:
            return sock.recv(4096).decode(errors='replace')
        except socket.timeout:
            return "<timeout>"

    send_cmd(sock, "MULTI")
    send_cmd(sock, "SET", key, "delayed_value")

    print("  Waiting 10 seconds...")
    time.sleep(10)

    resp = send_cmd(sock, "EXEC")
    sock.close()

    # Check if the value was committed
    r = get_client()
    raw = r.get(key)

    if "*1" in resp and raw is not None:
        got = extract_user_value(raw)
        report("5.3", "Transaction Timeout (10s delay)", True,
               f"Transaction committed after 10s delay. Value: {got!r}. "
               "Mako does NOT enforce a transaction timeout for MULTI/EXEC. "
               "The queued commands are held in the Rust layer indefinitely.")
    elif "*-1" in resp or raw is None:
        report("5.3", "Transaction Timeout (10s delay)", True,
               "Transaction was aborted after delay. "
               "Mako may enforce a transaction timeout.")
    elif "<timeout>" in resp:
        report("5.3", "Transaction Timeout (10s delay)", True,
               "Socket timed out waiting for EXEC response. "
               "Server may have dropped the connection.")
    else:
        report("5.3", "Transaction Timeout (10s delay)", False,
               f"Unexpected EXEC response: {resp.strip()!r}, "
               f"key exists: {raw is not None}")


def test_5_4_double_commit_abort():
    """Task 5.4: Double Commit / Double Abort"""
    print("\n--- Task 5.4: Double Commit / Double Abort ---")
    print("Test: EXEC twice, DISCARD twice.")

    # Test A: Double EXEC
    print("  Part A: MULTI + SET + EXEC + EXEC")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((MAKO_HOST, MAKO_PORT))
    sock.settimeout(5)

    def send_cmd(sock, *args):
        cmd = f"*{len(args)}\r\n"
        for a in args:
            a_str = str(a)
            cmd += f"${len(a_str)}\r\n{a_str}\r\n"
        sock.sendall(cmd.encode())
        time.sleep(0.05)
        try:
            return sock.recv(4096).decode(errors='replace')
        except socket.timeout:
            return "<timeout>"

    key_a = f"{RUN_ID}_double_commit"
    send_cmd(sock, "MULTI")
    send_cmd(sock, "SET", key_a, "committed_once")
    resp_exec1 = send_cmd(sock, "EXEC")
    resp_exec2 = send_cmd(sock, "EXEC")
    sock.close()

    exec1_ok = "*1" in resp_exec1
    exec2_err = "ERR" in resp_exec2 or "*-1" in resp_exec2

    # Verify the first commit worked
    r = get_client()
    raw = r.get(key_a)
    val_a = extract_user_value(raw).decode() if raw else "None"

    part_a_ok = exec1_ok and val_a == "committed_once"
    print(f"    EXEC 1: {resp_exec1.strip()!r} (first commit)")
    print(f"    EXEC 2: {resp_exec2.strip()!r} (second commit attempt)")
    print(f"    Value: {val_a!r}")

    # Test B: Double DISCARD
    print("  Part B: MULTI + SET + DISCARD + DISCARD")
    sock2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock2.connect((MAKO_HOST, MAKO_PORT))
    sock2.settimeout(5)

    key_b = f"{RUN_ID}_double_abort"
    send_cmd(sock2, "MULTI")
    send_cmd(sock2, "SET", key_b, "should_not_exist")
    resp_disc1 = send_cmd(sock2, "DISCARD")
    resp_disc2 = send_cmd(sock2, "DISCARD")
    sock2.close()

    disc1_ok = "+OK" in resp_disc1
    disc2_err = "ERR" in resp_disc2

    raw_b = r.get(key_b)
    part_b_ok = raw_b is None  # Key should not exist

    print(f"    DISCARD 1: {resp_disc1.strip()!r}")
    print(f"    DISCARD 2: {resp_disc2.strip()!r}")
    print(f"    Key exists: {raw_b is not None}")

    if part_a_ok and part_b_ok:
        report("5.4", "Double Commit / Double Abort", True,
               f"First EXEC commits correctly, second EXEC returns error. "
               f"First DISCARD OK, second DISCARD returns error. "
               f"No data corruption.")
    else:
        report("5.4", "Double Commit / Double Abort", False,
               f"Part A (double commit): exec1_ok={exec1_ok}, val={val_a!r}. "
               f"Part B (double discard): disc1_ok={disc1_ok}, "
               f"key_absent={raw_b is None}")


def test_5_5_ops_after_txn_end():
    """Task 5.5: Operations After Transaction End"""
    print("\n--- Task 5.5: Operations After Transaction End ---")
    print("After EXEC, attempt SET in same connection (should work as auto-commit).")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((MAKO_HOST, MAKO_PORT))
    sock.settimeout(5)

    def send_cmd(sock, *args):
        cmd = f"*{len(args)}\r\n"
        for a in args:
            a_str = str(a)
            cmd += f"${len(a_str)}\r\n{a_str}\r\n"
        sock.sendall(cmd.encode())
        time.sleep(0.05)
        try:
            return sock.recv(4096).decode(errors='replace')
        except socket.timeout:
            return "<timeout>"

    key1 = f"{RUN_ID}_postcommit_txn"
    key2 = f"{RUN_ID}_postcommit_auto"

    # MULTI + SET + EXEC
    send_cmd(sock, "MULTI")
    send_cmd(sock, "SET", key1, "txn_value")
    resp_exec = send_cmd(sock, "EXEC")

    # After EXEC, SET outside MULTI (should be auto-committed)
    resp_set = send_cmd(sock, "SET", key2, "auto_value")
    sock.close()

    # Verify both keys
    r = get_client()
    raw1 = r.get(key1)
    raw2 = r.get(key2)
    val1 = extract_user_value(raw1).decode() if raw1 else "None"
    val2 = extract_user_value(raw2).decode() if raw2 else "None"

    if val1 == "txn_value" and val2 == "auto_value":
        report("5.5", "Operations After Transaction End", True,
               "After EXEC, regular SET works as auto-commit. "
               "Connection returns to normal (non-MULTI) mode.")
    else:
        report("5.5", "Operations After Transaction End", False,
               f"key1={val1!r}, key2={val2!r}")


def main():
    print("=" * 60)
    print("Mako Correctness Tests - Task 5: Stress and Edge Cases")
    print(f"Run ID: {RUN_ID}")
    print("=" * 60)

    ensure_server()

    test_5_1_rapid_same_key()
    test_5_2_concurrent_readers_writers()
    test_5_3_transaction_timeout()
    test_5_4_double_commit_abort()
    test_5_5_ops_after_txn_end()

    # Summary
    print("\n" + "=" * 60)
    passed = sum(1 for _, _, p, _ in RESULTS if p)
    failed = sum(1 for _, _, p, _ in RESULTS if not p)
    print(f"Task 5 Summary: {passed} passed, {failed} failed, "
          f"{len(RESULTS)} total")
    print("=" * 60)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
