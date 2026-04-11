#!/usr/bin/env python3
"""
Mako Workload Test - Task 6.4: Message Queue Simulation
========================================================
Tests makoCon as a simple message queue with 5 producer threads and
3 consumer threads passing 1000 total messages (5 × 200 per producer).

Design works around known server limitations:
  - MULTI/EXEC multi-SET-overwrite bug: avoided entirely — each producer
    uses simple r.set() with a unique, never-repeated key.
  - No WATCH/CAS: not needed — each message key is written once by exactly
    one producer, so there is no race on the write side.
  - DEL always returns 1: accepted — consumers call r.delete() for
    bookkeeping only; correctness is verified by checking key absence.

Pass criteria:
  - All 1000 messages produced and consumed (zero corrupt values).
  - All 1000 message keys absent after the run.
  - Server responds to PING at the end.
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
NUM_PRODUCERS = 5
MSGS_PER_PRODUCER = 200
NUM_CONSUMERS = 3

# Derived constant — used in assertions and reporting.
TOTAL_MESSAGES = NUM_PRODUCERS * MSGS_PER_PRODUCER  # 1000

# Consumers retry GET up to this many times before giving up on a message.
MAX_POLL_RETRIES = 30
POLL_SLEEP_S = 0.5


# ── Helpers ────────────────────────────────────────────────────────────────────

def report(test_id, name, passed, detail=""):
    status = "PASS" if passed else "FAIL"
    msg = f"[{status}] {test_id}: {name}"
    if detail:
        msg += f" -- {detail}"
    print(msg)
    RESULTS.append((test_id, name, passed, detail))


def msg_key(p, n):
    """Return the canonical key for producer p's message number n."""
    return f"{RUN_ID}_msg_{p:02d}_{n:04d}"


def expected_msg(p, n):
    """Return the expected message value string for producer p, seq n."""
    return f"MSG:p{p:02d}:n{n:04d}:payload_{n * 7 + 13}"


def assign_partitions():
    """
    Pre-compute consumer partition assignments.

    Each of the 1000 message keys is assigned to exactly one consumer based
    on hash(key) % NUM_CONSUMERS.  Returns a list of NUM_CONSUMERS lists,
    where each inner list contains (producer_id, seq) tuples.
    """
    partitions = [[] for _ in range(NUM_CONSUMERS)]
    for p in range(NUM_PRODUCERS):
        for n in range(MSGS_PER_PRODUCER):
            key = msg_key(p, n)
            consumer_id = hash(key) % NUM_CONSUMERS
            partitions[consumer_id].append((p, n))
    return partitions


# ── Producer worker ────────────────────────────────────────────────────────────

def producer_worker(pid, stats):
    """
    Write MSGS_PER_PRODUCER messages to the server.

    Each message uses a unique key that is written exactly once, so simple
    r.set() is safe — no MULTI/EXEC needed and the multi-overwrite bug
    does not apply.

    stats[pid] is set to a dict with:
      - "produced": number of successfully written messages
      - "errors":   number of SET failures
    """
    try:
        r = get_client(socket_timeout=15, retry_on_timeout=True)
    except Exception:
        stats[pid] = {"produced": 0, "errors": MSGS_PER_PRODUCER}
        return

    produced = 0
    errors = 0

    for n in range(MSGS_PER_PRODUCER):
        key = msg_key(pid, n)
        value = expected_msg(pid, n)
        try:
            r.set(key, value)
            produced += 1
        except Exception:
            errors += 1
            # Attempt reconnect and retry once.
            try:
                r.close()
                r = get_client(socket_timeout=15, retry_on_timeout=True)
                r.set(key, value)
                produced += 1
                errors -= 1
            except Exception:
                pass

    try:
        r.close()
    except Exception:
        pass

    stats[pid] = {"produced": produced, "errors": errors}


# ── Consumer worker ────────────────────────────────────────────────────────────

def consumer_worker(cid, assigned_keys, stats):
    """
    Consume all messages in assigned_keys.

    For each (producer_id, seq) pair:
      1. Poll r.get(key) with up to MAX_POLL_RETRIES retries (POLL_SLEEP_S
         between attempts) until the message appears.
      2. Verify the value matches expected_msg(p, n).
      3. Call r.delete(key) to mark the message consumed.

    stats[cid] is set to a dict with:
      - "consumed":  number of successfully consumed messages
      - "corrupt":   number of messages with wrong content
      - "timeout":   number of messages that never appeared
      - "errors":    number of unexpected exceptions
    """
    try:
        r = get_client(socket_timeout=15, retry_on_timeout=True)
    except Exception:
        stats[cid] = {
            "consumed": 0,
            "corrupt": 0,
            "timeout": len(assigned_keys),
            "errors": 0,
        }
        return

    consumed = 0
    corrupt = 0
    timeout = 0
    errors = 0

    for (p, n) in assigned_keys:
        key = msg_key(p, n)
        expected = expected_msg(p, n)
        found = False

        for attempt in range(MAX_POLL_RETRIES):
            try:
                raw = r.get(key)
            except Exception:
                errors += 1
                try:
                    r.close()
                    r = get_client(socket_timeout=15, retry_on_timeout=True)
                except Exception:
                    pass
                time.sleep(POLL_SLEEP_S)
                continue

            if raw is None:
                # Message not yet written by producer — wait and retry.
                time.sleep(POLL_SLEEP_S)
                continue

            # Message arrived — verify content.
            actual = extract_user_value(raw)
            if actual is not None:
                actual_str = actual.decode(errors="replace").strip()
            else:
                actual_str = ""

            if actual_str == expected:
                consumed += 1
            else:
                corrupt += 1

            # Mark consumed (DEL always returns 1, so we don't check return).
            try:
                r.delete(key)
            except Exception:
                pass

            found = True
            break

        if not found:
            timeout += 1

    try:
        r.close()
    except Exception:
        pass

    stats[cid] = {
        "consumed": consumed,
        "corrupt": corrupt,
        "timeout": timeout,
        "errors": errors,
    }


