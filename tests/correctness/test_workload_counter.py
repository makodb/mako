#!/usr/bin/env python3
"""
Mako Workload Test - Task 6.3: Counter Service (High-Contention Single Key)
============================================================================
Tests makoCon as a counter with 10 concurrent clients each attempting
200 increment operations (read + write-back) on a single shared key.

Increment pattern used
----------------------
  1. Read counter value outside MULTI.
  2. MULTI { SET key (val + 1) } EXEC

Because each MULTI block contains only ONE SET (a write-only transaction),
the OCC read-set is empty and the transaction always commits — there is NO
server-side abort on write-only MULTI blocks.  This means two clients that
concurrently read the same value N will BOTH write N+1, resulting in a
"lost update":

  Client A: read=5, write 6
  Client B: read=5, write 6   ← lost update: net effect is +1, not +2

There is no WATCH/CAS command in makoCon, so single-key CAS is not
available.  This test documents the lost-update rate under high contention.

A retry loop (up to 10 retries per attempt) is included for completeness
and to handle genuine network errors, but because write-only MULTI/EXEC
never aborts via OCC, retries will not recover lost updates (they will
instead re-read the already-advanced counter and write a fresh +1, which
is correct behaviour for a retry).

Pass criteria
-------------
  - Final counter value is in [200, 2000].
  - Server is alive at the end.
  - The exact lost-update count is reported.
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

NUM_CLIENTS         = 10
INCREMENTS_PER_CLIENT = 200
EXPECTED_TOTAL      = NUM_CLIENTS * INCREMENTS_PER_CLIENT   # 2000
MAX_RETRIES         = 10
COUNTER_KEY_TEMPLATE = "{run_id}_counter"


# ── Helpers ───────────────────────────────────────────────────────────────────

def counter_key():
    return COUNTER_KEY_TEMPLATE.format(run_id=RUN_ID)


def read_counter(r):
    """Return the current integer value of the counter, or 0 if absent."""
    raw = r.get(counter_key())
    if raw is None:
        return 0
    val = extract_user_value(raw)
    if val is None:
        return 0
    try:
        return int(val.decode(errors="replace").strip())
    except (ValueError, AttributeError):
        return 0


def report(test_id, test_name, passed, detail=""):
    status = "PASS" if passed else "FAIL"
    msg = f"[{status}] {test_id}: {test_name}"
    if detail:
        msg += f" -- {detail}"
    print(msg)
    RESULTS.append((test_id, test_name, passed, detail))


# ── Increment with retry ───────────────────────────────────────────────────────

def increment_once(r):
    """
    Attempt a single counter increment with up to MAX_RETRIES retries.

    Protocol:
      1. Read current value outside MULTI (snapshot read).
      2. MULTI { SET key (val + 1) } EXEC

    Because the MULTI block is write-only (no GET inside MULTI), the OCC
    read-set is empty and the server will ALWAYS commit.  Retries therefore
    never recover from a lost update — they simply re-read the already-
    advanced value and write a new +1, which is still correct per-attempt
    behaviour.

    Returns: True if the increment was applied, False if all retries failed.
    """
    key = counter_key()
    for attempt in range(MAX_RETRIES):
        try:
            # Step 1: read outside MULTI
            current = read_counter(r)

            # Step 2: write-only MULTI/EXEC
            pipe = r.pipeline(transaction=True)
            pipe.set(key, str(current + 1))
            result = pipe.execute()

            # pipeline(transaction=True) raises on EXEC abort;
            # a falsy result list also signals failure.
            if result is None or len(result) == 0:
                # Unexpected abort — retry
                continue
            return True

        except Exception:
            # Connection error or OCC abort exception — retry
            if attempt < MAX_RETRIES - 1:
                try:
                    r.close()
                    r = get_client(socket_timeout=15, retry_on_timeout=True)
                except Exception:
                    pass
            continue

    return False


# ── Client worker ─────────────────────────────────────────────────────────────

def client_worker(client_id, stats):
    try:
        r = get_client(socket_timeout=15, retry_on_timeout=True)
    except Exception:
        stats[client_id] = {"attempted": 0, "succeeded": 0, "failed": 0}
        return

    attempted  = 0
    succeeded  = 0
    failed     = 0

    for _ in range(INCREMENTS_PER_CLIENT):
        attempted += 1
        ok = increment_once(r)
        if ok:
            succeeded += 1
        else:
            failed += 1

    try:
        r.close()
    except Exception:
        pass

    stats[client_id] = {
        "attempted":  attempted,
        "succeeded":  succeeded,
        "failed":     failed,
    }


# ── Main test ─────────────────────────────────────────────────────────────────

def test_6_3_counter():
    print("\n--- Task 6.3: Counter Service (High-Contention Single Key) ---")

    r_setup = get_client(socket_timeout=15)

    # Initialise counter to 0
    print(f"  [Init] Setting counter key to 0...")
    r_setup.set(counter_key(), "0")
    init_val = read_counter(r_setup)
    print(f"  [Init] Confirmed initial value: {init_val}")

    # Launch workers
    print(f"  [Run] {NUM_CLIENTS} clients × {INCREMENTS_PER_CLIENT} increments "
          f"= {EXPECTED_TOTAL} attempts...")
    stats   = {}
    threads = []

    t0 = time.time()
    for cid in range(NUM_CLIENTS):
        t = threading.Thread(
            target=client_worker, args=(cid, stats), daemon=True
        )
        t.start()
        threads.append(t)

    for t in threads:
        t.join(timeout=120)

    elapsed = time.time() - t0

    # Aggregate client stats
    total_attempted = sum(s["attempted"]  for s in stats.values())
    total_succeeded = sum(s["succeeded"]  for s in stats.values())
    total_failed    = sum(s["failed"]     for s in stats.values())

    # Read final counter value
    final_val = read_counter(r_setup)

    # Calculate lost updates
    # "succeeded" means the client's write-only MULTI committed (always true
    # unless there was a connection error). "lost" is the gap between the
    # number of successful increments and the final counter value — this
    # reflects concurrent reads of the same value (classic lost-update race).
    lost_updates = total_succeeded - final_val

    print(f"\n  [Results] Elapsed:         {elapsed:.1f}s")
    print(f"  [Results] Attempts:        {total_attempted:,}  (expected {EXPECTED_TOTAL:,})")
    print(f"  [Results] Succeeded:       {total_succeeded:,}")
    print(f"  [Results] Failed (error):  {total_failed:,}")
    print(f"  [Results] Final counter:   {final_val:,}  (max possible {EXPECTED_TOTAL:,})")
    print(f"  [Results] Lost updates:    {lost_updates:,}  "
          f"(due to concurrent read-then-write without CAS/WATCH)")
    print()
    print("  NOTE: With write-only MULTI/EXEC (empty OCC read-set), the server")
    print("  always commits. Lost updates arise from the application-level race:")
    print("  two clients read the same value N and both write N+1 (net: +1 not +2).")
    print("  No WATCH command is available in makoCon, so single-key CAS is")
    print("  not possible via the Redis-compatible interface.")

    # Server liveness check
    server_alive = True
    try:
        probe = get_client(socket_timeout=5)
        probe.ping()
        probe.close()
    except Exception:
        server_alive = False

    r_setup.close()

    # Pass criteria:
    #   - final_val in [200, 2000]: at least 10% of increments landed (generous
    #     lower bound given maximum contention), and no phantom writes
    #   - server_alive
    in_range = 200 <= final_val <= EXPECTED_TOTAL
    passed   = in_range and server_alive

    detail = (
        f"{total_attempted:,} attempts, {total_succeeded:,} succeeded, "
        f"{total_failed:,} errors. "
        f"final_counter={final_val:,}/{EXPECTED_TOTAL:,}. "
        f"lost_updates={lost_updates:,}. "
        f"server_alive={server_alive}."
    )
    if not in_range:
        detail += (
            f" FAIL: final_val={final_val} is outside expected range "
            f"[200, {EXPECTED_TOTAL}]."
        )
    if not server_alive:
        detail += " FAIL: server did not respond to PING after test."

    report("6.3", "Counter Service — High-Contention Single Key", passed, detail)


def main():
    print("=" * 60)
    print("Mako Workload Test - Task 6.3: Counter Service")
    print(f"Run ID: {RUN_ID}")
    print("=" * 60)

    ensure_server()
    test_6_3_counter()

    print("\n" + "=" * 60)
    passed = sum(1 for _, _, p, _ in RESULTS if p)
    failed = sum(1 for _, _, p, _ in RESULTS if not p)
    print(f"Task 6.3 Summary: {passed} passed, {failed} failed, "
          f"{len(RESULTS)} total")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
