from __future__ import annotations

import os
import uuid
from collections.abc import Callable

import pytest
import redis


def case_key(name: str) -> str:
    return f"mako:kvrocks:set:{uuid.uuid4()}:{name}"


def reference_client() -> redis.Redis | None:
    if os.environ.get("KVROCKS_CASES_COMPARE_REDIS") != "1":
        return None
    host = os.environ.get("REDIS_REFERENCE_HOST", "127.0.0.1")
    port = int(os.environ.get("REDIS_REFERENCE_PORT", "6379"))
    client = redis.Redis(host=host, port=port, decode_responses=False)
    try:
        client.ping()
    except redis.RedisError:
        pytest.skip("reference Redis is not reachable")
    return client


def run_against_reference(scenario: Callable[[redis.Redis], object]) -> object | None:
    client = reference_client()
    if client is None:
        return None
    try:
        return scenario(client)
    finally:
        client.close()


def assert_same_as_reference(
    mako_client: redis.Redis,
    scenario: Callable[[redis.Redis], object],
) -> None:
    mako_result = scenario(mako_client)
    redis_result = run_against_reference(scenario)
    if redis_result is not None:
        assert mako_result == redis_result


def test_sadd_scard_sismember_smembers_basics(mako_client: redis.Redis) -> None:
    def scenario(client: redis.Redis) -> object:
        key = case_key("basic")
        client.delete(key)
        out = {
            "sadd_foo": client.sadd(key, b"foo"),
            "sadd_bar": client.sadd(key, b"bar"),
            "sadd_bar_again": client.sadd(key, b"bar"),
            "scard": client.scard(key),
            "foo": client.sismember(key, b"foo"),
            "bar": client.sismember(key, b"bar"),
            "bla": client.sismember(key, b"bla"),
            "members": client.smembers(key),
        }
        client.delete(key)
        return out

    result = scenario(mako_client)
    assert result == {
        "sadd_foo": 1,
        "sadd_bar": 1,
        "sadd_bar_again": 0,
        "scard": 2,
        "foo": 1,
        "bar": 1,
        "bla": 0,
        "members": {b"bar", b"foo"},
    }
    assert_same_as_reference(mako_client, scenario)


def test_variadic_sadd_and_srem_cases(mako_client: redis.Redis) -> None:
    def scenario(client: redis.Redis) -> object:
        key = case_key("variadic")
        client.delete(key)
        out = {
            "sadd_first": client.sadd(key, b"a", b"b", b"c"),
            "sadd_second": client.sadd(key, b"A", b"a", b"b", b"c", b"B"),
            "members_after_add": client.smembers(key),
            "srem_missing": client.srem(key, b"k", b"k", b"k"),
            "srem_some": client.srem(key, b"b", b"B", b"x", b"y"),
            "members_after_rem": client.smembers(key),
        }
        client.delete(key)
        return out

    result = scenario(mako_client)
    assert result == {
        "sadd_first": 3,
        "sadd_second": 2,
        "members_after_add": {b"A", b"B", b"a", b"b", b"c"},
        "srem_missing": 0,
        "srem_some": 2,
        "members_after_rem": {b"A", b"a", b"c"},
    }
    assert_same_as_reference(mako_client, scenario)


def test_set_algebra_two_and_three_sets(mako_client: redis.Redis) -> None:
    def scenario(client: redis.Redis) -> object:
        set1 = case_key("set1")
        set2 = case_key("set2")
        set3 = case_key("set3")
        client.delete(set1, set2, set3)
        client.sadd(set1, *[str(i).encode() for i in range(0, 6)])
        client.sadd(set2, *[str(i).encode() for i in range(3, 9)])
        client.sadd(set3, *[str(i).encode() for i in range(5, 11)])
        out = {
            "sinter2": client.sinter(set1, set2),
            "sunion2": client.sunion(set1, set2),
            "sdiff2": client.sdiff(set1, set2),
            "sinter3": client.sinter(set1, set2, set3),
            "sdiff3": client.sdiff(set1, set2, set3),
        }
        client.delete(set1, set2, set3)
        return out

    result = scenario(mako_client)
    assert result == {
        "sinter2": {b"3", b"4", b"5"},
        "sunion2": {b"0", b"1", b"2", b"3", b"4", b"5", b"6", b"7", b"8"},
        "sdiff2": {b"0", b"1", b"2"},
        "sinter3": {b"5"},
        "sdiff3": {b"0", b"1", b"2"},
    }
    assert_same_as_reference(mako_client, scenario)


