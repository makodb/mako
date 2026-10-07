from __future__ import annotations

import uuid

import pytest
import redis


def key(name: str) -> str:
    return f"mako:test:set-options:{uuid.uuid4()}:{name}"


@pytest.mark.parametrize("expiry_args", [(), ("EX", 60)])
@pytest.mark.parametrize(
    ("condition", "initial", "expected_reply", "expected_value"),
    [
        ("NX", None, True, b"new"),
        ("NX", b"old", None, b"old"),
        ("XX", None, None, None),
        ("XX", b"old", True, b"new"),
    ],
)
def test_set_nx_xx_matrix(
    mako_client: redis.Redis,
    condition: str,
    initial: bytes | None,
    expected_reply: bool | None,
    expected_value: bytes | None,
    expiry_args: tuple[str, int],
) -> None:
    name = key(f"{condition.lower()}-{bool(expiry_args)}")
    if initial is not None:
        assert mako_client.set(name, initial) is True

    reply = mako_client.execute_command("SET", name, b"new", condition, *expiry_args)

    assert reply is expected_reply
    assert mako_client.get(name) == expected_value


def test_set_get_returns_old_value_and_updates(mako_client: redis.Redis) -> None:
    name = key("get-present")

    assert mako_client.set(name, b"old") is True
    assert mako_client.set(name, b"new", get=True) == b"old"
    assert mako_client.get(name) == b"new"


def test_set_get_missing_returns_nil_and_sets(mako_client: redis.Redis) -> None:
    name = key("get-missing")

    assert mako_client.execute_command("SET", name, b"new", "GET") is None
    assert mako_client.get(name) == b"new"


def test_set_growing_value_retries_resize_abort(mako_client: redis.Redis) -> None:
    name = key("growing-value")

    retries_before = int(mako_client.info("mako")["mako_txn_retries"])

    assert mako_client.set(name, b"old") is True
    larger = b"longer" * 1024
    assert mako_client.set(name, larger) is True
    assert mako_client.get(name) == larger
    retries_after = int(mako_client.info("mako")["mako_txn_retries"])
    assert retries_after > retries_before
