from __future__ import annotations

import uuid

import pytest
import redis


def key(name: str) -> str:
    return f"mako:test:scan-family:{uuid.uuid4()}:{name}"


def collect_sscan(client: redis.Redis, name: str, match: str = "*", count: int = 2) -> set[bytes]:
    cursor: int | bytes = 0
    out: set[bytes] = set()
    for _ in range(100):
        cursor, members = client.sscan(name, cursor=cursor, match=match, count=count)
        out.update(members)
        if cursor == 0:
            return out
    raise AssertionError("SSCAN cursor did not terminate")


def collect_zscan(
    client: redis.Redis, name: str, match: str = "*", count: int = 2
) -> dict[bytes, float]:
    cursor: int | bytes = 0
    out: dict[bytes, float] = {}
    for _ in range(100):
        cursor, items = client.zscan(name, cursor=cursor, match=match, count=count)
        out.update(items)
        if cursor == 0:
            return out
    raise AssertionError("ZSCAN cursor did not terminate")


def test_sscan_returns_paginated_matching_members(mako_client: redis.Redis) -> None:
    name = key("sscan")
    assert mako_client.sadd(name, b"a:1", b"a:2", b"b:1", b"a:3") == 4

    assert collect_sscan(mako_client, name, match="a:*", count=1) == {
        b"a:1",
        b"a:2",
        b"a:3",
    }


def test_zscan_supports_nonzero_cursor_and_match(mako_client: redis.Redis) -> None:
    name = key("zscan")
    assert mako_client.zadd(
        name,
        {
            b"a:1": 1.0,
            b"a:2": 2.0,
            b"b:1": 3.0,
            b"a:3": 4.0,
        },
    ) == 4

    assert collect_zscan(mako_client, name, match="a:*", count=1) == {
        b"a:1": 1.0,
        b"a:2": 2.0,
        b"a:3": 4.0,
    }


def test_hscan_remains_blocked_until_hash_storage_exists(mako_client: redis.Redis) -> None:
    with pytest.raises(redis.ResponseError, match="hash command storage"):
        mako_client.hscan(key("hash"), 0)
