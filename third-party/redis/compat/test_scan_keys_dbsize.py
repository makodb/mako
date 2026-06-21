from __future__ import annotations

import time
import uuid

import redis


def prefix() -> str:
    return f"mako:test:scan:{uuid.uuid4()}:"


def collect_scan(client: redis.Redis, match: str, count: int = 2) -> set[bytes]:
    cursor: int | bytes = 0
    out: set[bytes] = set()
    for _ in range(100):
        cursor, keys = client.scan(cursor=cursor, match=match, count=count)
        out.update(keys)
        if cursor == 0:
            return out
    raise AssertionError("SCAN cursor did not terminate")


def test_keys_uses_glob_pattern_and_hides_internal_keys(mako_client: redis.Redis) -> None:
    p = prefix()
    expected = {f"{p}a".encode(), f"{p}b".encode()}

    assert mako_client.set(f"{p}a", b"1") is True
    assert mako_client.set(f"{p}b", b"2") is True
    assert mako_client.set(f"{p}other", b"3") is True

    keys = set(mako_client.keys(f"{p}[ab]"))

    assert keys == expected
    assert all(not key.startswith(b"\x01") for key in keys)


def test_scan_returns_cursor_and_all_matching_keys(mako_client: redis.Redis) -> None:
    p = prefix()
    expected = {f"{p}{i}".encode() for i in range(5)}
    for key in expected:
        assert mako_client.set(key, b"value") is True

    assert collect_scan(mako_client, f"{p}*", count=2) == expected


def test_scan_skips_expired_keys(mako_client: redis.Redis) -> None:
    p = prefix()
    live = f"{p}live"
    expired = f"{p}expired"

    assert mako_client.set(live, b"value") is True
    assert mako_client.set(expired, b"value", px=1) is True
    time.sleep(0.02)

    keys = collect_scan(mako_client, f"{p}*", count=10)

    assert live.encode() in keys
    assert expired.encode() not in keys


def test_scan_type_non_string_returns_empty_result(mako_client: redis.Redis) -> None:
    p = prefix()
    assert mako_client.set(f"{p}a", b"1") is True

    cursor, keys = mako_client.execute_command("SCAN", 0, "MATCH", f"{p}*", "TYPE", "hash")

    assert cursor == 0
    assert keys == []


def test_dbsize_counts_user_visible_keys(mako_client: redis.Redis) -> None:
    p = prefix()
    before = mako_client.dbsize()

    assert mako_client.set(f"{p}a", b"1") is True
    assert mako_client.set(f"{p}b", b"2", ex=60) is True

    assert mako_client.dbsize() >= before + 2


def test_flushdb_removes_visible_and_composite_keys(mako_client: redis.Redis) -> None:
    p = prefix()
    string_key = f"{p}string"
    set_key = f"{p}set"
    list_key = f"{p}list"
    zset_key = f"{p}zset"
    hash_key = f"{p}hash"

    assert mako_client.set(string_key, b"value") is True
    assert mako_client.sadd(set_key, b"a") == 1
    assert mako_client.rpush(list_key, b"a") == 1
    assert mako_client.zadd(zset_key, {b"a": 1.0}) == 1
    assert mako_client.hset(hash_key, b"f", b"v") == 1

    assert mako_client.execute_command("FLUSHDB") is True

    assert mako_client.exists(string_key, set_key, list_key, zset_key, hash_key) == 0
    assert mako_client.keys(f"{p}*") == []


def test_flushall_large_keyspace_keeps_server_alive(mako_client: redis.Redis) -> None:
    p = prefix()
    pipe = mako_client.pipeline(transaction=False)
    key_count = 5_000
    for i in range(key_count):
        pipe.set(f"{p}{i}", b"value")
        if i % 500 == 499:
            pipe.execute()
    pipe.execute()

    assert mako_client.dbsize() >= key_count
    assert mako_client.execute_command("FLUSHALL") is True
    assert mako_client.ping() is True
    assert mako_client.dbsize() == 0


def test_hscan_missing_hash_returns_empty_result(mako_client: redis.Redis) -> None:
    assert mako_client.hscan("missing", 0) == (0, {})
