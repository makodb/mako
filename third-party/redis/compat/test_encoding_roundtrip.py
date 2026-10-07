from __future__ import annotations

import random
import uuid

import redis


def key(index: int) -> str:
    return f"mako:test:encoding:{uuid.uuid4()}:{index}"


def payloads() -> list[bytes]:
    rng = random.Random(0)
    values = [
        b"",
        b"42",
        b"\x00",
        b"\xff",
        b"prefix\x00middle\xffsuffix",
    ]
    values.extend(bytes(rng.randrange(0, 256) for _ in range(size)) for size in range(1, 101))
    return values


def test_set_get_roundtrips_bytes(mako_client: redis.Redis) -> None:
    for index, payload in enumerate(payloads()):
        name = key(index)
        assert mako_client.set(name, payload) is True
        assert mako_client.get(name) == payload
