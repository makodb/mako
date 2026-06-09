#!/usr/bin/env bash
set -u

HOST="${MAKO_HOST:-127.0.0.1}"
PORT="${MAKO_PORT:-6380}"
CASES="${FUZZ_CASES:-80}"
POST_FUZZ_WAIT_S="${POST_FUZZ_WAIT_S:-15.0}"
POST_FUZZ_FINAL_WAIT_S="${POST_FUZZ_FINAL_WAIT_S:-30.0}"

python3 - "$HOST" "$PORT" "$CASES" "$POST_FUZZ_WAIT_S" "$POST_FUZZ_FINAL_WAIT_S" <<'PY'
import random
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
cases = int(sys.argv[3])
post_fuzz_wait_s = float(sys.argv[4])
post_fuzz_final_wait_s = float(sys.argv[5])
rng = random.Random(12345)

frames = [
    b"*1\r\n$4\r\nPING\r\n",
    b"*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n",
    b"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",
    b"*0\r\n",
    b"*2\r\n$7\r\nUNKNOWN\r\n$1\r\nx\r\n",
]

for _ in range(cases):
    if rng.random() < 0.35:
        frame = rng.choice(frames)
    else:
        frame = rng.randbytes(rng.randint(1, 96))
    try:
        sock = socket.create_connection((host, port), timeout=0.1)
        sock.settimeout(0.1)
        sock.sendall(frame)
        try:
            sock.recv(4096)
        except socket.timeout:
            pass
        sock.close()
    except OSError:
        pass

time.sleep(post_fuzz_wait_s)
sock = socket.create_connection((host, port), timeout=1.0)
sock.settimeout(1.0)
sock.sendall(b"*1\r\n$4\r\nPING\r\n")
reply = sock.recv(64)
sock.close()
if b"PONG" not in reply:
    print(f"post-fuzz PING failed reply={reply!r}")
    raise SystemExit(1)
time.sleep(post_fuzz_final_wait_s)
sock = socket.create_connection((host, port), timeout=1.0)
sock.settimeout(1.0)
sock.sendall(b"*1\r\n$4\r\nPING\r\n")
reply = sock.recv(64)
sock.close()
if b"PONG" not in reply:
    print(f"post-fuzz delayed PING failed reply={reply!r}")
    raise SystemExit(1)
print(f"resp fuzz completed cases={cases}")
PY
