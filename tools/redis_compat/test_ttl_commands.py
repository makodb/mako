from __future__ import annotations

import time
import uuid

import pytest
import redis


def key(name: str) -> str:
    return f"mako:test:ttl-commands:{uuid.uuid4()}:{name}"


def wait_until_missing(client: redis.Redis, name: str, timeout_s: float = 2.0) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if client.get(name) is None:
            return
        time.sleep(0.05)
    raise AssertionError(f"{name} did not expire")


def test_ttl_and_pttl_missing_key_codes(mako_client: redis.Redis) -> None:
    name = key("missing")

    assert mako_client.ttl(name) == -2
    assert mako_client.pttl(name) == -2
    assert mako_client.expire(name, 10) is False
    assert mako_client.pexpire(name, 10) is False
    assert mako_client.persist(name) is False


def test_expire_ttl_pttl_and_persist(mako_client: redis.Redis) -> None:
    name = key("expire-persist")

    assert mako_client.set(name, b"value") is True
    assert mako_client.expire(name, 10) is True

    ttl = mako_client.ttl(name)
    pttl = mako_client.pttl(name)
    assert 1 <= ttl <= 10
    assert pttl > 0

    assert mako_client.persist(name) is True
    assert mako_client.ttl(name) == -1
    assert mako_client.pttl(name) == -1
    assert mako_client.persist(name) is False


def test_setex_and_psetex_alias_set_with_expiry(mako_client: redis.Redis) -> None:
    setex_key = key("setex")
    psetex_key = key("psetex")

    assert mako_client.execute_command("SETEX", setex_key, 1, b"value") is True
    assert mako_client.get(setex_key) == b"value"
    assert 0 <= mako_client.ttl(setex_key) <= 1

    assert mako_client.execute_command("PSETEX", psetex_key, 150, b"value") is True
    assert mako_client.get(psetex_key) == b"value"
    assert 0 < mako_client.pttl(psetex_key) <= 150
    wait_until_missing(mako_client, psetex_key)


def test_pexpire_zero_deletes_key_lazily(mako_client: redis.Redis) -> None:
    name = key("pexpire-zero")

    assert mako_client.set(name, b"value") is True
    assert mako_client.pexpire(name, 0) is True

    assert mako_client.get(name) is None
    assert mako_client.exists(name) == 0
    assert mako_client.ttl(name) == -2


def test_expireat_and_pexpireat(mako_client: redis.Redis) -> None:
    expireat_key = key("expireat")
    pexpireat_key = key("pexpireat")
    now = time.time()

    assert mako_client.set(expireat_key, b"value") is True
    assert mako_client.expireat(expireat_key, int(now) - 1) is True
    assert mako_client.get(expireat_key) is None

    assert mako_client.set(pexpireat_key, b"value") is True
    future_ms = int((time.time() + 0.15) * 1000)
    assert mako_client.pexpireat(pexpireat_key, future_ms) is True
    wait_until_missing(mako_client, pexpireat_key)


def test_expiretime_and_pexpiretime_return_absolute_timestamps(mako_client: redis.Redis) -> None:
    name = key("expiretime")
    now = time.time()

    assert mako_client.set(name, b"value") is True
    assert mako_client.expire(name, 10) is True

    expiretime = mako_client.execute_command("EXPIRETIME", name)
    pexpiretime = mako_client.execute_command("PEXPIRETIME", name)

    assert int(now) + 1 <= expiretime <= int(now) + 11
    assert int(now * 1000) + 1 <= pexpiretime <= int((now + 11) * 1000)


def test_ttl_commands_queue_inside_transaction(mako_client: redis.Redis) -> None:
    name = key("transaction")

    pipe = mako_client.pipeline(transaction=True)
    pipe.set(name, b"value")
    pipe.expire(name, 10)
    pipe.ttl(name)
    result = pipe.execute()

    assert result[0] is True
    assert result[1] is True
    assert 1 <= result[2] <= 10


def test_reserved_internal_prefix_is_rejected(mako_client: redis.Redis) -> None:
    with pytest.raises(redis.ResponseError, match="reserved internal prefix"):
        mako_client.set(b"\x01TTL:user", b"value")

    with pytest.raises(redis.ResponseError, match="reserved internal prefix"):
        mako_client.ttl(b"\x01TTL:user")


def test_ttl_arithmetic_overflow_is_rejected(mako_client: redis.Redis) -> None:
    name = key("overflow")
    huge = str(2**63 - 1)

    with pytest.raises(redis.ResponseError):
        mako_client.execute_command("SET", name, b"value", "EX", huge)

    assert mako_client.set(name, b"value") is True
    with pytest.raises(redis.ResponseError):
        mako_client.execute_command("EXPIRE", name, huge)


def test_expire_nx_and_xx_modifiers(mako_client: redis.Redis) -> None:
    name = key("nx-xx")

    assert mako_client.set(name, b"value") is True
    assert mako_client.execute_command("EXPIRE", name, 10, "NX") is True
    assert mako_client.execute_command("EXPIRE", name, 20, "NX") is False
    assert mako_client.execute_command("EXPIRE", name, 20, "XX") is True
    assert 1 <= mako_client.ttl(name) <= 20

    no_ttl = key("xx-no-ttl")
    assert mako_client.set(no_ttl, b"value") is True
    assert mako_client.execute_command("EXPIRE", no_ttl, 10, "XX") is False
    assert mako_client.ttl(no_ttl) == -1


def test_expire_gt_and_lt_modifiers(mako_client: redis.Redis) -> None:
    name = key("gt-lt")

    assert mako_client.set(name, b"value") is True
    assert mako_client.execute_command("EXPIRE", name, 10, "GT") is False
    assert mako_client.execute_command("EXPIRE", name, 10, "LT") is True
    assert mako_client.execute_command("EXPIRE", name, 5, "GT") is False
    assert mako_client.execute_command("EXPIRE", name, 20, "GT") is True
    assert mako_client.execute_command("EXPIRE", name, 30, "LT") is False
    assert mako_client.execute_command("EXPIRE", name, 5, "LT") is True


def test_expire_modifier_combinations(mako_client: redis.Redis) -> None:
    name = key("modifier-combinations")

    assert mako_client.set(name, b"value") is True
    assert mako_client.execute_command("EXPIRE", name, 10, "NX") is True
    assert mako_client.execute_command("EXPIRE", name, 20, "XX", "GT") is True
    assert mako_client.execute_command("EXPIRE", name, 10, "XX", "LT") is True

    with pytest.raises(redis.ResponseError, match="NX and XX, GT or LT"):
        mako_client.execute_command("EXPIRE", name, 10, "NX", "GT")

    with pytest.raises(redis.ResponseError, match="GT and LT"):
        mako_client.execute_command("EXPIRE", name, 10, "GT", "LT")
