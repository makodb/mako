from __future__ import annotations

import os
import sys
from dataclasses import dataclass

import redis


EXIT_NA = 78


@dataclass(frozen=True)
class RedisTarget:
    host: str
    port: int


def mako_target() -> RedisTarget:
    host = os.environ.get("MAKO_REDIS_HOST") or os.environ.get("MAKO_HOST", "127.0.0.1")
    port = int(os.environ.get("MAKO_REDIS_PORT") or os.environ.get("MAKO_PORT", "6380"))
    return RedisTarget(host=host, port=port)


def connect(target: RedisTarget | None = None) -> redis.Redis:
    target = target or mako_target()
    client = redis.Redis(host=target.host, port=target.port, decode_responses=False)
    client.ping()
    return client


def na(message: str) -> None:
    print(message)
    raise SystemExit(EXIT_NA)


def fail(message: str) -> None:
    print(message)
    raise SystemExit(1)


def env_int(name: str, default: int) -> int:
    raw = os.environ.get(name)
    if raw is None:
        return default
    try:
        return int(raw)
    except ValueError:
        fail(f"{name} must be an integer")


def require_env(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        na(f"missing {name}")
    return value


def main_guard(fn) -> None:
    try:
        fn()
    except redis.RedisError as exc:
        print(f"redis error: {exc}")
        sys.exit(1)
