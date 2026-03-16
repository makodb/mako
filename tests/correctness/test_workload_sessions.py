#!/usr/bin/env python3
"""
Mako Workload Test - Task 6.2: Session Store Simulation
========================================================
Tests makoCon as a session store with concurrent login, browse,
add-to-cart, and logout operations across 500 session slots.

Each session value is a JSON-like string of the form:
  {"session_id": "sXXX", "user": "user_XXX", "cart": [...], "status": "active"}

Single-key operations only (SET/GET/DEL on one key at a time) so the
known MULTI/EXEC multi-overwrite bug does not apply here.  The test
checks that every value that can be read has a valid JSON-like format
and that the server does not crash under a 30-second concurrent load.
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

NUM_SESSIONS   = 500
NUM_CLIENTS    = 20
RUN_SECS       = 30
CART_ITEMS     = ["itemA", "itemB", "itemC", "itemD", "itemE",
                  "itemF", "itemG", "itemH", "itemI", "itemJ"]


# ── Helpers ───────────────────────────────────────────────────────────────────

def session_key(i):
    return f"{RUN_ID}_sess_{i:04d}"


def make_session_value(i, cart=None, status="active"):
    cart_str = ", ".join(f'"{c}"' for c in (cart or []))
    return (
        f'{{"session_id": "s{i:04d}", "user": "user_{i:04d}", '
        f'"cart": [{cart_str}], "status": "{status}"}}'
    )


def is_valid_session(raw):
    """Return True if raw (bytes or None) decodes to a recognisable session."""
    if raw is None:
        return False
    try:
        val = extract_user_value(raw)
        if val is None:
            return False
        s = val.decode(errors="replace").strip()
        # Must look like our JSON-like format
        return (
            s.startswith("{")
            and '"session_id"' in s
            and '"user"' in s
            and '"cart"' in s
            and '"status"' in s
        )
    except Exception:
        return False


def report(test_id, test_name, passed, detail=""):
    status = "PASS" if passed else "FAIL"
    msg = f"[{status}] {test_id}: {test_name}"
    if detail:
        msg += f" -- {detail}"
    print(msg)
    RESULTS.append((test_id, test_name, passed, detail))


# ── Worker operations ─────────────────────────────────────────────────────────

def op_login(r, idx):
    """Create or overwrite a session (single-key SET)."""
    key = session_key(idx)
    val = make_session_value(idx)
    r.set(key, val)
    return "login"


def op_browse(r, idx):
    """Read a session and verify its format."""
    key = session_key(idx)
    raw = r.get(key)
    if raw is None:
        return "browse_missing"
    if is_valid_session(raw):
        return "browse_ok"
    return "browse_corrupt"


def op_add_to_cart(r, idx):
    """
    Read session, append a cart item, re-write with a single-key MULTI/EXEC.

    Single-key update: the known multi-overwrite bug does NOT apply because
    there is only one SET inside the MULTI block.
    """
    key = session_key(idx)
    raw = r.get(key)
    if raw is None:
        # Session doesn't exist yet — just create it with an item
        item = random.choice(CART_ITEMS)
        val = make_session_value(idx, cart=[item])
        r.set(key, val)
        return "add_to_cart_new"

    # Parse existing value
    try:
        s = extract_user_value(raw).decode(errors="replace").strip()
        # Extract cart list (simple string parse, not full JSON)
        cart_start = s.find('"cart": [') + len('"cart": [')
        cart_end   = s.find(']', cart_start)
        cart_body  = s[cart_start:cart_end].strip()
        if cart_body:
            existing_items = [
                c.strip().strip('"')
                for c in cart_body.split(",")
                if c.strip()
            ]
        else:
            existing_items = []
    except Exception:
        existing_items = []

    new_item = random.choice(CART_ITEMS)
    new_cart = existing_items + [new_item]

    new_val = make_session_value(idx, cart=new_cart)

    # Single-key MULTI/EXEC — safe from multi-overwrite bug
    pipe = r.pipeline(transaction=True)
    pipe.set(key, new_val)
    try:
        pipe.execute()
        return "add_to_cart_ok"
    except Exception:
        return "add_to_cart_abort"


def op_logout(r, idx):
    """Delete a session."""
    key = session_key(idx)
    r.delete(key)
    return "logout"


# ── Client worker ─────────────────────────────────────────────────────────────

OP_WEIGHTS = [
    ("login",        0.25),
    ("browse",       0.40),
    ("add_to_cart",  0.25),
    ("logout",       0.10),
]
OP_NAMES, OP_PROBS = zip(*OP_WEIGHTS)


def client_worker(client_id, stop_event, stats):
    try:
        r = get_client(socket_timeout=15, retry_on_timeout=True)
    except Exception:
        stats[client_id] = {
            "login": 0, "browse_ok": 0, "browse_missing": 0,
            "browse_corrupt": 0, "add_to_cart_ok": 0, "add_to_cart_new": 0,
            "add_to_cart_abort": 0, "logout": 0, "errors": 0,
        }
        return

    counts = {
        "login": 0, "browse_ok": 0, "browse_missing": 0,
        "browse_corrupt": 0, "add_to_cart_ok": 0, "add_to_cart_new": 0,
        "add_to_cart_abort": 0, "logout": 0, "errors": 0,
    }

    while not stop_event.is_set():
        idx = random.randint(0, NUM_SESSIONS - 1)
        op  = random.choices(OP_NAMES, weights=OP_PROBS, k=1)[0]
        try:
            if op == "login":
                result = op_login(r, idx)
            elif op == "browse":
                result = op_browse(r, idx)
            elif op == "add_to_cart":
                result = op_add_to_cart(r, idx)
            else:
                result = op_logout(r, idx)
            counts[result] = counts.get(result, 0) + 1
        except Exception:
            counts["errors"] += 1
            # Reconnect on connection error
            try:
                r.close()
                r = get_client(socket_timeout=15, retry_on_timeout=True)
            except Exception:
                pass

    try:
        r.close()
    except Exception:
        pass

    stats[client_id] = counts


# ── Integrity sweep ───────────────────────────────────────────────────────────

def check_all_sessions(r):
    """
    Read every session key and verify that any present value is valid.
    Returns (present_count, corrupt_count).
    """
    present  = 0
    corrupt  = 0
    for i in range(NUM_SESSIONS):
        raw = r.get(session_key(i))
        if raw is None:
            continue
        present += 1
        if not is_valid_session(raw):
            corrupt += 1
    return present, corrupt


# ── Main test ─────────────────────────────────────────────────────────────────

def test_6_2_session_store():
    print("\n--- Task 6.2: Session Store Simulation ---")

    r_setup = get_client(socket_timeout=15)

    # Initialize all sessions
    print(f"  [Init] Creating {NUM_SESSIONS} sessions...")
    for i in range(NUM_SESSIONS):
        r_setup.set(session_key(i), make_session_value(i))
    print(f"  [Init] Done.")

    # Run concurrent workload
    print(f"  [Run] {NUM_CLIENTS} clients for {RUN_SECS} seconds...")
    stop_event = threading.Event()
    stats      = {}
    workers    = []

    for cid in range(NUM_CLIENTS):
        t = threading.Thread(
            target=client_worker, args=(cid, stop_event, stats), daemon=True
        )
        t.start()
        workers.append(t)

    time.sleep(RUN_SECS)
    stop_event.set()

    for t in workers:
        t.join(timeout=15)

    # Aggregate stats
    total_logins       = sum(s.get("login", 0)            for s in stats.values())
    total_browse_ok    = sum(s.get("browse_ok", 0)        for s in stats.values())
    total_browse_miss  = sum(s.get("browse_missing", 0)   for s in stats.values())
    total_browse_corr  = sum(s.get("browse_corrupt", 0)   for s in stats.values())
    total_cart_ok      = sum(s.get("add_to_cart_ok", 0)   for s in stats.values())
    total_cart_new     = sum(s.get("add_to_cart_new", 0)  for s in stats.values())
    total_cart_abort   = sum(s.get("add_to_cart_abort", 0)for s in stats.values())
    total_logouts      = sum(s.get("logout", 0)           for s in stats.values())
    total_errors       = sum(s.get("errors", 0)           for s in stats.values())
    total_ops = (total_logins + total_browse_ok + total_browse_miss
                 + total_browse_corr + total_cart_ok + total_cart_new
                 + total_cart_abort + total_logouts)

    print(f"\n  [Stats] Operations completed: {total_ops:,}")
    print(f"    logins:         {total_logins:,}")
    print(f"    browse_ok:      {total_browse_ok:,}")
    print(f"    browse_missing: {total_browse_miss:,}  (session was deleted)")
    print(f"    browse_corrupt: {total_browse_corr:,}  (BAD if > 0)")
    print(f"    add_to_cart_ok: {total_cart_ok:,}")
    print(f"    add_to_cart_new:{total_cart_new:,}")
    print(f"    cart_aborts:    {total_cart_abort:,}")
    print(f"    logouts:        {total_logouts:,}")
    print(f"    errors:         {total_errors:,}")

    # Final integrity sweep
    print(f"\n  [Sweep] Scanning all {NUM_SESSIONS} session keys for corruption...")
    present, corrupt = check_all_sessions(r_setup)
    print(f"  [Sweep] present={present}, corrupt={corrupt}")

    r_setup.close()

    # Determine pass/fail
    corruption_detected = (total_browse_corr > 0) or (corrupt > 0)
    server_alive = True
    try:
        probe = get_client(socket_timeout=5)
        probe.ping()
        probe.close()
    except Exception:
        server_alive = False

    detail = (
        f"{total_ops:,} ops in {RUN_SECS}s across {NUM_CLIENTS} clients. "
        f"browse_corrupt={total_browse_corr}, final_corrupt={corrupt}/{present}. "
        f"errors={total_errors}. server_alive={server_alive}."
    )

    passed = (not corruption_detected) and server_alive
    if corruption_detected:
        detail += " DATA CORRUPTION DETECTED."
    if not server_alive:
        detail += " SERVER CRASHED."

    report("6.2", "Session Store", passed, detail)


def main():
    print("=" * 60)
    print("Mako Workload Test - Task 6.2: Session Store Simulation")
    print(f"Run ID: {RUN_ID}")
    print("=" * 60)

    ensure_server()
    test_6_2_session_store()

    print("\n" + "=" * 60)
    passed = sum(1 for _, _, p, _ in RESULTS if p)
    failed = sum(1 for _, _, p, _ in RESULTS if not p)
    print(f"Task 6.2 Summary: {passed} passed, {failed} failed, "
          f"{len(RESULTS)} total")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
