#!/usr/bin/env python3
"""Bounded separate-process STO/SRPC functional gate (run via Docker/CTest).

Generates all configuration under --output, never edits source configuration,
never searches for or kills unrelated processes, and makes no performance
comparison. Replicated scenarios run Raft with three voting replicas (the
deprecated Paxos mode, with its learner, is no longer a choice here; see
src/deptran/paxos/DEPRECATED.md). Every leader must commit remote NewOrder and
Payment work.
Every nonleader must report a nonempty batch consumed by the real replay
backend. That receipt proves replay execution, not an exact payload digest
(the native integration tests cover payload and source-version correctness).
"""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import random
import re
import resource
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from collections.abc import Sequence
from dataclasses import dataclass

from run_sto_tpcc_ctest import RESULT_PREFIX, positive_int, validate_result


REPLAY_PREFIX = "STO_PROTOCOL_REPLAY_RESULT "
ROLES = ("localhost", "p1", "p2", "learner")
# How much of a failed child's stdout and stderr is copied into the test output.
TAIL_LINES = 40
TAIL_BYTES = 16384


def dump_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--engine", required=True, choices=("rust", "cpp"))
    parser.add_argument("--shards", type=positive_int, default=2)
    parser.add_argument("--replication", choices=("none", "raft"), default="none")
    parser.add_argument("--colocated-leader", action="store_true",
                        help="host all leader shards together; replicas use identical full topology")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--runtime", type=positive_int, default=5,
                        help="workload seconds per leader (maximum 30; no throughput campaign)")
    parser.add_argument("--timeout", type=positive_int, default=600,
                        help="whole scenario deadline, including loading and replica drain")
    args = parser.parse_args(argv)
    if not 2 <= args.shards <= 31:
        parser.error("--shards must be between 2 and 31 (cross-shard wire bitset)")
    if args.runtime > 30:
        parser.error("--runtime must not exceed 30 for this functional-only gate")
    if not args.runtime < args.timeout <= 3600:
        parser.error("--timeout must exceed --runtime and be at most 3600")
    args.binary = args.binary.resolve(strict=True)
    if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
        parser.error("--binary must name an executable file")
    args.output = args.output.resolve()
    return args


# Lowest port handed to a child; keeps clear of common developer-machine services.
MIN_LISTEN_PORT = 10000
# Linux's default net.ipv4.ip_local_port_range, assumed when /proc is unreadable.
DEFAULT_EPHEMERAL_RANGE = (32768, 60999)


def parse_port_ranges(text: str) -> list[tuple[int, int]]:
    """Parse ip_local_reserved_ports syntax: "a-b,c,d-e" (may be empty)."""
    ranges = []
    for item in text.replace("\n", ",").split(","):
        item = item.strip()
        if item:
            low, _, high = item.partition("-")
            ranges.append((int(low), int(high or low)))
    return ranges


