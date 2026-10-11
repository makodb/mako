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
    def __init__(self, config, owner, log, metadata=None, initialize=False):
        self.log = log.open("wb")
        environment = {**os.environ, "MAKO_CLUSTER_CONFIG": "1"}
        # Keep the existing suite genuinely live-only even in an inherited env.
        environment.pop("MAKO_SHARD_METADATA_DIR", None)
        environment.pop("MAKO_SHARD_METADATA_INIT", None)
        if metadata is not None:
            environment["MAKO_SHARD_METADATA_DIR"] = str(metadata)
            if initialize:
                environment["MAKO_SHARD_METADATA_INIT"] = "1"
        try:
            # Rust diagnostics can span writes. Keep stderr out of the line-framed
            # stdout control channel so a log prefix cannot consume a SMOKE marker.
            self.process = subprocess.Popen([str(BINARY), str(config), str(owner)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
                env=environment)
        except BaseException:
            self.log.close()
            raise
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

    def kill(self):
        # Popen.kill is SIGKILL on Linux: no fixture/native graceful shutdown.
        self.process.kill()
        self.process.wait(timeout=30)
        self.close(False)

    def close(self, success):
        if self.log.closed:
            return
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
            try:
                self.process.stdin.close()
            finally:
                try:
                    self.process.stdout.close()
                finally:
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
                # for the requested grant, then reports its actual ownership version.
                result = by_owner[owner].command(f"route {owner} {table} {minimum}")
                return int(result.rsplit(" ", 1)[1])

            by_owner[0].command("remember-unmoved 0 raw")
            for table, name, lo, hi in (("raw", "smoke_raw", "6d", "6e"),
                                        ("warehouse", "warehouse", "00000001", "00000002")):
                previous = route(table, 0, 0)
                by_owner[0].command(f"remember 0 {table}")
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
                by_owner[0].command(f"probe-stale 0 {table}")
                by_owner[0].command("probe-unmoved 0 raw")
                by_owner[1].command(f"check 1 {table} 0")
                by_owner[1].command(f"write 1 {table}")
                by_owner[1].command(f"check 1 {table} 1")
                sequence += 1
                admin("begin", sequence, name, lo, hi, 1, 0)
                terminal(sequence, "committed")
                returned = route(table, 0, current + 1)
                by_owner[1].command(f"stale 1 {table} {current}")
                by_owner[0].command(f"stale 0 {table} {previous}")
                by_owner[0].command(f"probe-stale 0 {table}")
                by_owner[0].command(f"check 0 {table} 1")
                if returned <= current:
                    raise RuntimeError("owner return reused its old ownership version")
                by_owner[0].command(f"replay 0 {table}")
                if route(table, 0, returned) != returned:
                    raise RuntimeError("old Start/Final/Commit changed returned ownership version")
                by_owner[0].command(f"check 0 {table} 1")
                by_owner[1].command(f"stale 1 {table} {current}")
            by_owner[0].command("unmoved 0 raw")
            by_owner[0].command("gateway-forged 0 raw")
            # A held real participant lease deterministically prevents commit.
            # Abort must terminate without consuming or changing the old grant.
            ownership_version = route("raw", 0, 0)
            by_owner[0].command("remember 0 raw")
            by_owner[0].command("hold 0 raw")
            by_owner[0].command("capture 0 raw")
            sequence += 1
            admin("begin", sequence, "smoke_raw", "6d", "6e", 0, 1)
            by_owner[0].command("probe-frozen 0 raw")
            by_owner[0].command("probe-unmoved 0 raw")
            pending = admin("poll", sequence)
            if pending[1] != "pending":
                raise RuntimeError("migration crossed an unresolved real access lease")
            admin("abort", sequence)
            by_owner[0].command("release 0 raw")
            terminal(sequence, "aborted")
            by_owner[0].command("capture-stop 0 raw")
            if route("raw", 0, ownership_version) != ownership_version:
                raise RuntimeError("abort published a replacement grant")
            by_owner[0].command("check 0 raw 1")
            # Keep the issued Abort bytes, commit a newer generation, and then
            # replay the old Abort (and any actually issued earlier controls)
            # to their original peers. Neither activation nor data may regress.
            sequence += 1
            admin("begin", sequence, "smoke_raw", "6d", "6e", 0, 1)
            terminal(sequence, "committed")
            current = route("raw", 1, ownership_version + 1)
            by_owner[1].command("check 1 raw 1")
            by_owner[0].command("replay-abort 0 raw")
            if route("raw", 1, current) != current:
                raise RuntimeError("old Abort changed the newer published ownership version")
            by_owner[1].command("check 1 raw 1")
            by_owner[0].command(f"stale 0 raw {ownership_version}")
            sequence += 1
            admin("begin", sequence, "smoke_raw", "6d", "6e", 1, 0)
            terminal(sequence, "committed")
            returned = route("raw", 0, current + 1)
            by_owner[0].command("replay-abort 0 raw")
            if route("raw", 0, returned) != returned:
                raise RuntimeError("old Abort changed the newer returned ownership version")
            by_owner[0].command("check 0 raw 1")
            by_owner[1].command(f"stale 1 raw {current}")
            success = True
        finally:
            for node in nodes:
                node.close(success)
    print(f"PASS {mode}: raw and warehouse owner return, values/absence, writes, "
          "forward/reverse pagination, paired snapshot/ownership versions, stale-view fallback, "
          "matching-version freeze rejection, untrusted gateway fencing, retained outcomes, held-lease abort, "
          "real callback-boundary Commit reply loss/retry and issued old control replay", flush=True)

def run_metadata(both, pending):
    """SIGKILL retention of metadata only; MBTA rows are deliberately not restored."""
    mode = "single-process" if both else "two-process"
    outcome = "pending" if pending else "committed"
    logs = BUILD / "native-sharding-smoke" / f"metadata-{mode}-{outcome}"
    logs.mkdir(parents=True, exist_ok=True)
    base = ports()
    address = f"127.0.0.1:{base + 20000}"
    client, sequence = 730000, 1
    intent = ("smoke_raw", "6d", "6e", 0, 1)

    def admin(verb, seq=sequence, args=(), error=None):
        result = subprocess.run([str(ADMIN), verb, address, str(client), str(seq),
            *map(str, args)], text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=60)
        with (logs / "admin.log").open("a") as stream:
            stream.write(f"{verb} {seq} {args}: rc={result.returncode}\n{result.stdout}")
        if error is not None:
            if result.returncode != 1 or f"failed: {error}" not in result.stdout:
                raise RuntimeError(f"expected metadata rejection ({error}): {result.stdout}")
            return None
        match = re.search(r"generation=(\d+) outcome=(pending|committed|aborted)", result.stdout)
        if not match or result.returncode != (1 if match[2] == "aborted" else 0):
            raise RuntimeError(f"metadata admin {verb}: {result.stdout}")
        return int(match[1]), match[2]

    with tempfile.TemporaryDirectory(prefix="mako-metadata-smoke-") as directory:
        root = Path(directory)
        config = root / "config.yml"
        config.write_text(f"shards: 2\nwarehouses: 1\n"
                          f"memlocalhost: 0\nmemlearner: 0\nmemp1: 0\nmemp2: 0\nlocalhost:\n"
                          f"  - ip: 127.0.0.1\n    port: {base}\n"
                          f"  - ip: 127.0.0.1\n    port: {base + 100}\n")
        metadata = root / "metadata"
        nodes = []
        success = False

        def start(owner, initialize):
            node = Node(config, owner, logs / f"{owner}-{'init' if initialize else 'reopen'}.log",
                        metadata=metadata, initialize=initialize)
            nodes.append(node)
            node.marker("SMOKE READY")
            return node

        try:
            if both:
                origin = destination = start("both", True)
            else:
                origin = start(0, True)
                destination = start(1, True)
            origin.command("ready 0 raw")
            destination.command("ready 1 raw")
            if pending:
                origin.command("hold 0 raw")
            elif not both:
                origin.command("fault-arm 0 raw")
            begun = admin("begin", args=intent)
            deadline = time.monotonic() + 90
            while True:
                retained = admin("poll")
                if retained[1] == outcome:
                    break
                if retained[1] != "pending" or time.monotonic() >= deadline:
                    raise RuntimeError(f"metadata migration did not reach {outcome}: {retained}")
                time.sleep(0.1)
            if retained[0] != begun[0]:
                raise RuntimeError("metadata migration changed generation")
            if pending:
                # Ensure the real held lease keeps the migration pending while
                # the driver runs, rather than merely observing an early BEGIN.
                time.sleep(0.5)
                if admin("poll") != retained:
                    raise RuntimeError("held lease failed to pin pending metadata")
            else:
                if not both:
                    origin.command("fault-assert 0 raw")
                destination.command("route 1 raw 1")
                destination.command("check 1 raw 0")
                destination.command("write 1 raw")
                destination.command("check 1 raw 1")
                if admin("begin", args=intent) != retained:
                    raise RuntimeError("live committed nonce was not retained")
                if not both:
                    # Origin's captured bytes and real srpc host survive death
                    # of the destination. Even old Commit/cleanup must Retry.
                    destination.kill()
                    destination = start(1, False)
                    destination.command("metadata-fenced 1 raw")
                    origin.command("replay-target-fenced 0 raw 1")
            # Owner zero is both the migration origin and coordinator. Pending
            # death loses its real held lease, but must not allow orphan release
            # or automatically resume/abort the retained migration.
            origin.kill()
            if both:
                origin = destination = start("both", False)
                destination.command("metadata-fenced 1 raw")
            else:
                origin = start(0, False)
            origin.command("metadata-fenced 0 raw")
            for _ in range(3):
                if admin("poll") != retained or admin("begin", args=intent) != retained:
                    raise RuntimeError("reopened coordinator lost retained generation/outcome")
                admin("begin", args=("smoke_raw", "6d", "6e", 1, 0), error="invalid")
                admin("begin", seq=2, args=intent, error="retry")
                admin("abort", error="retry")
                time.sleep(0.1)
            if admin("poll") != retained:
                raise RuntimeError("fenced requests changed retained metadata")
            success = True
        finally:
            cleanup_errors = []
            for node in reversed(nodes):
                try:
                    node.close(success)
                except Exception as error:
                    cleanup_errors.append(error)
            if success and cleanup_errors:
                raise RuntimeError(f"metadata fixture cleanup failed: {cleanup_errors}")
    print(f"PASS metadata {mode} {outcome}: SIGKILL/reopen retained generation and nonce; "
          "all lease entrypoints/data readiness fenced; no row recovery or migration resume",
          flush=True)

run(False)
run(True)
run_metadata(False, False)
run_metadata(False, True)
run_metadata(True, False)
run_metadata(True, True)
