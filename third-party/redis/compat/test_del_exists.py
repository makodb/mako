from __future__ import annotations

import uuid

import pytest
import redis


def key(name: str) -> str:
    return f"mako:test:del-exists:{uuid.uuid4()}:{name}"


@pytest.mark.parametrize("delete_command", ["DEL", "UNLINK"])
def test_delete_existing_and_missing_keys(mako_client: redis.Redis, delete_command: str) -> None:
    present = key("present")
    missing = key("missing")

    assert mako_client.set(present, b"value") is True
    assert mako_client.exists(present) == 1

    assert mako_client.execute_command(delete_command, present) == 1
    assert mako_client.exists(present) == 0
    assert mako_client.execute_command(delete_command, missing) == 0


def test_exists_variadic_count(mako_client: redis.Redis) -> None:
    first = key("first")
    second = key("second")
    missing = key("missing")

    assert mako_client.set(first, b"1") is True
    assert mako_client.set(second, b"2") is True

    assert mako_client.exists(first, second, missing) == 2


def test_exists_counts_duplicate_arguments(mako_client: redis.Redis) -> None:
    present = key("duplicate")

    assert mako_client.set(present, b"1") is True
    assert mako_client.exists(present, present, present) == 3


def test_delete_inside_transaction(mako_client: redis.Redis) -> None:
    present = key("tx-present")

    assert mako_client.set(present, b"value") is True
    pipe = mako_client.pipeline(transaction=True)
    pipe.exists(present)
    pipe.delete(present)
    pipe.exists(present)

    assert pipe.execute() == [1, 1, 0]
