#!/usr/bin/env python3
"""Fill the optional string cache past capacity and persist its invariants."""

from __future__ import annotations

import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path

import redis


def cache_metrics(client: redis.Redis) -> dict[str, int]:
    info = client.info("mako")
    names = (
        "mako_cache_enabled",
        "mako_cache_capacity_bytes",
        "mako_cache_entries",
        "mako_cache_bytes",
        "mako_cache_hits",
        "mako_cache_misses",
        "mako_cache_inserts",
        "mako_cache_evictions",
        "mako_cache_invalidations",
    )
    return {name: int(info[name]) for name in names}


def main() -> None:
    host = os.environ.get("MAKO_CACHE_STRESS_HOST", "127.0.0.1")
    port = int(os.environ.get("MAKO_CACHE_STRESS_PORT", "6380"))
    keys = int(os.environ.get("MAKO_CACHE_STRESS_KEYS", "200000"))
    batch_size = int(os.environ.get("MAKO_CACHE_STRESS_BATCH", "1000"))
    out = Path(os.environ.get("MAKO_CACHE_STRESS_OUT", "cache_eviction_stress.json"))
    if keys < 1 or batch_size < 1:
        raise SystemExit("key and batch counts must be positive")

    client = redis.Redis(host=host, port=port, decode_responses=False)
    if not client.ping():
        raise SystemExit(f"server did not answer PING at {host}:{port}")
    before = cache_metrics(client)
    if before["mako_cache_enabled"] != 1:
        raise SystemExit("cache is not enabled")

    prefix = f"mako:cache-eviction:{time.time_ns()}:".encode()
    value = b"eviction-value"
    started = time.monotonic()
    for start in range(0, keys, batch_size):
        stop = min(keys, start + batch_size)
        pipe = client.pipeline(transaction=False)
        for index in range(start, stop):
            pipe.set(prefix + str(index).encode(), value)
        assert all(result is True for result in pipe.execute())

        pipe = client.pipeline(transaction=False)
        for index in range(start, stop):
            pipe.get(prefix + str(index).encode())
        assert all(result == value for result in pipe.execute())

    # Re-read keys across the insertion order. Evicted entries must fall back to
    # Mako and still return the correct value before being admitted again.
    sample_indices = sorted({0, keys // 4, keys // 2, keys * 3 // 4, keys - 1})
    for index in sample_indices:
        assert client.get(prefix + str(index).encode()) == value

    after = cache_metrics(client)
    elapsed = time.monotonic() - started
    assert after["mako_cache_bytes"] <= after["mako_cache_capacity_bytes"]
    assert after["mako_cache_evictions"] > before["mako_cache_evictions"]
    assert after["mako_cache_entries"] < keys

    result = {
        "status": "pass",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "server": {"host": host, "port": port},
        "keys_written_and_read": keys,
        "batch_size": batch_size,
        "value_bytes": len(value),
        "elapsed_seconds": elapsed,
        "sample_indices_revalidated": sample_indices,
        "metrics_before": before,
        "metrics_after": after,
        "assertions": [
            "cache bytes never exceeded configured capacity at final observation",
            "forced fill produced at least one eviction",
            "evicted keys remained correct through Mako fallback",
        ],
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(out)


if __name__ == "__main__":
    main()
