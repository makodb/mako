from __future__ import annotations

import os
import socket
import time
import uuid

import redis


def channel(name: str) -> str:
    return f"mako:test:pubsub:{uuid.uuid4()}:{name}"


def wait_pubsub(pubsub: redis.client.PubSub, expected_type: str, timeout_s: float = 2.0):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        message = pubsub.get_message(ignore_subscribe_messages=False, timeout=0.1)
        if message and message["type"] == expected_type:
            return message
    raise AssertionError(f"did not receive {expected_type}")


def send_resp(sock: socket.socket, *parts: bytes) -> None:
    payload = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        payload.append(f"${len(part)}\r\n".encode())
        payload.append(part)
        payload.append(b"\r\n")
    sock.sendall(b"".join(payload))


def recv_until(sock: socket.socket, needle: bytes, timeout_s: float = 2.0) -> bytes:
    deadline = time.monotonic() + timeout_s
    data = b""
    sock.settimeout(0.1)
    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(4096)
        except TimeoutError:
            continue
        if not chunk:
            break
        data += chunk
        if needle in data:
            return data
    raise AssertionError(f"did not receive {needle!r}; got {data!r}")


def test_subscribe_publish_and_unsubscribe(mako_client: redis.Redis) -> None:
    name = channel("direct")
    pubsub = mako_client.pubsub()
    try:
        pubsub.subscribe(name)
        subscribed = wait_pubsub(pubsub, "subscribe")
        assert subscribed["channel"] == name.encode()
        assert subscribed["data"] == 1

        assert mako_client.publish(name, b"hello") == 1
        message = wait_pubsub(pubsub, "message")
        assert message["channel"] == name.encode()
        assert message["data"] == b"hello"

        pubsub.unsubscribe(name)
        unsubscribed = wait_pubsub(pubsub, "unsubscribe")
        assert unsubscribed["channel"] == name.encode()
        assert unsubscribed["data"] == 0
    finally:
        pubsub.close()


def test_psubscribe_receives_matching_publish(mako_client: redis.Redis) -> None:
    prefix = channel("pattern")
    pattern = f"{prefix}:*"
    name = f"{prefix}:one"
    pubsub = mako_client.pubsub()
    try:
        pubsub.psubscribe(pattern)
        subscribed = wait_pubsub(pubsub, "psubscribe")
        assert subscribed["pattern"] is None
        assert subscribed["channel"] == pattern.encode()
        assert subscribed["data"] == 1

        assert mako_client.publish(name, b"pattern-hit") == 1
        message = wait_pubsub(pubsub, "pmessage")
        assert message["pattern"] == pattern.encode()
        assert message["channel"] == name.encode()
        assert message["data"] == b"pattern-hit"
    finally:
        pubsub.close()


def test_pubsub_introspection_and_info_metrics(mako_client: redis.Redis) -> None:
    name = channel("introspection")
    pubsub = mako_client.pubsub()
    try:
        pubsub.subscribe(name)
        wait_pubsub(pubsub, "subscribe")

        channels = mako_client.execute_command("PUBSUB", "CHANNELS", name)
        assert channels == [name.encode()]

        numsub = mako_client.execute_command("PUBSUB", "NUMSUB", name, "missing")
        assert numsub == [name.encode(), 1, b"missing", 0]

        assert mako_client.execute_command("PUBSUB", "NUMPAT") == 0
        info = mako_client.info("clients")
        assert info["pubsub_channels"] >= 1
        assert info["pubsub_patterns"] >= 0
    finally:
        pubsub.close()


def test_publish_inside_multi_delivers_on_exec(mako_client: redis.Redis) -> None:
    name = channel("multi")
    pubsub = mako_client.pubsub()
    try:
        pubsub.subscribe(name)
        wait_pubsub(pubsub, "subscribe")

        pipe = mako_client.pipeline(transaction=True)
        queued = pipe.publish(name, b"queued-message")
        assert queued is pipe
        assert pubsub.get_message(ignore_subscribe_messages=True, timeout=0.1) is None

        assert pipe.execute() == [1]
        message = wait_pubsub(pubsub, "message")
        assert message["channel"] == name.encode()
        assert message["data"] == b"queued-message"
    finally:
        pubsub.close()


def test_subscriber_mode_rejects_storage_commands() -> None:
    host = os.environ.get("MAKO_REDIS_HOST", "127.0.0.1")
    port = int(os.environ.get("MAKO_REDIS_PORT", "6380"))
    name = channel("raw").encode()

    with socket.create_connection((host, port), timeout=2.0) as sock:
        send_resp(sock, b"SUBSCRIBE", name)
        recv_until(sock, b"subscribe")

        send_resp(sock, b"GET", b"k")
        data = recv_until(sock, b"allowed in subscriber mode")
        assert data.startswith(b"-ERR")

        send_resp(sock, b"UNSUBSCRIBE", name)
        recv_until(sock, b"unsubscribe")

        send_resp(sock, b"PING")
        assert recv_until(sock, b"PONG").startswith(b"+PONG")
