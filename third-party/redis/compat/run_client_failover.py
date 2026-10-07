#!/usr/bin/env python3
from __future__ import annotations

import os
import time

import redis

from harness_common import RedisTarget, main_guard, na


def parse_targets(raw: str) -> list[RedisTarget]:
    targets = []
    for item in raw.split(","):
        host, _, port = item.partition(":")
        if not host or not port:
            na("MAKO_FAILOVER_TARGETS must be host:port[,host:port...]")
        targets.append(RedisTarget(host=host, port=int(port)))
    return targets


def main() -> None:
    raw = os.environ.get("MAKO_FAILOVER_TARGETS")
    if not raw:
        na("missing MAKO_FAILOVER_TARGETS")
    targets = parse_targets(raw)
    deadline = time.time() + float(os.environ.get("MAKO_FAILOVER_TIMEOUT_S", "5"))
    last_error: BaseException | None = None
    attempts = 0

    while time.time() < deadline:
        for target in targets:
            attempts += 1
            try:
                client = redis.Redis(host=target.host, port=target.port, decode_responses=False)
                client.ping()
                client.set("client-failover:probe", b"ok")
                assert client.get("client-failover:probe") == b"ok"
                client.delete("client-failover:probe")
                client.close()
                print(f"client failover probe succeeded attempts={attempts} target={target.host}:{target.port}")
                return
            except BaseException as exc:  # noqa: BLE001
                last_error = exc
                time.sleep(0.1)

    print(f"client failover probe failed attempts={attempts} last_error={last_error}")
    raise SystemExit(1)


if __name__ == "__main__":
    main_guard(main)
