from __future__ import annotations

import time
import uuid

import pytest
import redis


def key(name: str) -> str:
    return f"mako:test:list:{uuid.uuid4()}:{name}"


def test_basic_push_pop_and_range(mako_client: redis.Redis) -> None:
    name = key("basic")

    assert mako_client.lpush(name, b"b", b"a") == 2
    assert mako_client.rpush(name, b"c", b"d") == 4
    assert mako_client.llen(name) == 4
    assert mako_client.lrange(name, 0, -1) == [b"a", b"b", b"c", b"d"]
    assert mako_client.lindex(name, 0) == b"a"
    assert mako_client.lindex(name, -1) == b"d"
    assert mako_client.lpop(name) == b"a"
    assert mako_client.rpop(name) == b"d"
    assert mako_client.lrange(name, 0, -1) == [b"b", b"c"]


def test_list_mutation_commands(mako_client: redis.Redis) -> None:
    name = key("mutate")

    assert mako_client.linsert(name, "BEFORE", b"missing", b"value") == 0
    assert mako_client.exists(name) == 0

    assert mako_client.rpush(name, b"a", b"b", b"c", b"b", b"d") == 5
    assert mako_client.lset(name, 1, b"B") is True
    assert mako_client.lrange(name, 0, -1) == [b"a", b"B", b"c", b"b", b"d"]
    assert mako_client.lrem(name, 1, b"b") == 1
    assert mako_client.lrange(name, 0, -1) == [b"a", b"B", b"c", b"d"]
    assert mako_client.linsert(name, "BEFORE", b"c", b"x") == 5
    assert mako_client.ltrim(name, 1, 3) is True
    assert mako_client.lrange(name, 0, -1) == [b"B", b"x", b"c"]


def test_pushx_lmove_rpoplpush_lpos(mako_client: redis.Redis) -> None:
    source = key("source")
    dest = key("dest")

    assert mako_client.lpushx(source, b"missing") == 0
    assert mako_client.rpush(source, b"a", b"b", b"c") == 3
    assert mako_client.lpushx(source, b"left") == 4
    assert mako_client.rpushx(source, b"right") == 5
    assert mako_client.lmove(source, dest, "RIGHT", "LEFT") == b"right"
    assert mako_client.rpoplpush(source, dest) == b"c"
    assert mako_client.lrange(source, 0, -1) == [b"left", b"a", b"b"]
    assert mako_client.lrange(dest, 0, -1) == [b"c", b"right"]
    assert mako_client.execute_command("LPOS", source, b"a") == 1
    assert mako_client.execute_command("LPOS", source, b"missing") is None


def test_list_type_ttl_and_delete(mako_client: redis.Redis) -> None:
    name = key("typed")

    assert mako_client.rpush(name, b"a") == 1
    assert mako_client.type(name) == b"list"
    assert all(not item.startswith(b"\x01") for item in mako_client.keys("*"))
    assert mako_client.expire(name, 1) is True
    time.sleep(1.2)
    assert mako_client.type(name) == b"none"
    assert mako_client.lrange(name, 0, -1) == []
    assert mako_client.ttl(name) == -2


def test_list_replaced_by_string_and_wrong_type(mako_client: redis.Redis) -> None:
    name = key("wrong-type")

    assert mako_client.rpush(name, b"a") == 1
    assert mako_client.set(name, b"value") is True
    assert mako_client.type(name) == b"string"
    assert mako_client.get(name) == b"value"
    with pytest.raises(redis.ResponseError):
        mako_client.lrange(name, 0, -1)

    with pytest.raises(redis.ResponseError):
        mako_client.rpush(name, b"b")
    assert mako_client.get(name) == b"value"


def test_list_commands_inside_multi(mako_client: redis.Redis) -> None:
    name = key("multi")

    pipe = mako_client.pipeline(transaction=True)
    pipe.rpush(name, b"a", b"b")
    pipe.lpush(name, b"z")
    pipe.lrange(name, 0, -1)
    pipe.lpop(name)
    pipe.llen(name)

    assert pipe.execute() == [2, 3, [b"z", b"a", b"b"], b"z", 2]


def test_immediate_expire_clears_staged_list_inside_multi(mako_client: redis.Redis) -> None:
    name = key("multi-expire")

    pipe = mako_client.pipeline(transaction=True)
    pipe.rpush(name, b"a")
    pipe.expire(name, 0)
    pipe.exists(name)
    pipe.type(name)
    pipe.lrange(name, 0, -1)

    assert pipe.execute() == [1, True, 0, b"none", []]