class Ports:
    """Hold wildcard TCP reservations until all configuration has been written.

    Production SRPC listens on 0.0.0.0 even for loopback configurations. Reserve
    on that same address, including worker and helper-server offsets.

    The reservations are released before the children exec, and each child
    binds its own listeners. Ports therefore come only from ports the kernel
    never hands out by itself: outside net.ipv4.ip_local_port_range, or inside
    net.ipv4.ip_local_reserved_ports. No outgoing connect() source port and no
    bind() to port 0 (by a child or anything else) can take one between release
    and exec. A port chosen by bind(0) would be in that ephemeral pool: under
    CI's reserved-ports sysctl, bind(0) and connect() both crowd into the few
    unreserved ports, and a peer's outgoing connection regularly took a
    released port before its owner bound it. Only an explicit bind by an
    unrelated process can still collide; a bind failure fails the scenario
    rather than selecting a different peer silently. If the kernel leaves no
    such ports (an ephemeral range covering everything), fall back to bind(0).
    """

    def __init__(self) -> None:
        self.sockets: list[socket.socket] = []
        self.allocated: set[int] = set()
        self.random = random.SystemRandom()
        self.ephemeral_range = DEFAULT_EPHEMERAL_RANGE
        self.reserved_ranges: list[tuple[int, int]] = []
        self.policy_source = "assumed default"
        try:
            low, high = map(int, Path("/proc/sys/net/ipv4/ip_local_port_range").read_text().split())
            self.ephemeral_range = (low, high)
            self.policy_source = "kernel"
            self.reserved_ranges = parse_port_ranges(
                Path("/proc/sys/net/ipv4/ip_local_reserved_ports").read_text())
        except (OSError, ValueError):
            # Treating nothing as reserved only shrinks the candidate set.
            self.reserved_ranges = []
        # kernel_assignable[port] is 1 when bind(0) or connect() may pick it.
        self.kernel_assignable = bytearray(65536)
        low, high = self.ephemeral_range
        for port in range(max(low, 0), min(high, 65535) + 1):
            self.kernel_assignable[port] = 1
        for low, high in self.reserved_ranges:
            for port in range(max(low, 0), min(high, 65535) + 1):
                self.kernel_assignable[port] = 0
        self.candidates: dict[tuple[int, ...], list[int]] = {}
        self.fallback_blocks = 0

    def candidate_bases(self, offsets: tuple[int, ...]) -> list[int]:
        bases = self.candidates.get(offsets)
        if bases is None:
            assignable = self.kernel_assignable
            bases = [base for base in range(MIN_LISTEN_PORT, 65536 - offsets[-1])
                     if not any(assignable[base + offset] for offset in offsets)]
            self.candidates[offsets] = bases
        return bases

    def block(self, count: int = 1, *, control: bool = False) -> int:
        offsets = set(range(count))
        if control:
            # PaxosWorker and RaftWorker both use CtrlPortDelta = 10000.
            offsets.add(10000)
        bases = self.candidate_bases(tuple(sorted(offsets)))
        for _ in range(200):
            held: list[socket.socket] = []
            try:
                first = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                held.append(first)
                first.bind(("0.0.0.0", self.random.choice(bases) if bases else 0))
                base = first.getsockname()[1]
                reserved = {base + offset for offset in offsets}
                if any(port >= 65536 or port in self.allocated for port in reserved):
                    continue
                for port in sorted(reserved - {base}):
                    item = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    held.append(item)
                    item.bind(("0.0.0.0", port))
                self.sockets.extend(held)
                self.allocated.update(reserved)
                if not bases:
                    self.fallback_blocks += 1
                held = []
                return base
            except OSError:
                pass
            finally:
                for item in held:
                    item.close()
        raise RuntimeError("could not reserve a unique loopback port block")

    def description(self) -> dict[str, object]:
        return {"ephemeral_range": list(self.ephemeral_range),
                "reserved_ranges": [list(item) for item in self.reserved_ranges],
                "policy_source": self.policy_source,
                "kernel_assigned_fallback_blocks": self.fallback_blocks,
                "kernel_assignable_allocated": sorted(
                    port for port in self.allocated if self.kernel_assignable[port])}

    def close(self) -> None:
        for item in self.sockets:
            item.close()
        self.sockets.clear()


def write_configs(directory: Path, args: argparse.Namespace, ports: Ports) -> tuple[Path, list[Path], Path]:
    directory.mkdir()
    # Children listen inside each 8-port block: the leader at offsets 0, 6 and
    # 7, replicas at 2 and 3. The shard file still names all four ROLES; no
    # learner process is started (only the deprecated Paxos mode had one).
    transport = {role: [ports.block(8) for _ in range(args.shards)] for role in ROLES}
    lines = [f"shards: {args.shards}", "replicas: 3", "warehouses: 1"]
    for role in ROLES:
        lines.append(f"{role}:")
        for shard, port in enumerate(transport[role]):
            lines.extend((f"  - name: shard{shard}", f"    index: {shard}",
                          "    ip: 127.0.0.1", f"    port: {port}"))
    # These ports identify NFSSync files, not an external memcached service.
    for role in ROLES:
        lines.append(f"mem{role}: {ports.block()}")
    shard_config = directory / "shards.yml"
    shard_config.write_text("\n".join(lines) + "\n", encoding="utf-8")
    replication_configs = []
    # Raft has three voting replicas and no learner.
    roles = ROLES[:3]
    if args.replication != "none":
        for shard in range(args.shards):
            sites = [f"s{100 * (index + 1) + 1}" for index in range(len(roles))]
            addresses = [f"{site}:{ports.block(control=True)}" for site in sites]
            content = ["site:", "  server:", "    - " + json.dumps(addresses), "process:"]
            content.extend(f"  {site}: {role}" for site, role in zip(sites, roles))
            content.append("host:")
            content.extend(f"  {role}: 127.0.0.1" for role in roles)
            path = directory / f"{args.replication}_shard{shard}.yml"
            path.write_text("\n".join(content) + "\n", encoding="utf-8")
            replication_configs.append(path)
    mode = directory / "mode.yml"
    # Only a replicated child is handed this file (start_child).
    protocol = "raft" if args.replication == "raft" else "none"
    mode.write_text(f"mode:\n  ab: {protocol}\n  batch: false\n  retry: 20\n", encoding="utf-8")
    dump_json(directory / "ports.json", sorted(ports.allocated))
    dump_json(directory / "port_selection.json", ports.description())
    return shard_config, replication_configs, mode


