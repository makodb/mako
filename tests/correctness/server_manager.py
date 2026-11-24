#!/usr/bin/env python3
"""Own one native Rust Redis process for each correctness-test invocation."""
import atexit
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

import redis

MAKO_ROOT = str(Path(__file__).resolve().parents[2])
MAKO_PORT = int(os.environ.get("MAKO_REDIS_PORT", "6380"))
MAKO_HOST = "127.0.0.1"
SERVER_STARTUP_TIMEOUT = 30
MAKOCON_BIN = str(Path(MAKO_ROOT) / os.environ.get("BUILD_DIR", "build") / "mako_redis")
_server_proc = None
_server_dir = None


def is_server_running():
    try:
        return get_client(socket_timeout=2).ping()
    except redis.RedisError:
        return False


def start_server():
    global _server_proc, _server_dir
    if _server_proc is not None and _server_proc.poll() is None:
        return _server_proc
    if is_server_running():
        raise RuntimeError(f"Redis test port {MAKO_PORT} belongs to another process")
    binary = Path(MAKOCON_BIN)
    if not binary.is_file():
        raise RuntimeError(f"Build the native Redis executable first: {binary}")
    if _server_dir is not None:
        _server_dir.cleanup()
    runs = binary.parent / "runs"
    runs.mkdir(exist_ok=True)
    _server_dir = tempfile.TemporaryDirectory(prefix="redis-correctness-", dir=runs)
    directory = Path(_server_dir.name)
    config = dict(node=dict(owner=0, nodes=1, listen="127.0.0.1:0", workers=4,
        warehouses_per_shard=1, tables=[dict(id=1, name="records", kind="governed",
        warehouse=True, initial_owner=0)]), redis_listen=f"{MAKO_HOST}:{MAKO_PORT}")
    path = directory / "config.json"
    path.write_text(json.dumps(config))
    log_path = directory / "server.log"
    with log_path.open("w") as log:
        _server_proc = subprocess.Popen([str(binary), "--config", str(path)],
            stdout=log, stderr=subprocess.STDOUT, cwd=MAKO_ROOT, start_new_session=True)
    deadline = time.monotonic() + SERVER_STARTUP_TIMEOUT
    while time.monotonic() < deadline:
        if _server_proc.poll() is not None:
            raise RuntimeError(f"Native Redis exited {_server_proc.returncode}: {log_path.read_text()}")
        if is_server_running():
            return _server_proc
        time.sleep(0.05)
    stop_server()
    raise RuntimeError(f"Native Redis startup timed out: {log_path.read_text()}")


def stop_server():
    global _server_proc
    if _server_proc is None:
        return
    child, _server_proc = _server_proc, None
    if child.poll() is None:
        try:
            os.killpg(child.pid, signal.SIGTERM)
            child.wait(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait(timeout=5)
        except ProcessLookupError:
            child.wait(timeout=5)
    else:
        child.wait()


def get_server_pid():
    return _server_proc.pid if _server_proc and _server_proc.poll() is None else None


def get_client(**kwargs):
    defaults = dict(host=MAKO_HOST, port=MAKO_PORT, socket_timeout=10, decode_responses=False)
    defaults.update(kwargs)
    return redis.Redis(**defaults)


def ensure_server():
    if _server_proc is None or _server_proc.poll() is not None:
        start_server()


def extract_user_value(raw_value, expected_len=None):
    # The native API returns exactly the stored user bytes, including NULs.
    if raw_value is not None and expected_len is not None and len(raw_value) != expected_len:
        raise AssertionError(f"stored value length {len(raw_value)} != {expected_len}")
    return raw_value


ENCODE_OVERHEAD = 0
atexit.register(stop_server)
