from __future__ import annotations

import os

import pytest
import redis


@pytest.fixture
def mako_client() -> redis.Redis:
    host = os.environ.get("MAKO_REDIS_HOST", "127.0.0.1")
    port = int(os.environ.get("MAKO_REDIS_PORT", "6380"))
    client = redis.Redis(host=host, port=port, decode_responses=False)
    client.ping()
    try:
        yield client
    finally:
        client.close()