@dataclass
class Child:
    shard: int
    role: str
    directory: Path
    process: subprocess.Popen[bytes]

    def text(self) -> str:
        return "\n".join((self.directory / name).read_text(encoding="utf-8", errors="replace")
                         for name in ("stdout.log", "stderr.log"))


def log_tail(path: Path) -> str:
    """The last TAIL_LINES lines of a log, reading at most TAIL_BYTES of it."""
    try:
        with path.open("rb") as stream:
            size = stream.seek(0, os.SEEK_END)
            stream.seek(max(0, size - TAIL_BYTES))
            lines = stream.read().decode("utf-8", errors="replace").splitlines()
    except OSError as error:
        return f"<unreadable: {error}>"
    if size > TAIL_BYTES:
        lines = lines[1:]  # the read started in the middle of this line
    return "\n".join(lines[-TAIL_LINES:]) or "<empty>"


def print_child_logs(children: list[Child]) -> None:
    # CI keeps this process's output but not the evidence directory, so a
    # child's last words are otherwise lost. Only the two log files are shown:
    # env.json holds the caller's environment and must stay out of CI logs.
    for child in children:
        for name in ("stdout.log", "stderr.log"):
            print(f"--- {child.directory.name}/{name}, last lines ---\n"
                  f"{log_tail(child.directory / name)}", file=sys.stderr)


def start_child(args: argparse.Namespace, shard: int, role: str, work: Path,
                config: Path, replication: list[Path], mode: Path,
                environment: dict[str, str]) -> Child:
    directory = args.output / f"shard{shard}-{role}"
    directory.mkdir()
    command = [str(args.binary), "--storage-engine", args.engine, "--num-threads", "1",
               "--shard-config", str(config), "--shard-index", str(shard),
               "--paxos-proc-name", role, "--runtime", str(args.runtime),
               "--sync-dir", str(work / "sync"), "--startup-timeout-sec", str(args.timeout)]
    if args.colocated_leader and role == "localhost":
        command.extend(("--local-shards", ",".join(map(str, range(args.shards)))))
    if args.replication != "none":
        command.extend(("--is-replicated", "--replication", args.replication))
        for path in replication if args.colocated_leader else [replication[shard]]:
            command.extend(("-F", str(path)))
        command.extend(("-F", str(mode)))
    dump_json(directory / "argv.json", command)
    dump_json(directory / "env.json", environment)
    (directory / "env.json").chmod(0o600)
    dump_json(directory / "cwd.json", str(work))
    with (directory / "stdout.log").open("wb") as stdout, (directory / "stderr.log").open("wb") as stderr:
        process = subprocess.Popen(command, cwd=work, env=environment, stdin=subprocess.DEVNULL,
                                   stdout=stdout, stderr=stderr, start_new_session=True)
    return Child(shard, role, directory, process)


def records(text: str, prefix: str) -> list[object]:
    values = []
    for line in text.splitlines():
        # Replay receipts are emitted by stdio; tolerate the logger's prefix on
        # an adjacent partial diagnostic, but do not accept malformed JSON.
        position = line.find(prefix)
        if position >= 0:
            values.append(json.loads(line[position + len(prefix):]))
    return values


