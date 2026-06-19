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


def collect_hscan(
    client: redis.Redis, name: str, match: str = "*", count: int = 2
) -> dict[bytes, bytes]:
    cursor: int | bytes = 0
    out: dict[bytes, bytes] = {}
    for _ in range(100):
        cursor, items = client.hscan(name, cursor=cursor, match=match, count=count)
        out.update(items)
        if cursor == 0:
            return out
    raise AssertionError("HSCAN cursor did not terminate")


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


def test_hscan_supports_nonzero_cursor_and_match(mako_client: redis.Redis) -> None:
    name = key("hscan")
    assert mako_client.hset(
        name,
        mapping={
            b"a:1": b"one",
            b"a:2": b"two",
            b"b:1": b"three",
            b"a:3": b"four",
        },
    ) == 4

    assert collect_hscan(mako_client, name, match="a:*", count=1) == {
        b"a:1": b"one",
        b"a:2": b"two",
        b"a:3": b"four",
    }


def test_member_scans_return_empty_for_missing_keys(mako_client: redis.Redis) -> None:
    assert mako_client.sscan(key("missing-set"), 0) == (0, [])
    assert mako_client.zscan(key("missing-zset"), 0) == (0, [])
    assert mako_client.hscan(key("missing-hash"), 0) == (0, {})


def test_member_scans_reject_wrong_type(mako_client: redis.Redis) -> None:
    string_key = key("string")
    assert mako_client.set(string_key, b"value") is True

    with pytest.raises(redis.ResponseError):
        mako_client.sscan(string_key, 0)
    with pytest.raises(redis.ResponseError):
        mako_client.zscan(string_key, 0)
    with pytest.raises(redis.ResponseError):
        mako_client.hscan(string_key, 0)
