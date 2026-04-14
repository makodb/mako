#!/usr/bin/env python3
"""
Mako Workload Test - Task 6.6: Crash Recovery (in-memory durability)
=====================================================================
Tests that makoCon behaves correctly as a purely in-memory store after
a hard kill (SIGKILL):

  1. Write 50 keys before the crash.
  2. SIGKILL the server process.
  3. Restart the server.
  4. Verify all 50 pre-crash keys are GONE (no on-disk persistence expected).
  5. Write 50 new keys and verify they are readable (server works post-restart).

Pass criteria
-------------
  - Server restarts successfully after SIGKILL.
  - All 50 pre-crash keys are absent after restart (in-memory = no durability).
  - All 50 post-restart writes succeed and read back with the correct value.

Fail criteria
-------------
  - Server does not restart (start_server() times out or errors).
  - Any pre-crash key is still present after restart (unexpected persistence).
  - Any post-restart write is unreadable or returns the wrong value.
"""

import sys
import os
import signal
import time
import uuid
import subprocess

sys.path.insert(0, os.path.dirname(__file__))
from server_manager import (
    ensure_server,
    start_server,
    get_client,
    extract_user_value,
    get_server_pid,
    MAKO_HOST,
    MAKO_PORT,
)

RUN_ID = uuid.uuid4().hex[:8]
RESULTS = []

NUM_KEYS         = 50
PRE_CRASH_VALUE  = "1000"
POST_CRASH_VALUE = "2000"


# ── Helpers ────────────────────────────────────────────────────────────────────

def crash_key(i):
    return f"{RUN_ID}_crash_acct_{i:04d}"


def post_key(i):
    return f"{RUN_ID}_post_crash_{i:04d}"


def decode_value(raw):
    """Decode a raw bytes value from makoCon to a stripped string."""
    if raw is None:
        return None
    val = extract_user_value(raw)
    if val is None:
        return None
    try:
        return val.decode(errors="replace").strip()
    except AttributeError:
        return None


def report(test_id, name, passed, detail=""):
    status = "PASS" if passed else "FAIL"
    msg = f"[{status}] {test_id}: {name}"
    if detail:
        msg += f" -- {detail}"
    print(msg)
    RESULTS.append((test_id, name, passed, detail))