def validate_child(child: Child, args: argparse.Namespace) -> dict[str, object]:
    if child.process.returncode != 0:
        raise RuntimeError(f"{child.directory.name} exited {child.process.returncode}")
    text = child.text()
    if "timed out waiting for end signal" in text:
        raise RuntimeError(f"{child.directory.name} did not receive replication termination")
    receipt: dict[str, object] = {"shard": child.shard, "role": child.role,
                                 "exit_code": child.process.returncode}
    if child.role == "localhost":
        results = records(text, RESULT_PREFIX)
        count = args.shards if args.colocated_leader else 1
        if len(results) != count:
            raise RuntimeError(f"{child.directory.name}: expected {count} {RESULT_PREFIX.strip()} records")
        expected = argparse.Namespace(expected_engine=args.engine, expected_threads=1,
                                      expected_warehouses=1, expected_seconds=args.runtime)
        for result in results:
            validate_result(result, expected)
        remote = {}
        for workload in ("NewOrder", "Payment"):
            evidence = {}
            for metric in ("remote_ratio", "remote_commit_latency"):
                matches = re.findall(rf"\b{workload}_{metric}:\s*([0-9.eE+\-]+)", text)
                if len(matches) != count:
                    raise RuntimeError(f"{child.directory.name}: missing/duplicate {workload}_{metric}")
                values = list(map(float, matches))
                if any(not math.isfinite(value) or value <= 0 for value in values):
                    raise RuntimeError(f"{child.directory.name}: no committed remote {workload} evidence")
                evidence[metric] = values
            remote[workload] = evidence
        receipt.update(results=results, remote_commits_evidence=remote)
    else:
        replay = records(text, REPLAY_PREFIX)
        if not replay:
            raise RuntimeError(f"{child.directory.name}: no successful nonempty backend replay receipt")
        for item in replay:
            if not isinstance(item, dict) or item.get("schema_version") != 1 or \
                    item.get("engine") != args.engine or item.get("shard") != child.shard:
                raise RuntimeError(f"{child.directory.name}: invalid replay receipt identity")
            for field in ("processed_records", "recovered_time_term"):
                if type(item.get(field)) is not int or item[field] <= 0:
                    raise RuntimeError(f"{child.directory.name}: invalid replay {field}")
            timestamp = item["recovered_time_term"]
            if timestamp < 10 or timestamp // 10 > ((1 << 32) - 10) // 10:
                raise RuntimeError(f"{child.directory.name}: invalid replay decimal timestamp")
        receipt["replay"] = replay
    dump_json(child.directory / "receipt.json", receipt)
    return receipt


