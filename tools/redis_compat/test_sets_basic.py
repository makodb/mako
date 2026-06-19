from __future__ import annotations

import time
import uuid

import pytest
import redis


def key(name: str) -> str:
    return f"mako:test:set:{uuid.uuid4()}:{name}"


def test_basic_set_commands(mako_client: redis.Redis) -> None:
    name = key("basic")

    assert mako_client.sadd(name, b"a", b"b", b"a") == 2
    assert mako_client.scard(name) == 2
    assert mako_client.sismember(name, b"a") == 1
    assert mako_client.sismember(name, b"z") == 0
    assert mako_client.smembers(name) == {b"a", b"b"}
    assert mako_client.srem(name, b"a", b"missing") == 1
    assert mako_client.smembers(name) == {b"b"}


def test_smismember_returns_membership_vector(mako_client: redis.Redis) -> None:
    name = key("smismember")

    assert mako_client.sadd(name, b"a", b"b") == 2
    assert mako_client.execute_command("SMISMEMBER", name, b"a", b"z", b"b") == [1, 0, 1]


def test_spop_and_srandmember_count_modes(mako_client: redis.Redis) -> None:
    name = key("random")

    assert mako_client.sadd(name, b"a", b"b", b"c") == 3
    sample = mako_client.srandmember(name)
    assert sample in {b"a", b"b", b"c"}
    assert len(mako_client.srandmember(name, 2)) == 2
    assert len(mako_client.srandmember(name, -5)) == 5

    popped = mako_client.spop(name, 2)
    assert len(popped) == 2
    assert mako_client.scard(name) == 1


def test_smove_moves_member_between_sets(mako_client: redis.Redis) -> None:
    source = key("source")
    destination = key("destination")

    assert mako_client.sadd(source, b"a", b"b") == 2
    assert mako_client.smove(source, destination, b"a") is True
    assert mako_client.smove(source, destination, b"missing") is False
    assert mako_client.smembers(source) == {b"b"}
    assert mako_client.smembers(destination) == {b"a"}


def test_set_algebra_and_store_variants(mako_client: redis.Redis) -> None:
    left = key("left")
    right = key("right")
    dest = key("dest")

    assert mako_client.sadd(left, b"a", b"b", b"c") == 3
    assert mako_client.sadd(right, b"b", b"c", b"d") == 3

    assert mako_client.sinter(left, right) == {b"b", b"c"}
    assert mako_client.execute_command("SINTERCARD", 2, left, right) == 2
    assert mako_client.execute_command("SINTERCARD", 2, left, right, "LIMIT", 1) == 1
    assert mako_client.sunion(left, right) == {b"a", b"b", b"c", b"d"}
    assert mako_client.sdiff(left, right) == {b"a"}

    assert mako_client.sinterstore(dest, left, right) == 2
    assert mako_client.smembers(dest) == {b"b", b"c"}
    assert mako_client.sunionstore(dest, left, right) == 4
    assert mako_client.smembers(dest) == {b"a", b"b", b"c", b"d"}
    assert mako_client.sdiffstore(dest, left, right) == 1
    assert mako_client.smembers(dest) == {b"a"}


def test_set_type_scan_visibility_and_ttl(mako_client: redis.Redis) -> None:
    name = key("typed")

    assert mako_client.sadd(name, b"a") == 1
    assert mako_client.type(name) == b"set"
    assert all(not key.startswith(b"\x01") for key in mako_client.keys("*"))

    assert mako_client.expire(name, 1) is True
    time.sleep(1.2)
    assert mako_client.type(name) == b"none"
    assert mako_client.smembers(name) == set()


def test_set_composite_key_encoding_is_binary_safe(mako_client: redis.Redis) -> None:
    p = key("collision")
    left = f"{p}:a:b"
    right = f"{p}:a"

    assert mako_client.sadd(left, b"c") == 1
    assert mako_client.sadd(right, b"b:c") == 1

    assert mako_client.smembers(left) == {b"c"}
    assert mako_client.smembers(right) == {b"b:c"}
    assert mako_client.scard(left) == 1
    assert mako_client.scard(right) == 1


def test_set_ttl_is_cleared_when_last_member_removed(mako_client: redis.Redis) -> None:
    name = key("ttl-clear")

    assert mako_client.sadd(name, b"a") == 1
    assert mako_client.expire(name, 100) is True
    assert mako_client.srem(name, b"a") == 1
    assert mako_client.exists(name) == 0
    assert mako_client.ttl(name) == -2

    assert mako_client.sadd(name, b"b") == 1
    assert mako_client.ttl(name) == -1


def test_set_replaced_by_string_and_delete_removes_logical_key(mako_client: redis.Redis) -> None:
    name = key("replace")

    assert mako_client.sadd(name, b"member") == 1
    assert mako_client.set(name, b"value") is True
    assert mako_client.type(name) == b"string"
    assert mako_client.get(name) == b"value"
    with pytest.raises(redis.ResponseError):
        mako_client.smembers(name)

    assert mako_client.delete(name) == 1
    assert mako_client.type(name) == b"none"
    assert mako_client.smembers(name) == set()


def test_sadd_does_not_create_parallel_set_for_string_key(mako_client: redis.Redis) -> None:
    name = key("wrong-type")

    assert mako_client.set(name, b"value") is True
    with pytest.raises(redis.ResponseError):
        mako_client.sadd(name, b"member")
    assert mako_client.type(name) == b"string"
    assert mako_client.get(name) == b"value"


def test_keys_star_includes_leading_null_user_key(mako_client: redis.Redis) -> None:
    name = b"\x00" + key("leading-null").encode()

    assert mako_client.set(name, b"value") is True
    assert mako_client.get(name) == b"value"
    assert name in mako_client.keys("*")