def test_store_variants_overwrite_destination(mako_client: redis.Redis) -> None:
    def scenario(client: redis.Redis) -> object:
        left = case_key("left")
        right = case_key("right")
        dest = case_key("dest")
        client.delete(left, right, dest)
        client.sadd(left, b"a", b"b", b"c")
        client.sadd(right, b"b", b"c", b"d")
        client.sadd(dest, b"old")
        out = {
            "sinterstore": client.sinterstore(dest, left, right),
            "after_inter": client.smembers(dest),
            "sunionstore": client.sunionstore(dest, left, right),
            "after_union": client.smembers(dest),
            "sdiffstore": client.sdiffstore(dest, left, right),
            "after_diff": client.smembers(dest),
        }
        client.delete(left, right, dest)
        return out

    result = scenario(mako_client)
    assert result == {
        "sinterstore": 2,
        "after_inter": {b"b", b"c"},
        "sunionstore": 4,
        "after_union": {b"a", b"b", b"c", b"d"},
        "sdiffstore": 1,
        "after_diff": {b"a"},
    }
    assert_same_as_reference(mako_client, scenario)


def test_non_existing_keys_are_empty_sets(mako_client: redis.Redis) -> None:
    def scenario(client: redis.Redis) -> object:
        left = case_key("left")
        missing = case_key("missing")
        dest = case_key("dest")
        client.delete(left, missing, dest)
        client.sadd(left, b"a", b"b")
        client.sadd(dest, b"old")
        out = {
            "sinter": client.sinter(left, missing),
            "sunion": client.sunion(left, missing),
            "sdiff": client.sdiff(left, missing),
            "sinterstore": client.sinterstore(dest, left, missing),
            "dest_after_empty_store": client.smembers(dest),
        }
        client.delete(left, missing, dest)
        return out

    result = scenario(mako_client)
    assert result == {
        "sinter": set(),
        "sunion": {b"a", b"b"},
        "sdiff": {b"a", b"b"},
        "sinterstore": 0,
        "dest_after_empty_store": set(),
    }
    assert_same_as_reference(mako_client, scenario)


def test_smove_cases(mako_client: redis.Redis) -> None:
    def scenario(client: redis.Redis) -> object:
        source = case_key("source")
        destination = case_key("destination")
        client.delete(source, destination)
        client.sadd(source, b"one", b"two")
        out = {
            "move_existing": client.smove(source, destination, b"one"),
            "move_missing": client.smove(source, destination, b"missing"),
            "source": client.smembers(source),
            "destination": client.smembers(destination),
        }
        client.delete(source, destination)
        return out

    result = scenario(mako_client)
    assert result == {
        "move_existing": 1,
        "move_missing": 0,
        "source": {b"two"},
        "destination": {b"one"},
    }
    assert_same_as_reference(mako_client, scenario)


def test_spop_and_srandmember_count_contracts(mako_client: redis.Redis) -> None:
    key = case_key("random")
    mako_client.delete(key)
    mako_client.sadd(key, b"a", b"b", b"c")

    one = mako_client.srandmember(key)
    assert one in {b"a", b"b", b"c"}

    unique = mako_client.srandmember(key, 2)
    assert len(unique) == 2
    assert set(unique).issubset({b"a", b"b", b"c"})

    with_duplicates = mako_client.srandmember(key, -5)
    assert len(with_duplicates) == 5
    assert set(with_duplicates).issubset({b"a", b"b", b"c"})

    popped = mako_client.spop(key, 2)
    assert len(popped) == 2
    assert set(popped).issubset({b"a", b"b", b"c"})
    assert mako_client.scard(key) == 1
    mako_client.delete(key)