def cleanup(children: list[Child]) -> None:
    # Every child is a session/process-group leader. Kill only these groups,
    # including descendants left behind by a direct child that already exited.
    for child in children:
        try:
            os.killpg(child.process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + 3
    for child in children:
        try:
            child.process.wait(timeout=max(0.01, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            pass
    for child in children:
        try:
            os.killpg(child.process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.process.wait()
    # Finish terminating all groups before any filesystem cleanup can fail.
    for child in children:
        dump_json(child.directory / "exit.json", {"exit_code": child.process.returncode})
        # Production RocksDB chooses a PID-scoped /tmp path independently of
        # cwd. Only remove a path actually announced by this exact child, with
        # both its shard and PID encoded; never glob a user's shared /tmp area.
        removed = []
        pattern = rf"Leader initializing RocksDB at path: (/tmp/[^/\s]+_mako_rocksdb_shard{child.shard}_leader_pid{child.process.pid}) with"
        for match in re.finditer(pattern, child.text()):
            path = Path(match.group(1))
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
                removed.append(str(path))
        dump_json(child.directory / "cleanup.json", {"removed_owned_rocksdb": removed})


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    args.output.mkdir(parents=True, exist_ok=True)
    # Repeated CTest runs retain their own evidence without overwriting a prior
    # attempt. mkdtemp also protects captured child environments with mode 0700.
    args.output = Path(tempfile.mkdtemp(prefix="run-", dir=args.output))
    work = Path(tempfile.mkdtemp(prefix="work-", dir=args.output))
    (work / "sync").mkdir()
    children: list[Child] = []
    # The children whose logs explain a failure. Chosen before cleanup(),
    # which terminates every child and so makes them all look failed.
    suspects: list[Child] = []
    ports = Ports()
    summary: dict[str, object] = {"schema_version": 1, "engine": args.engine,
                                  "shards": args.shards, "replication": args.replication,
                                  "runtime": args.runtime, "timeout": args.timeout,
                                  "work_directory": str(work), "status": "failed"}
    summary["topology"] = "colocated-leader" if args.colocated_leader else "separate-leaders"
    old_handlers = {}

    def interrupted(signum: int, _frame: object) -> None:
        raise RuntimeError(f"scenario interrupted by signal {signum}")

    try:
        for signum in (signal.SIGINT, signal.SIGTERM):
            old_handlers[signum] = signal.signal(signum, interrupted)
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        desired = 65535 if hard == resource.RLIM_INFINITY else min(65535, hard)
        if soft < desired:
            resource.setrlimit(resource.RLIMIT_NOFILE, (desired, hard))
        config, replication, mode = write_configs(work / "configs", args, ports)
        summary["port_selection"] = ports.description()
        if ports.fallback_blocks:
            print(f"warning: the kernel may assign every port >= {MIN_LISTEN_PORT}; "
                  f"{ports.fallback_blocks} port blocks came from bind(0) and can race "
                  "with outgoing connections", file=sys.stderr)
        environment = os.environ.copy()
        environment.update(MAKO_TPCC_WORKLOAD_MIX="50,50,0,0,0",
                           MAKO_STO_PROTOCOL_RECEIPTS="1",
                           MAKO_FOLLOWER_END_WAIT_SECONDS=str(args.timeout))
        if args.colocated_leader and args.replication != "none":
            environment["MAKO_FULL_REPLICATION_TOPOLOGY"] = "1"
        roles = ("localhost",) if args.replication == "none" else ("p1", "p2", "localhost")
        deadline = time.monotonic() + args.timeout
        ports.close()
        for role in roles:
            shards = [0] if args.colocated_leader and role == "localhost" else range(args.shards)
            for shard in shards:
                children.append(start_child(args, shard, role, work, config, replication, mode, environment))
                child = children[-1]
                dump_json(child.directory / "process.json",
                          {"pid": child.process.pid, "pgid": child.process.pid})
        while True:
            pending = []
            # Poll every process; short-circuiting on the first live peer hides
            # a later peer's failure behind the scenario timeout.
            for child in children:
                if child.process.poll() is None:
                    pending.append(child)
                if child.process.returncode not in (None, 0):
                    suspects = [child]
                    raise RuntimeError(f"{child.directory.name} exited {child.process.returncode}; see its logs")
            if not pending:
                break
            if time.monotonic() >= deadline:
                suspects = pending
                names = ", ".join(child.directory.name for child in pending)
                raise RuntimeError(f"scenario timed out waiting for {names}")
            time.sleep(0.2)
        receipts = []
        for child in children:
            suspects = [child]
            receipts.append(validate_child(child, args))
        suspects = []
        summary["receipts"] = receipts
        summary["status"] = "passed"
    except (OSError, RuntimeError, ValueError) as error:
        summary["error"] = str(error)
    finally:
        for signum in old_handlers:
            signal.signal(signum, signal.SIG_IGN)
        ports.close()
        try:
            cleanup(children)
        except OSError as error:
            summary["status"] = "failed"
            summary["cleanup_error"] = str(error)
        for signum, handler in old_handlers.items():
            signal.signal(signum, handler)
        dump_json(args.output / "receipt.json", summary)
    if summary["status"] != "passed":
        print(f"STO protocol scenario failed: {summary.get('error', summary.get('cleanup_error'))}; "
              f"evidence: {args.output}", file=sys.stderr)
        print_child_logs(suspects)
        return 1
    print(f"Validated {args.engine} STO/{args.replication}: {args.shards} shards, {summary['topology']}, "
          f"remote NewOrder/Payment commits and all required replica replay receipts; {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"STO protocol scenario failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
