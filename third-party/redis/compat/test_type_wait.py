from __future__ import annotations

import time
import uuid

import pytest
import redis


def key(name: str) -> str:
    return f"mako:test:type-wait:{uuid.uuid4()}:{name}"


def test_type_reports_string_none_and_expired_none(mako_client: redis.Redis) -> None:
    name = key("type")
    expired = key("expired")

    assert mako_client.type(name) == b"none"
    assert mako_client.set(name, b"value") is True
    assert mako_client.type(name) == b"string"

    assert mako_client.set(expired, b"value", px=10) is True
    time.sleep(0.2)
    assert mako_client.type(expired) == b"none"


def test_wait_zero_zero_returns_zero(mako_client: redis.Redis) -> None:
    assert mako_client.execute_command("WAIT", 0, 0) == 0


def test_wait_queues_inside_multi(mako_client: redis.Redis) -> None:
    pipe = mako_client.pipeline(transaction=True)
    pipe.execute_command("WAIT", 0, 0)
    assert pipe.execute() == [0]


def test_time_returns_epoch_seconds_and_microseconds(mako_client: redis.Redis) -> None:
    seconds, microseconds = mako_client.execute_command("TIME")

    assert int(seconds) > 0
    assert 0 <= int(microseconds) < 1_000_000


def test_exec_aborts_after_queue_time_error(mako_client: redis.Redis) -> None:
    assert mako_client.execute_command("MULTI") == b"OK"
    with pytest.raises(redis.ResponseError):
        mako_client.execute_command("SET", key("missing-value"))
    with pytest.raises(redis.exceptions.ExecAbortError):
        mako_client.execute_command("EXEC")

    assert mako_client.ping() is True
