from __future__ import annotations

import uuid

import redis


def test_tag_index_workflow(mako_client: redis.Redis) -> None:
    prefix = f"mako:test:tags:{uuid.uuid4()}"
    tags = [f"{prefix}:tag:{idx}" for idx in range(10)]

    for article_id in range(100):
        article = f"article:{article_id}".encode()
        for tag in (tags[article_id % 10], tags[(article_id + 1) % 10]):
            assert mako_client.sadd(tag, article) == 1

    overlap = mako_client.sinter(tags[0], tags[1])
    assert overlap == {f"article:{idx}".encode() for idx in range(0, 100, 10)}

    assert b"article:0" in mako_client.smembers(tags[0])
    assert mako_client.srem(tags[0], b"article:0") == 1
    assert b"article:0" not in mako_client.smembers(tags[0])
