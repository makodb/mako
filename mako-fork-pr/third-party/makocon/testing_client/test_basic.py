#!/usr/bin/env python3
"""Simple test for basic SET/GET operations"""

import redis
import argparse

parser = argparse.ArgumentParser(description="Test basic operations")
parser.add_argument("--port", type=int, default=6380, help="Redis port")
args = parser.parse_args()

r = redis.Redis(host='localhost', port=args.port)

print("=== Test 1: PING ===")
try:
    response = r.ping()
    print(f"✓ PING: {response}")
except Exception as e:
    print(f"✗ PING failed: {e}")

print("\n=== Test 2: Simple SET/GET ===")
for i in range(3):
    key = f"key{i}"
    value = f"value{i}"
    try:
        r.set(key, value)
        result = r.get(key)
        if result == value.encode():
            print(f"✓ SET/GET {key}={value}")
        else:
            print(f"✗ SET/GET mismatch: expected {value}, got {result}")
    except Exception as e:
        print(f"✗ SET/GET failed for {key}: {e}")

print("\n=== Test 3: Simple MULTI/EXEC with 1 SET ===")
try:
    print("Sending MULTI...")
    r.execute_command("MULTI")
    print("Sending SET tx1 val1...")
    r.execute_command("SET", "tx1", "val1")
    print("Sending EXEC...")
    results = r.execute_command("EXEC")
    print(f"EXEC results: {results}")

    # Verify
    verify = r.get("tx1")
    print(f"Verification GET tx1: {verify}")
except Exception as e:
    print(f"✗ MULTI/EXEC failed: {e}")

print("\n=== All tests completed ===")
