#!/usr/bin/env python3
"""
Simple test for MULTI/EXEC transaction support in makoCon
Only tests SET and GET operations (what we support)
"""

import redis
import argparse

parser = argparse.ArgumentParser(description="Test MULTI/EXEC transactions")
parser.add_argument("--port", type=int, default=6380, help="Redis port")
args = parser.parse_args()

r = redis.Redis(host='localhost', port=args.port)

# Test 1: Basic PING
print("=== Test 1: PING ===")
try:
    response = r.ping()
    print(f"PING response: {response}")
except Exception as e:
    print(f"PING failed: {e}")

# Test 2: Basic SET/GET (non-transactional)
print("\n=== Test 2: Basic SET/GET ===")
try:
    r.set("test_key", "test_value")
    value = r.get("test_key")
    print(f"SET test_key = test_value")
    print(f"GET test_key = {value}")
except Exception as e:
    print(f"Basic SET/GET failed: {e}")

# Test 3: Direct MULTI/EXEC with SET operations
print("\n=== Test 3: Direct MULTI/EXEC (SET only) ===")
try:
    r.execute_command("MULTI")
    print("MULTI sent")

    r.execute_command("SET", "tx:key1", "value1")
    print("SET tx:key1 value1 -> QUEUED")

    r.execute_command("SET", "tx:key2", "value2")
    print("SET tx:key2 value2 -> QUEUED")

    r.execute_command("SET", "tx:key3", "value3")
    print("SET tx:key3 value3 -> QUEUED")

    results = r.execute_command("EXEC")
    print(f"EXEC results: {results}")

    # Verify the values were set
    v1 = r.get("tx:key1")
    v2 = r.get("tx:key2")
    v3 = r.get("tx:key3")
    print(f"Verification: tx:key1={v1}, tx:key2={v2}, tx:key3={v3}")

except Exception as e:
    print(f"MULTI/EXEC (SET) failed: {e}")

# Test 4: MULTI/EXEC with GET operations
print("\n=== Test 4: Direct MULTI/EXEC (GET only) ===")
try:
    r.execute_command("MULTI")
    print("MULTI sent")

    r.execute_command("GET", "tx:key1")
    print("GET tx:key1 -> QUEUED")

    r.execute_command("GET", "tx:key2")
    print("GET tx:key2 -> QUEUED")

    results = r.execute_command("EXEC")
    print(f"EXEC results: {results}")

except Exception as e:
    print(f"MULTI/EXEC (GET) failed: {e}")

# Test 5: MULTI/EXEC with mixed SET and GET
print("\n=== Test 5: Direct MULTI/EXEC (mixed SET/GET) ===")
try:
    r.execute_command("MULTI")
    print("MULTI sent")

    r.execute_command("SET", "tx:mixed1", "mixed_value1")
    print("SET tx:mixed1 mixed_value1 -> QUEUED")

    r.execute_command("GET", "tx:key1")
    print("GET tx:key1 -> QUEUED")

    r.execute_command("SET", "tx:mixed2", "mixed_value2")
    print("SET tx:mixed2 mixed_value2 -> QUEUED")

    r.execute_command("GET", "tx:mixed1")
    print("GET tx:mixed1 -> QUEUED (should get value from same transaction)")

    results = r.execute_command("EXEC")
    print(f"EXEC results: {results}")

except Exception as e:
    print(f"MULTI/EXEC (mixed) failed: {e}")

# Test 6: DISCARD transaction
print("\n=== Test 6: MULTI/DISCARD ===")
try:
    r.set("discard_test", "original")
    print(f"Initial: discard_test = {r.get('discard_test')}")

    r.execute_command("MULTI")
    print("MULTI sent")

    r.execute_command("SET", "discard_test", "should_not_persist")
    print("SET discard_test should_not_persist -> QUEUED")

    result = r.execute_command("DISCARD")
    print(f"DISCARD result: {result}")

    # Verify the value was NOT changed
    final = r.get("discard_test")
    print(f"Final: discard_test = {final}")
    if final == b"original":
        print("✓ DISCARD worked correctly!")
    else:
        print("✗ DISCARD failed - value was changed!")

except Exception as e:
    print(f"MULTI/DISCARD failed: {e}")

# Test 7: Empty transaction
print("\n=== Test 7: Empty MULTI/EXEC ===")
try:
    r.execute_command("MULTI")
    print("MULTI sent")

    results = r.execute_command("EXEC")
    print(f"EXEC (empty) results: {results}")

except Exception as e:
    print(f"Empty MULTI/EXEC failed: {e}")

print("\n=== All tests completed ===")
