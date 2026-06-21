from __future__ import annotations

import time
import uuid

import redis


def key(name: str) -> str:
    return f"mako:test:ttl-config-client:{uuid.uuid4()}:{name}"


def wait_until_missing(client: redis.Redis, name: str, timeout_s: float = 2.0) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if client.get(name) is None:
            return
        time.sleep(0.05)
    raise AssertionError(f"{name} did not expire")


def test_set_px_expiry_is_enforced_by_reads(mako_client: redis.Redis) -> None:
    name = key("px-expiry")

    assert mako_client.execute_command("SET", name, b"value", "PX", 100) is True
    wait_until_missing(mako_client, name)

    assert mako_client.get(name) is None
    assert mako_client.exists(name) == 0


def test_set_without_ttl_clears_previous_expiry(mako_client: redis.Redis) -> None:
    name = key("ttl-cleared")

    assert mako_client.execute_command("SET", name, b"old", "PX", 100) is True
    assert mako_client.set(name, b"persist") is True
    time.sleep(0.2)

    assert mako_client.get(name) == b"persist"


def test_set_keepttl_preserves_previous_expiry(mako_client: redis.Redis) -> None:
    name = key("keepttl")

    assert mako_client.execute_command("SET", name, b"old", "PX", 150) is True
    assert mako_client.execute_command("SET", name, b"new", "KEEPTTL") is True

    wait_until_missing(mako_client, name)
    assert mako_client.get(name) is None


def test_client_id_is_stable_for_connection(mako_client: redis.Redis) -> None:
    first = mako_client.execute_command("CLIENT", "ID")
    second = mako_client.execute_command("CLIENT", "ID")

    assert isinstance(first, int)
    assert first > 0
    assert second == first


def test_config_get_and_resetstat_are_client_compatible(mako_client: redis.Redis) -> None:
    save = mako_client.execute_command("CONFIG", "GET", "save")
    all_known = mako_client.execute_command("CONFIG", "GET", "*")

    assert save == [b"save", b""]
    assert b"appendonly" in all_known
    assert mako_client.execute_command("CONFIG", "RESETSTAT") == b"OK"
