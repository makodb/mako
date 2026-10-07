from __future__ import annotations

import time
import uuid

import pytest
import redis


def key(name: str) -> str:
    return f"mako:test:hash:{uuid.uuid4()}:{name}"


def test_hash_set_get_update_and_delete(mako_client: redis.Redis) -> None:
    name = key("basic")

    assert mako_client.hset(name, b"field", b"value") == 1
    assert mako_client.hset(name, b"field", b"updated") == 0
    assert mako_client.hget(name, b"field") == b"updated"
    assert mako_client.hsetnx(name, b"field", b"ignored") == 0
    assert mako_client.hsetnx(name, b"other", b"value") == 1
    assert mako_client.hlen(name) == 2
    assert mako_client.hdel(name, b"field", b"missing") == 1
    assert mako_client.hget(name, b"field") is None
    assert mako_client.hlen(name) == 1


def test_hash_multi_get_and_enumeration(mako_client: redis.Redis) -> None:
    name = key("enum")

    assert mako_client.execute_command("HMSET", name, b"a", b"1", b"b", b"22") is True

    assert mako_client.hmget(name, [b"a", b"missing", b"b"]) == [b"1", None, b"22"]
    assert mako_client.hgetall(name) == {b"a": b"1", b"b": b"22"}
    assert set(mako_client.hkeys(name)) == {b"a", b"b"}
    assert set(mako_client.hvals(name)) == {b"1", b"22"}
    assert mako_client.hstrlen(name, b"b") == 2
    assert mako_client.hexists(name, b"missing") is False


def test_hash_numeric_commands(mako_client: redis.Redis) -> None:
    name = key("numeric")

    assert mako_client.hincrby(name, b"n", 2) == 2
    assert mako_client.hincrby(name, b"n", -1) == 1
    assert mako_client.hincrbyfloat(name, b"f", 1.5) == 1.5


def test_hash_scan_type_and_expiry(mako_client: redis.Redis) -> None:
    name = key("scan")
    assert mako_client.hset(name, mapping={b"a:1": b"one", b"a:2": b"two", b"b:1": b"three"}) == 3

    cursor, items = mako_client.hscan(name, 0, match="a:*", count=1)
    while cursor:
        next_cursor, next_items = mako_client.hscan(name, cursor, match="a:*", count=1)
        cursor = next_cursor
        items.update(next_items)
    assert items == {b"a:1": b"one", b"a:2": b"two"}
    assert mako_client.type(name) == b"hash"

    assert mako_client.pexpire(name, 1) is True
    time.sleep(0.02)
    assert mako_client.hlen(name) == 0
    assert mako_client.type(name) == b"none"


def test_hash_wrong_type_errors(mako_client: redis.Redis) -> None:
    name = key("wrongtype")
    assert mako_client.set(name, b"value") is True

    with pytest.raises(redis.ResponseError):
        mako_client.hget(name, b"field")
