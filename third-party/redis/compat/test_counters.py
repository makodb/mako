from __future__ import annotations

import uuid

import pytest
import redis


def key(name: str) -> str:
    return f"mako:test:counters:{uuid.uuid4()}:{name}"


def test_integer_counter_commands(mako_client: redis.Redis) -> None:
    name = key("int")

    assert mako_client.incr(name) == 1
    assert mako_client.incrby(name, 4) == 5
    assert mako_client.decr(name) == 4
    assert mako_client.decrby(name, 3) == 1
    assert mako_client.get(name) == b"1"


def test_counter_rejects_non_integer_value(mako_client: redis.Redis) -> None:
    name = key("bad-int")

    assert mako_client.set(name, b"not-an-int") is True
    with pytest.raises(redis.ResponseError):
        mako_client.incr(name)


def test_incrbyfloat(mako_client: redis.Redis) -> None:
    name = key("float")

    assert mako_client.incrbyfloat(name, 1.5) == 1.5
    assert mako_client.incrbyfloat(name, 2.25) == 3.75
    assert mako_client.get(name) == b"3.75"


def test_incrbyfloat_decimal_format(mako_client: redis.Redis) -> None:
    name = key("float-format")

    assert mako_client.execute_command("INCRBYFLOAT", name, "0.1") == 0.1
    assert mako_client.execute_command("INCRBYFLOAT", name, "0.2") == 0.3
    assert mako_client.get(name) == b"0.3"
