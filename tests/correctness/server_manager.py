#!/usr/bin/env python3
"""
Server manager for makoCon (Redis-compatible Mako server).

Handles starting, stopping, and health-checking the makoCon process.
Used by all correctness test scripts.
"""

import os
import sys
import time
import signal
import subprocess
import socket
import redis
import atexit

MAKO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MAKOCON_BIN = os.path.join(MAKO_ROOT, "build", "makoCon")
MAKO_PORT = 6380
MAKO_HOST = "127.0.0.1"
SERVER_STARTUP_TIMEOUT = 30  # seconds

_server_proc = None


def is_server_running():
    """Check if makoCon is accepting connections on the expected port."""
    try:
        r = redis.Redis(host=MAKO_HOST, port=MAKO_PORT, socket_timeout=2)
        return r.ping()
    except Exception:
        return False


def start_server():
    """Start makoCon server. Returns the subprocess.Popen object."""
    global _server_proc

    if is_server_running():
        print(f"[server_manager] makoCon already running on {MAKO_HOST}:{MAKO_PORT}")
        return None

    if not os.path.isfile(MAKOCON_BIN):
        print(f"[server_manager] ERROR: makoCon binary not found at {MAKOCON_BIN}")
        sys.exit(1)

    print(f"[server_manager] Starting makoCon from {MAKOCON_BIN}...")
    log_path = os.path.join(MAKO_ROOT, "tests", "correctness", "makocon_server.log")
    log_file = open(log_path, "w")

    _server_proc = subprocess.Popen(
        [MAKOCON_BIN],
        stdout=log_file,
        stderr=subprocess.STDOUT,
        cwd=MAKO_ROOT,
        preexec_fn=os.setsid,  # New process group for clean shutdown
    )

    # Register cleanup
    atexit.register(stop_server)

    # Wait for server to be ready
    start_time = time.time()
    while time.time() - start_time < SERVER_STARTUP_TIMEOUT:
        if _server_proc.poll() is not None:
            print(f"[server_manager] ERROR: makoCon exited with code {_server_proc.returncode}")
            print(f"[server_manager] Check log: {log_path}")
            sys.exit(1)
        if is_server_running():
            print(f"[server_manager] makoCon ready on {MAKO_HOST}:{MAKO_PORT} "
                  f"(took {time.time() - start_time:.1f}s)")
            return _server_proc
        time.sleep(0.5)

    print(f"[server_manager] ERROR: makoCon did not start within {SERVER_STARTUP_TIMEOUT}s")
    stop_server()
    sys.exit(1)


def stop_server():
    """Stop the makoCon server process."""
    global _server_proc
    if _server_proc is not None and _server_proc.poll() is None:
        print("[server_manager] Stopping makoCon...")
        try:
            os.killpg(os.getpgid(_server_proc.pid), signal.SIGTERM)
            _server_proc.wait(timeout=5)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            try:
                os.killpg(os.getpgid(_server_proc.pid), signal.SIGKILL)
                _server_proc.wait(timeout=3)
            except Exception:
                pass
        _server_proc = None
        print("[server_manager] makoCon stopped.")
    else:
        # Fallback: kill any makoCon process started by a different Python process
        # (e.g., when this module is imported fresh and _server_proc is None).
        # This ensures test scripts that restart the server always get a clean state.
        try:
            result = subprocess.run(
                ["pkill", "-TERM", "-f", "build/makoCon"],
                capture_output=True,
            )
            if result.returncode == 0:
                print("[server_manager] Stopped external makoCon process via pkill.")
                time.sleep(1)  # give it time to exit before caller restarts
        except Exception:
            pass


def get_server_pid():
    """Return PID of the running makoCon server (if we started it)."""
    global _server_proc
    if _server_proc and _server_proc.poll() is None:
        return _server_proc.pid
    return None


def get_client(**kwargs):
    """Return a redis.Redis client connected to makoCon."""
    defaults = {
        "host": MAKO_HOST,
        "port": MAKO_PORT,
        "socket_timeout": 10,
        "decode_responses": False,  # Binary mode for proper value comparison
    }
    defaults.update(kwargs)
    return redis.Redis(**defaults)


def ensure_server():
    """Ensure the server is running, starting it if necessary."""
    if not is_server_running():
        start_server()


def extract_user_value(raw_value, expected_len=None):
    """
    Extract the user-provided value from a Mako-encoded value.

    Mako's Encode() appends metadata bytes (EXTRA_BITS_FOR_VALUE = ~20 bytes)
    to the end of each value. The user data is at the beginning.

    If expected_len is given, returns exactly that many bytes.
    Otherwise returns the value with trailing null bytes stripped.
    """
    if raw_value is None:
        return None
    if expected_len is not None:
        return raw_value[:expected_len]
    # Strip trailing null bytes (metadata is all zeros initially)
    return raw_value.rstrip(b'\x00')


# Metadata overhead added by mako::Encode()
# sizeof(uint32_t) + sizeof(Node) where Node = {uint32_t, int16_t, char*}
# On x86_64: 4 + 16 = 20 bytes (with struct padding)
ENCODE_OVERHEAD = 20
