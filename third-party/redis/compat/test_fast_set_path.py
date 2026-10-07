from __future__ import annotations

import concurrent.futures
import os
import threading
import time
import uuid

import pytest
import redis


def key(name: str) -> str:
    return f"mako:test:fast-set:{uuid.uuid4()}:{name}"


def cache_metrics(client: redis.Redis) -> dict[str, int]:
    info = client.info("mako")
    return {
        name: int(info.get(name, 0))
        for name in (
            "mako_cache_enabled",
            "mako_cache_hits",
            "mako_cache_misses",
            "mako_cache_inserts",
            "mako_cache_invalidations",
            "mako_txn_commits",
        )
    }


def require_cache(client: redis.Redis) -> None:
    if cache_metrics(client)["mako_cache_enabled"] != 1:
        pytest.skip("start makoCon with MAKO_REDIS_CACHE_MB > 0")


def test_cache_hit_bypasses_mako_and_generic_write_invalidates(
    mako_client: redis.Redis,
) -> None:
    require_cache(mako_client)
    name = key("cache-hit")
    assert mako_client.set(name, b"v1") is True

    # The first read populates after a generic SET of a new key. Subsequent
    # reads must be served without creating Mako transactions.
    assert mako_client.get(name) == b"v1"
    before = cache_metrics(mako_client)
    for _ in range(100):
        assert mako_client.get(name) == b"v1"
    after = cache_metrics(mako_client)
    assert after["mako_cache_hits"] - before["mako_cache_hits"] == 100
    assert after["mako_txn_commits"] == before["mako_txn_commits"]

    # APPEND takes the generic transaction path. It must invalidate only after
    # commit; the next GET reloads the new value rather than returning v1.
    assert mako_client.append(name, b"-v2") == 5
    invalidated = cache_metrics(mako_client)
    assert invalidated["mako_cache_invalidations"] > after["mako_cache_invalidations"]
    assert mako_client.get(name) == b"v1-v2"


def test_cache_respects_ttl_and_multi_exec(mako_client: redis.Redis) -> None:
    require_cache(mako_client)
    expiring = key("cache-ttl")
    assert mako_client.set(expiring, b"short", px=40) is True
    before = cache_metrics(mako_client)
    assert mako_client.get(expiring) == b"short"
    assert mako_client.get(expiring) == b"short"
    after_reads = cache_metrics(mako_client)
    assert after_reads["mako_cache_hits"] == before["mako_cache_hits"]
    time.sleep(0.08)
    assert mako_client.get(expiring) is None

    transactional = key("cache-multi")
    assert mako_client.set(transactional, b"old") is True
    assert mako_client.get(transactional) == b"old"
    with mako_client.pipeline(transaction=True) as pipe:
        result = pipe.set(transactional, b"new").get(transactional).execute()
    assert result == [True, b"new"]
    assert mako_client.get(transactional) == b"new"


@pytest.mark.parametrize("kind", ["set", "hash", "list", "zset"])
def test_plain_set_replaces_collection_without_resurrection(
    mako_client: redis.Redis, kind: str
) -> None:
    """A missing string record must fall back and remove every collection record."""
    name = key(kind)
    if kind == "set":
        assert mako_client.sadd(name, b"member") == 1
    elif kind == "hash":
        assert mako_client.hset(name, b"field", b"value") == 1
    elif kind == "list":
        assert mako_client.lpush(name, b"item") == 1
    else:
        assert mako_client.zadd(name, {b"member": 1.0}) == 1

    # This is a syntactically plain SET. The raw fast executor must detect that
    # table_key_ is absent and delegate cross-type cleanup to the generic path.
    assert mako_client.set(name, b"string-value") is True
    assert mako_client.type(name) == b"string"
    assert mako_client.get(name) == b"string-value"

    # Expire the string and force fast GET to remove its string and TTL records.
    # Any collection records left beside table_key_ would now resurrect.
    assert mako_client.pexpire(name, 20) is True
    time.sleep(0.05)
    assert mako_client.get(name) is None
    assert mako_client.type(name) == b"none"
    if kind == "set":
        assert mako_client.smembers(name) == set()
    elif kind == "hash":
        assert mako_client.hgetall(name) == {}
    elif kind == "list":
        assert mako_client.lrange(name, 0, -1) == []
    else:
        assert mako_client.zrange(name, 0, -1) == []


def test_plain_fast_set_clears_existing_ttl(mako_client: redis.Redis) -> None:
    name = key("ttl")
    assert mako_client.set(name, b"old", px=80) is True

    # Existing strings use execute_fast_mako_string(), which must clear TTL.
    assert mako_client.set(name, b"new") is True
    assert mako_client.pttl(name) == -1
    time.sleep(0.12)
    assert mako_client.get(name) == b"new"


def test_concurrent_plain_fast_set_get_never_returns_torn_value() -> None:
    host = os.environ.get("MAKO_REDIS_HOST", "127.0.0.1")
    port = int(os.environ.get("MAKO_REDIS_PORT", "6380"))
    name = key("concurrent")
    seed = b"seed:" + b"S" * 4096
    values = [
        f"writer:{writer}:".encode() + bytes([65 + writer]) * 4096
        for writer in range(4)
    ]
    allowed = {seed, *values}
    setup = redis.Redis(host=host, port=port, decode_responses=False)
    assert setup.set(name, seed) is True

    barrier = threading.Barrier(8)

    def writer(value: bytes) -> None:
        client = redis.Redis(host=host, port=port, decode_responses=False)
        try:
            barrier.wait()
            for _ in range(500):
                assert client.set(name, value) is True
        finally:
            client.close()

    def reader() -> None:
        client = redis.Redis(host=host, port=port, decode_responses=False)
        try:
            barrier.wait()
            for _ in range(500):
                assert client.get(name) in allowed
        finally:
            client.close()

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        futures = [pool.submit(writer, value) for value in values]
        futures.extend(pool.submit(reader) for _ in range(4))
        for future in futures:
            future.result(timeout=30)

    assert setup.get(name) in allowed
    setup.delete(name)
    setup.close()
