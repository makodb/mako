from __future__ import annotations

import time
import uuid

import pytest
import redis


def key(name: str) -> str:
    return f"mako:test:zset:{uuid.uuid4()}:{name}"


def test_zset_basic_scores_and_ranges(mako_client: redis.Redis) -> None:
    name = key("basic")

    assert mako_client.zadd(name, {b"alice": 10.0, b"bob": 5.0, b"carol": 10.0}) == 3
    assert mako_client.zcard(name) == 3
    assert mako_client.zscore(name, b"bob") == 5.0
    assert mako_client.zrange(name, 0, -1) == [b"bob", b"alice", b"carol"]
    assert mako_client.zrange(name, 0, -1, withscores=True) == [
        (b"bob", 5.0),
        (b"alice", 10.0),
        (b"carol", 10.0),
    ]
    assert mako_client.zrevrange(name, 0, 1) == [b"carol", b"alice"]
    assert mako_client.zrangebyscore(name, 6, 10) == [b"alice", b"carol"]
    assert mako_client.zcount(name, 10, 10) == 2


def test_zset_update_modifiers_rank_and_pop(mako_client: redis.Redis) -> None:
    name = key("mutate")

    assert mako_client.zadd(name, {b"a": 1.0, b"b": 2.0}) == 2
    assert mako_client.execute_command("ZADD", name, "NX", 3, b"a") == 0
    assert mako_client.zscore(name, b"a") == 1.0
    assert mako_client.execute_command("ZADD", name, "XX", "CH", 3, b"a") == 1
    assert mako_client.zscore(name, b"a") == 3.0
    assert mako_client.zincrby(name, 2.0, b"b") == 4.0
    assert mako_client.zrank(name, b"a") == 0
    assert mako_client.zrevrank(name, b"b") == 0
    assert mako_client.zpopmin(name, 1) == [(b"a", 3.0)]
    assert mako_client.zpopmax(name, 1) == [(b"b", 4.0)]
    assert mako_client.zcard(name) == 0


def test_zadd_gt_lt_and_infinity_edge_cases(mako_client: redis.Redis) -> None:
    name = key("gt-lt-inf")

    assert mako_client.zadd(name, {b"foo": 10.0, b"x": 20.0, b"y": 30.0}) == 3
    assert mako_client.execute_command("ZADD", name, "GT", "CH", 5, b"foo", 21, b"x", 29, b"z") == 2
    assert mako_client.zscore(name, b"foo") == 10.0
    assert mako_client.zscore(name, b"x") == 21.0
    assert mako_client.zscore(name, b"z") == 29.0

    assert mako_client.execute_command("ZADD", name, "LT", "CH", 9, b"foo", 22, b"x", 31, b"new") == 2
    assert mako_client.zscore(name, b"foo") == 9.0
    assert mako_client.zscore(name, b"x") == 21.0
    assert mako_client.zscore(name, b"new") == 31.0

    assert mako_client.execute_command("ZADD", name, "+inf", b"pinf", "-inf", b"ninf") == 2
    assert mako_client.zrange(name, 0, 0) == [b"ninf"]
    assert mako_client.zrevrange(name, 0, 0) == [b"pinf"]
    assert mako_client.execute_command("ZADD", name, "LT", "INCR", 1, b"pinf") is None


def test_zset_type_ttl_delete_and_scan(mako_client: redis.Redis) -> None:
    name = key("typed")

    assert mako_client.zadd(name, {b"a": 1.0}) == 1
    assert mako_client.type(name) == b"zset"
    assert all(not item.startswith(b"\x01") for item in mako_client.keys("*"))
    assert mako_client.zscan(name, 0)[0] == 0
    assert mako_client.expire(name, 1) is True
    time.sleep(1.2)
    assert mako_client.type(name) == b"none"
    assert mako_client.zrange(name, 0, -1) == []
    assert mako_client.ttl(name) == -2


def test_zset_replaced_by_string_and_wrong_type(mako_client: redis.Redis) -> None:
    name = key("wrong-type")

    assert mako_client.zadd(name, {b"a": 1.0}) == 1
    assert mako_client.set(name, b"value") is True
    assert mako_client.type(name) == b"string"
    assert mako_client.get(name) == b"value"
    with pytest.raises(redis.ResponseError):
        mako_client.zrange(name, 0, -1)


def test_zset_commands_inside_multi(mako_client: redis.Redis) -> None:
    name = key("multi")

    pipe = mako_client.pipeline(transaction=True)
    pipe.zadd(name, {b"a": 1.0})
    pipe.zadd(name, {b"b": 2.0})
    pipe.zrange(name, 0, -1)
    pipe.zrem(name, b"a")
    pipe.zcard(name)

    assert pipe.execute() == [1, 1, [b"a", b"b"], 1, 1]


def test_immediate_expire_clears_staged_zset_inside_multi(mako_client: redis.Redis) -> None:
    name = key("multi-expire")

    pipe = mako_client.pipeline(transaction=True)
    pipe.zadd(name, {b"a": 1.0})
    pipe.expire(name, 0)
    pipe.exists(name)
    pipe.type(name)
    pipe.zrange(name, 0, -1)

    assert pipe.execute() == [1, True, 0, b"none", []]
