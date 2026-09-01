from __future__ import annotations

import uuid

import redis


def key(name: str) -> str:
    return f"mako:test:multi-strings:{uuid.uuid4()}:{name}"


def test_mget_preserves_missing_slots(mako_client: redis.Redis) -> None:
    first = key("first")
    missing = key("missing")
    third = key("third")

    assert mako_client.set(first, b"1") is True
    assert mako_client.set(third, b"3") is True

    assert mako_client.mget(first, missing, third) == [b"1", None, b"3"]


def test_mset_sets_all_pairs(mako_client: redis.Redis) -> None:
    first = key("mset-first")
    second = key("mset-second")

    assert mako_client.mset({first: b"1", second: b"2"}) is True

    assert mako_client.mget(first, second) == [b"1", b"2"]


def test_msetnx_is_all_or_nothing(mako_client: redis.Redis) -> None:
    existing = key("msetnx-existing")
    new_key = key("msetnx-new")

    assert mako_client.set(existing, b"old") is True

    assert mako_client.execute_command("MSETNX", existing, b"new", new_key, b"new") == 0
    assert mako_client.mget(existing, new_key) == [b"old", None]

    assert mako_client.execute_command("MSETNX", new_key, b"new") == 1
    assert mako_client.get(new_key) == b"new"


def test_getset_setnx_append_strlen(mako_client: redis.Redis) -> None:
    name = key("ops")
    other = key("setnx")

    assert mako_client.getset(name, b"old") is None
    assert mako_client.getset(name, b"new") == b"old"
    assert mako_client.get(name) == b"new"

    assert mako_client.setnx(other, b"first") is True
    assert mako_client.setnx(other, b"second") is False
    assert mako_client.get(other) == b"first"

    assert mako_client.append(name, b"-tail") == len(b"new-tail")
    assert mako_client.strlen(name) == len(b"new-tail")
    assert mako_client.get(name) == b"new-tail"
