#!/usr/bin/env python3
"""Drive real MBTA/native/srpc nodes; every OK is emitted after engine assertions.

This tests native migration, owner-local point I/O, production paginated scans,
and fixture-only faults at the real native peer callback boundary. Replays use
only previously issued bytes through real srpc; no storage or replies are faked.
The fixture does not start dbtest's ordinary FastTransport data plane.
"""
from pathlib import Path
import os
import random
import re
import selectors
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / os.environ.get("BUILD_DIR", "build_docker")
BINARY = BUILD / "native_sharding_smoke"
ADMIN = BUILD / "mako_admin"
if not (Path("/.dockerenv").exists() or Path("/run/.containerenv").exists()):
    raise SystemExit("Run ./docker_build.sh ci nativeShardingSmoke")
for binary in (BINARY, ADMIN):
    if not os.access(binary, os.X_OK):
        raise SystemExit(f"Missing real Docker-built executable: {binary}")

class Node:
    def __init__(self, config, owner, log):
        self.log = log.open("wb")
        self.process = subprocess.Popen([str(BINARY), str(config), str(owner)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env={**os.environ, "MAKO_CLUSTER_CONFIG": "1"})
        self.buffer = b""
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)

    def marker(self, prefix, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                text = line.decode(errors="replace")
                if "SMOKE FAILED" in text:
                    raise RuntimeError(text)
                if text.startswith(prefix):
                    return text
            if not self.selector.select(max(0, deadline - time.monotonic())):
                break
            data = os.read(self.process.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError(f"Fixture exited {self.process.poll()} before {prefix}")
            self.log.write(data)
            self.log.flush()
            self.buffer += data
        raise TimeoutError(f"Fixture did not report {prefix}")

    def command(self, text):
        self.process.stdin.write((text + "\n").encode())
        self.process.stdin.flush()
        return self.marker("SMOKE OK")

    def close(self, success):
        try:
            if success:
                self.process.stdin.write(b"stop\n")
                self.process.stdin.flush()
                self.marker("SMOKE STOPPED")
                if self.process.wait(timeout=30) != 0:
                    raise RuntimeError("native fixture shutdown failed")
            elif self.process.poll() is None:
                self.process.terminate()
                self.process.wait(timeout=10)
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.selector.close()
            self.log.close()


def ports():
    for _ in range(200):
        base = random.randrange(20000, 29000)
        sockets = []
        try:
            for offset in (0, 100, 20000, 20100):
                sock = socket.socket()
                sockets.append(sock)
                sock.bind(("127.0.0.1", base + offset))
            return base
        except OSError:
            pass
        finally:
            for sock in sockets:
                sock.close()
    raise RuntimeError("No free loopback port set for smoke")


def run(both):
    mode = "single-process" if both else "two-process"
    logs = BUILD / "native-sharding-smoke" / mode
    logs.mkdir(parents=True, exist_ok=True)
    base = ports()
    address = f"127.0.0.1:{base + 20000}"
    with tempfile.TemporaryDirectory(prefix="mako-native-smoke-") as directory:
        config = Path(directory) / "config.yml"
        # Same old-format transport::Configuration schema used by dbtest.
        config.write_text(f"shards: 2\nwarehouses: 1\n"
                          f"memlocalhost: 0\nmemlearner: 0\nmemp1: 0\nmemp2: 0\nlocalhost:\n"
                          f"  - ip: 127.0.0.1\n    port: {base}\n"
                          f"  - ip: 127.0.0.1\n    port: {base + 100}\n")
        nodes = []
        success = False
        try:
            if both:
                nodes.append(Node(config, "both", logs / "both.log"))
                nodes[0].marker("SMOKE READY")
                by_owner = [nodes[0], nodes[0]]
            else:
                nodes.append(Node(config, 0, logs / "owner0.log"))
                nodes[0].marker("SMOKE READY")
                nodes[0].command("unready 0 raw")
                nodes.append(Node(config, 1, logs / "owner1.log"))
                nodes[1].marker("SMOKE READY")
                by_owner = nodes
            for owner, node in enumerate(by_owner):
                node.command(f"ready {owner} raw")
                node.command(f"reconnect {owner} raw")
            client = 720001 if both else 720000
            sequence = 0

            def admin(verb, seq, *args, expected_error=False):
                result = subprocess.run([str(ADMIN), verb, address, str(client), str(seq),
                    *map(str, args)], text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, timeout=60)
                with (logs / "admin.log").open("a") as stream:
                    stream.write(f"{verb} {seq} {args}: rc={result.returncode}\n{result.stdout}")
                match = re.search(r"generation=(\d+) outcome=(pending|committed|aborted)", result.stdout)
                if match is None:
                    if (expected_error and result.returncode == 1
                            and "failed: invalid" in result.stdout):
                        return None
                    raise RuntimeError(f"admin {verb}: {result.stdout}")
                outcome = match[2]
                if result.returncode != (1 if outcome == "aborted" else 0):
                    raise RuntimeError(f"wrong CLI terminal exit status: {result.stdout}")
                return int(match[1]), outcome

            def terminal(seq, expected):
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline:
                    result = admin("poll", seq)
                    if result[1] != "pending":
                        if result[1] != expected:
                            raise RuntimeError(f"wrong terminal outcome: {result}")
                        if admin("poll", seq) != result:
                            raise RuntimeError("terminal poll consumed/changed retained result")
                        return result
                    time.sleep(0.1)
                raise TimeoutError("migration did not terminate")

            def route(table, owner, minimum):
                # Snapshot watchers refresh after publication. The fixture waits
                # for the requested grant, then reports its actual epoch.
                result = by_owner[owner].command(f"route {owner} {table} {minimum}")
                return int(result.rsplit(" ", 1)[1])

            for table, name, lo, hi in (("raw", "smoke_raw", "6d", "6e"),
                                        ("warehouse", "warehouse", "00000001", "00000002")):
                previous = route(table, 0, 0)
                by_owner[0].command(f"check 0 {table} 0")
                by_owner[0].command(f"fault-arm 0 {table}")
                sequence += 1
                intent = (name, lo, hi, 0, 1)
                # The fixture drops an actual successful source Commit reply
                # before the native sink, requiring real cleanup-receipt retry.
                admin("begin", sequence, *intent)
                result = terminal(sequence, "committed")
                by_owner[0].command(f"fault-assert 0 {table}")
                if admin("begin", sequence, *intent) != result:
                    raise RuntimeError("duplicate begin did not retain the terminal result")
                if admin("begin", sequence, name, lo, hi, 1, 0, expected_error=True) is not None:
                    raise RuntimeError("changed intent reused an accepted nonce")
                current = route(table, 1, previous + 1)
                by_owner[0].command(f"stale 0 {table} {previous}")
                by_owner[1].command(f"check 1 {table} 0")
                by_owner[1].command(f"write 1 {table}")
                by_owner[1].command(f"check 1 {table} 1")
                sequence += 1
                admin("begin", sequence, name, lo, hi, 1, 0)
                terminal(sequence, "committed")
                returned = route(table, 0, current + 1)
                by_owner[1].command(f"stale 1 {table} {current}")
                by_owner[0].command(f"stale 0 {table} {previous}")
                by_owner[0].command(f"check 0 {table} 1")
                if returned <= current:
                    raise RuntimeError("owner return reused its old epoch")
                by_owner[0].command(f"replay 0 {table}")
                if route(table, 0, returned) != returned:
                    raise RuntimeError("old Start/Final/Commit changed returned owner epoch")
                by_owner[0].command(f"check 0 {table} 1")
                by_owner[1].command(f"stale 1 {table} {current}")
            by_owner[0].command("unmoved 0 raw")
            # A held real participant lease deterministically prevents commit.
            # Abort must terminate without consuming or changing the old grant.
            epoch = route("raw", 0, 0)
            by_owner[0].command("hold 0 raw")
            by_owner[0].command("capture 0 raw")
            sequence += 1
            admin("begin", sequence, "smoke_raw", "6d", "6e", 0, 1)
            pending = admin("poll", sequence)
            if pending[1] != "pending":
                raise RuntimeError("migration crossed an unresolved real access lease")
            admin("abort", sequence)
            by_owner[0].command("release 0 raw")
            terminal(sequence, "aborted")
            by_owner[0].command("capture-stop 0 raw")
            if route("raw", 0, epoch) != epoch:
                raise RuntimeError("abort published a replacement grant")
            by_owner[0].command("check 0 raw 1")
            # Keep the issued Abort bytes, commit a newer generation, and then
            # replay the old Abort (and any actually issued earlier controls)
            # to their original peers. Neither activation nor data may regress.
            sequence += 1
            admin("begin", sequence, "smoke_raw", "6d", "6e", 0, 1)
            terminal(sequence, "committed")
            current = route("raw", 1, epoch + 1)
            by_owner[1].command("check 1 raw 1")
            by_owner[0].command("replay-abort 0 raw")
            if route("raw", 1, current) != current:
                raise RuntimeError("old Abort changed the newer published epoch")
            by_owner[1].command("check 1 raw 1")
            by_owner[0].command(f"stale 0 raw {epoch}")
            sequence += 1
            admin("begin", sequence, "smoke_raw", "6d", "6e", 1, 0)
            terminal(sequence, "committed")
            returned = route("raw", 0, current + 1)
            by_owner[0].command("replay-abort 0 raw")
            if route("raw", 0, returned) != returned:
                raise RuntimeError("old Abort changed the newer returned epoch")
            by_owner[0].command("check 0 raw 1")
            by_owner[1].command(f"stale 1 raw {current}")
            success = True
        finally:
            for node in nodes:
                node.close(success)
    print(f"PASS {mode}: raw and warehouse owner return, values/absence, writes, "
          "forward/reverse pagination, epochs, retained outcomes, held-lease abort, "
          "real callback-boundary Commit reply loss/retry and issued old control replay", flush=True)

run(False)
run(True)