def find_server_pid_fallback():
    """
    Fall back to pgrep if get_server_pid() returns None.
    Searches for any process whose command line contains 'build/makoCon'.
    Returns an int PID or None.
    """
    try:
        output = subprocess.check_output(
            ["pgrep", "-f", "build/makoCon"],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
        if output:
            # pgrep may return multiple PIDs (one per line); take the first
            first = output.splitlines()[0].strip()
            return int(first)
    except (subprocess.CalledProcessError, ValueError):
        pass
    return None


# ── Main test ─────────────────────────────────────────────────────────────────

def test_6_6_crash_recovery():
    print("\n--- Task 6.6: Crash Recovery (in-memory durability) ---")

    # ── Step 1: ensure server is running ──────────────────────────────────────
    print("  [Step 1] Ensuring server is running...")
    ensure_server()

    r = get_client(socket_timeout=10)
    try:
        r.ping()
    except Exception as exc:
        report(
            "6.6",
            "Crash Recovery",
            False,
            f"Server not reachable before test: {exc}",
        )
        return
    print("  [Step 1] Server is up.")

    # ── Step 2: write 50 pre-crash keys ───────────────────────────────────────
    print(f"  [Step 2] Writing {NUM_KEYS} pre-crash keys (value={PRE_CRASH_VALUE})...")
    write_errors = 0
    for i in range(NUM_KEYS):
        try:
            r.set(crash_key(i), PRE_CRASH_VALUE)
        except Exception:
            write_errors += 1

    if write_errors > 0:
        print(f"  [Step 2] WARNING: {write_errors} write errors during setup.")
    else:
        print(f"  [Step 2] All {NUM_KEYS} keys written.")

    # ── Step 3: verify pre-crash keys are readable ────────────────────────────
    print(f"  [Step 3] Verifying pre-crash keys are readable...")
    pre_readable = 0
    for i in range(NUM_KEYS):
        try:
            raw = r.get(crash_key(i))
            val = decode_value(raw)
            if val == PRE_CRASH_VALUE:
                pre_readable += 1
        except Exception:
            pass

    print(f"  [Step 3] Pre-crash readable: {pre_readable}/{NUM_KEYS}")

    if pre_readable < NUM_KEYS:
        print(
            f"  [Step 3] WARNING: only {pre_readable}/{NUM_KEYS} keys readable "
            f"before crash — proceeding anyway."
        )

    r.close()

    # ── Step 4: SIGKILL the server ────────────────────────────────────────────
    print("  [Step 4] Locating server PID for SIGKILL...")
    pid = get_server_pid()
    if pid is None:
        print("  [Step 4] get_server_pid() returned None, trying pgrep fallback...")
        pid = find_server_pid_fallback()

    if pid is None:
        report(
            "6.6",
            "Crash Recovery",
            False,
            "Could not determine server PID; cannot send SIGKILL.",
        )
        return

    print(f"  [Step 4] Sending SIGKILL to PID {pid}...")
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        print(f"  [Step 4] WARNING: PID {pid} not found — server may have already exited.")
    except PermissionError as exc:
        report(
            "6.6",
            "Crash Recovery",
            False,
            f"Permission denied sending SIGKILL to PID {pid}: {exc}",
        )
        return

    # ── Step 5: wait, then restart ────────────────────────────────────────────
    print("  [Step 5] Waiting 1s for port to free...")
    time.sleep(1)

    print("  [Step 5] Starting fresh makoCon instance...")
    try:
        start_server()
    except SystemExit as exc:
        report(
            "6.6",
            "Crash Recovery",
            False,
            f"start_server() called sys.exit({exc.code}) — server failed to restart.",
        )
        return

    # ── Step 6: verify pre-crash keys are GONE ────────────────────────────────
    print(f"  [Step 6] Checking that {NUM_KEYS} pre-crash keys are absent...")
    r2 = get_client(socket_timeout=10)

    keys_still_present = 0
    for i in range(NUM_KEYS):
        try:
            raw = r2.get(crash_key(i))
            if raw is not None:
                # Value present — strip and check it isn't leftover
                val = decode_value(raw)
                if val is not None:
                    keys_still_present += 1
        except Exception:
            pass

    pre_crash_absent = (keys_still_present == 0)
    print(
        f"  [Step 6] Pre-crash keys still present after restart: "
        f"{keys_still_present}/{NUM_KEYS} "
        f"({'PASS: all gone' if pre_crash_absent else 'FAIL: data persisted unexpectedly'})"
    )

    # ── Step 7: write and verify 50 post-restart keys ─────────────────────────
    print(f"  [Step 7] Writing {NUM_KEYS} post-restart keys (value={POST_CRASH_VALUE})...")
    post_write_errors = 0
    for i in range(NUM_KEYS):
        try:
            r2.set(post_key(i), POST_CRASH_VALUE)
        except Exception:
            post_write_errors += 1

    print(f"  [Step 7] Verifying post-restart keys are readable...")
    post_readable = 0
    post_wrong    = 0
    for i in range(NUM_KEYS):
        try:
            raw = r2.get(post_key(i))
            val = decode_value(raw)
            if val == POST_CRASH_VALUE:
                post_readable += 1
            elif val is not None:
                post_wrong += 1
        except Exception:
            pass

    post_ok = (post_readable == NUM_KEYS) and (post_write_errors == 0)
    print(
        f"  [Step 7] Post-restart: "
        f"write_errors={post_write_errors}, "
        f"readable={post_readable}/{NUM_KEYS}, "
        f"wrong_value={post_wrong}"
    )

    r2.close()

    # ── Verdict ───────────────────────────────────────────────────────────────
    passed = pre_crash_absent and post_ok

    detail = (
        f"pre_crash_keys_written={NUM_KEYS - write_errors}, "
        f"pre_crash_readable={pre_readable}. "
        f"After SIGKILL+restart: "
        f"keys_still_present={keys_still_present} "
        f"(expected 0 — in-memory store). "
        f"post_restart_write_errors={post_write_errors}, "
        f"post_restart_readable={post_readable}/{NUM_KEYS}, "
        f"post_restart_wrong_value={post_wrong}."
    )

    if not pre_crash_absent:
        detail += (
            f" FAIL: {keys_still_present} pre-crash key(s) survived restart "
            f"— unexpected persistence detected."
        )
    if not post_ok:
        if post_write_errors > 0:
            detail += (
                f" FAIL: {post_write_errors} post-restart write(s) failed "
                f"— server may be unhealthy."
            )
        if post_readable < NUM_KEYS:
            detail += (
                f" FAIL: only {post_readable}/{NUM_KEYS} post-restart keys "
                f"readable after write."
            )

    report("6.6", "Crash Recovery", passed, detail)


def main():
    print("=" * 60)
    print("Mako Workload Test - Task 6.6: Crash Recovery")
    print(f"Run ID: {RUN_ID}")
    print("=" * 60)

    test_6_6_crash_recovery()

    print("\n" + "=" * 60)
    passed = sum(1 for _, _, p, _ in RESULTS if p)
    failed = sum(1 for _, _, p, _ in RESULTS if not p)
    print(f"Task 6.6 Summary: {passed} passed, {failed} failed, "
          f"{len(RESULTS)} total")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