# ── Main test ──────────────────────────────────────────────────────────────────

def test_6_4_message_queue():
    print("\n--- Task 6.4: Message Queue Simulation ---")

    # Pre-compute partition assignments before any threads start.
    partitions = assign_partitions()
    partition_sizes = [len(p) for p in partitions]
    print(f"  [Init] Partition sizes: {partition_sizes} (total={sum(partition_sizes)})")

    # ── Phase 1: start producers and consumers concurrently ─────────────────
    print(f"  [Run] Launching {NUM_PRODUCERS} producers "
          f"({MSGS_PER_PRODUCER} msgs each) and {NUM_CONSUMERS} consumers...")

    producer_stats = {}
    consumer_stats = {}
    threads = []

    for pid in range(NUM_PRODUCERS):
        t = threading.Thread(
            target=producer_worker,
            args=(pid, producer_stats),
            daemon=True,
        )
        t.start()
        threads.append(t)

    for cid in range(NUM_CONSUMERS):
        t = threading.Thread(
            target=consumer_worker,
            args=(cid, partitions[cid], consumer_stats),
            daemon=True,
        )
        t.start()
        threads.append(t)

    # Wait for all threads. Consumer timeout bound:
    # 30 retries × 0.5s = 15s per message. With sequential iteration that is
    # the theoretical worst case; in practice producers finish quickly.
    for t in threads:
        t.join(timeout=60)

    # ── Phase 2: aggregate results ───────────────────────────────────────────
    total_produced = sum(s["produced"] for s in producer_stats.values())
    total_prod_err = sum(s["errors"]   for s in producer_stats.values())

    total_consumed  = sum(s["consumed"]  for s in consumer_stats.values())
    total_corrupt   = sum(s["corrupt"]   for s in consumer_stats.values())
    total_timeout   = sum(s["timeout"]   for s in consumer_stats.values())
    total_cons_err  = sum(s["errors"]    for s in consumer_stats.values())

    print(f"\n  [Producers] produced={total_produced}/{TOTAL_MESSAGES}, "
          f"errors={total_prod_err}")
    print(f"  [Consumers] consumed={total_consumed}/{TOTAL_MESSAGES}, "
          f"corrupt={total_corrupt}, timeout={total_timeout}, "
          f"errors={total_cons_err}")

    # ── Phase 3: verify all keys are gone (consumed) ─────────────────────────
    print(f"\n  [Verify] Checking all {TOTAL_MESSAGES} keys are absent...")
    r_verify = get_client(socket_timeout=15)
    still_present = 0
    for p in range(NUM_PRODUCERS):
        for n in range(MSGS_PER_PRODUCER):
            try:
                raw = r_verify.get(msg_key(p, n))
                if raw is not None:
                    still_present += 1
            except Exception:
                pass
    try:
        r_verify.close()
    except Exception:
        pass
    print(f"  [Verify] Keys still present after consumption: {still_present} "
          f"(expected 0)")

    # ── Phase 4: server liveness ──────────────────────────────────────────────
    server_alive = True
    try:
        r_ping = get_client(socket_timeout=5)
        r_ping.ping()
        r_ping.close()
    except Exception:
        server_alive = False

    # ── Pass/fail decision ────────────────────────────────────────────────────
    all_produced  = (total_produced == TOTAL_MESSAGES)
    all_consumed  = (total_consumed == TOTAL_MESSAGES)
    zero_corrupt  = (total_corrupt == 0)
    zero_leftover = (still_present == 0)

    passed = all_produced and all_consumed and zero_corrupt and zero_leftover and server_alive

    detail_parts = [
        f"produced={total_produced}/{TOTAL_MESSAGES}",
        f"consumed={total_consumed}/{TOTAL_MESSAGES}",
        f"corrupt={total_corrupt}",
        f"timeout={total_timeout}",
        f"leftover_keys={still_present}",
        f"prod_errors={total_prod_err}",
        f"cons_errors={total_cons_err}",
        f"server_alive={server_alive}",
    ]
    detail = ", ".join(detail_parts)

    if not all_produced:
        detail += f". FAIL: only {total_produced}/{TOTAL_MESSAGES} messages produced."
    if not all_consumed:
        detail += f". FAIL: only {total_consumed}/{TOTAL_MESSAGES} messages consumed."
    if not zero_corrupt:
        detail += f". FAIL: {total_corrupt} corrupt message(s) detected."
    if not zero_leftover:
        detail += f". FAIL: {still_present} key(s) still present after run."
    if not server_alive:
        detail += ". FAIL: server did not respond to PING."

    report("6.4", "Message Queue", passed, detail)


def main():
    print("=" * 60)
    print("Mako Workload Test - Task 6.4: Message Queue Simulation")
    print(f"Run ID: {RUN_ID}")
    print("=" * 60)

    ensure_server()
    test_6_4_message_queue()

    print("\n" + "=" * 60)
    passed = sum(1 for _, _, p, _ in RESULTS if p)
    failed = sum(1 for _, _, p, _ in RESULTS if not p)
    print(f"Task 6.4 Summary: {passed} passed, {failed} failed, "
          f"{len(RESULTS)} total")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
