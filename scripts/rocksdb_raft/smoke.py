#!/usr/bin/env python3
"""Real SRPC/Raft/RocksDB process scenarios. Run inside the project Docker image."""
import argparse
import concurrent.futures
import importlib.util
import json
from pathlib import Path
import queue
import socket
import subprocess
import tempfile
import threading
import time


def byte(value):
    return list(value.encode() if isinstance(value, str) else value)


def available_ports(count):
    sockets = []
    try:
        for _ in range(count):
            sock = socket.socket()
            sock.bind(("127.0.0.1", 0))
            sockets.append(sock)
        return [sock.getsockname()[1] for sock in sockets]
    finally:
        for sock in sockets:
            sock.close()


class Rpc:
    def __init__(self, binary, config, endpoint):
        self.process = subprocess.Popen([str(binary), "--config", str(config), "--endpoint", endpoint, "--stdio", "--timeout-ms", "5000"],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.replies = queue.Queue()
        self.lock = threading.Lock()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            try:
                value = json.loads(line)
                if isinstance(value, dict) and "kind" in value:
                    self.replies.put(value)
            except ValueError:
                pass
        self.replies.put(None)

    def send(self, request):
        self.process.stdin.write(json.dumps(request, separators=(",", ":")) + "\n")
        self.process.stdin.flush()

    def call(self, request, timeout=20):
        with self.lock:
            self.send(request)
            result = self.replies.get(timeout=timeout)
            if result is None:
                raise RuntimeError("client process exited without a response")
            return result

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.process.stdin.close()
        self.process.stdout.close()


def good(response, kind=None):
    assert response["kind"] != "error", response
    if kind:
        assert response["kind"] == kind, response
    return response


def error(response, *codes):
    assert response["kind"] == "error", response
    assert response["error"]["code"] in codes, response
    return response


class Cluster:
    def __init__(self, binaries, root, start=True, replicas=3, shards=2, server_workers=40, short_timeouts=True, durability="sync"):
        self.binaries = binaries
        self.root = root
        self.processes = {}
        self.logs = {}
        self.clients = {}
        self.sequence = 0
        if durability not in ("sync", "async"):
            raise ValueError("durability must be sync or async")
        self.durability = durability
        if replicas not in (1, 3) or shards not in (1, 2) or not 1 <= server_workers <= 1024:
            raise ValueError("expected 1/3 replicas, 1/2 shards, and 1..1024 server workers")
        self.replicas = replicas
        self.shards = shards
        self.server_workers = server_workers
        self.ports = available_ports(replicas * (shards + 1))
        self.configs = {}
        server_limits = {"key_bytes": 128, "value_bytes": 262144}
        client_limits = {"rpc_bytes": 16 * 1024 * 1024}
        if short_timeouts:
            server_limits.update(commit_ms=3000, idle_ms=20000)
            client_limits["request_timeout_ms"] = 5000
        for node in range(1, replicas + 1):
            groups = []
            for group in range(1, shards + 1):
                peers = [{"id": peer, "client": self.endpoint(peer), "raft": self.raft_endpoint(peer, group)} for peer in range(1, replicas + 1)]
                groups.append({"id": group, "data_path": str(root / f"node{node}/group{group}/data"),
                               "raft_path": str(root / f"node{node}/group{group}/raft"), "raft_listen": self.raft_endpoint(node, group), "peers": peers})
            routing = {"version": 1, "shards": [{"id": g["id"], "peers": g["peers"]} for g in groups],
                       "tables": [{"id": index, "name": name, "boundaries": [
                {"start": [], "owner": 1, "ownership_version": 1},
                *([{"start": byte("m"), "owner": 2, "ownership_version": 1}] if shards == 2 else [])
            ]} for index, name in enumerate(("alpha", "beta", "bench"), 1)]}
            config = {"node": node, "listen": self.endpoint(node), "groups": groups, "routing": routing,
                      "workers_per_group": server_workers, "lane_capacity": 16, "admin_workers": 4, "admin_capacity": 32,
                      "limits": server_limits, "durability": durability}
            path = root / f"node{node}.json"
            path.write_text(json.dumps(config, indent=2))
            self.configs[node] = path
        self.client_config = root / "client.json"
        self.client_config.write_text(json.dumps({
            "routing": routing,
            "limits": client_limits,
            "context_capacity": 128,
        }, indent=2))
        if not start:
            return
        for node in range(1, replicas + 1):
            self.start(node, bootstrap=True)
        for node in range(1, replicas + 1):
            self.clients[node] = Rpc(binaries / "rocksdb_raft_client", self.client_config, self.endpoint(node))
        for group in range(1, shards + 1):
            self.leader(group)

    def endpoint(self, node):
        return f"127.0.0.1:{self.ports[node - 1]}"

    def raft_endpoint(self, node, group):
        return f"127.0.0.1:{self.ports[self.replicas + (group - 1) * self.replicas + node - 1]}"

    def start(self, node, bootstrap=False):
        log = open(self.root / f"node{node}.log", "ab", buffering=0)
        command = [str(self.binaries / "rocksdb_raft_server"), "--config", str(self.configs[node])]
        if bootstrap:
            command.append("--bootstrap")
        self.processes[node] = subprocess.Popen(command, stdout=log, stderr=log)
        self.logs[node] = log

    def kill(self, node):
        process = self.processes.pop(node)
        process.kill()
        process.wait(timeout=15)
        self.logs.pop(node).close()

    def request_id(self):
        self.sequence += 1
        return {"client": 947261, "sequence": self.sequence}

    def status(self, node, group):
        return self.clients[node].call({"kind": "status", "group": group})

    def leader(self, group, exclude=(), timeout=40):
        deadline = time.monotonic() + timeout
        states = {}
        while time.monotonic() < deadline:
            for node in sorted(self.processes):
                if node in exclude:
                    continue
                if self.processes[node].poll() is not None:
                    raise RuntimeError(f"server {node} exited; inspect {self.root / f'node{node}.log'}")
                result = self.status(node, group)
                states[node] = result
                if result["kind"] == "status" and result["is_leader"] and result["failure"] is None:
                    return node
            time.sleep(.1)
        raise TimeoutError(f"no leader for group {group}; last statuses: {json.dumps(states, sort_keys=True)}")

    def admin(self, group, kind, **fields):
        node = self.leader(group)
        result = self.clients[node].call({"kind": kind, "group": group, **fields})
        if result["kind"] == "error":
            states = {peer: self.status(peer, group) for peer in sorted(self.processes)}
            print(json.dumps({"admin_failure": kind, "selected_node": node, "result": result, "replicas": states}), flush=True)
        return result

    def begin(self, group=1, rpc=None):
        node = self.leader(group)
        rpc = rpc or self.clients[node]
        request = self.request_id()
        response = good(rpc.call({"kind": "begin", "group": group, "request": request}), "begun")
        return {"handle": response["handle"], "request": request, "node": node, "rpc": rpc}

    def execute(self, tx, op, table="alpha", group=None, **fields):
        return tx["rpc"].call({"kind": "execute", "handle": tx["handle"], "group": group or tx["handle"]["group"],
                               "table": table, "operation": {"op": op, **fields}})

    def put(self, tx, key, value, table="alpha"):
        return self.execute(tx, "put", table=table, key=byte(key), value=byte(value))

    def commit(self, tx):
        return tx["rpc"].call({"kind": "commit", "handle": tx["handle"]})

    def rollback(self, tx):
        return tx["rpc"].call({"kind": "rollback", "handle": tx["handle"]})

    def value(self, key, table="alpha", group=1):
        tx = self.begin(group)
        result = good(self.execute(tx, "get", table=table, key=byte(key)), "value")["value"]
        good(self.commit(tx), "committed")
        return result

    def write(self, key, value, table="alpha"):
        tx = self.begin()
        good(self.put(tx, key, value, table))
        return tx, good(self.commit(tx), "committed")

    def close(self):
        for client in self.clients.values():
            client.close()
        for node in list(self.processes):
            self.kill(node)


def create_tables(c):
    for group in range(1, c.shards + 1):
        for table in ("alpha", "beta", "bench"):
            good(c.admin(group, "create_table", request=c.request_id(), name=table), "committed")


def scenarios(c):
    create_tables(c)
    c.leader(1)
    helper = Path(__file__).resolve().parents[2] / "ci" / "rocksdb_raft_client_smoke.py"
    spec = importlib.util.spec_from_file_location("rocksdb_raft_client_smoke", helper)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load C consumer smoke helper: {helper}")
    consumer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(consumer)
    consumer.run(c.binaries, c.client_config, group=1, table="alpha")
    catalog = good(c.admin(1, "list_tables"), "tables")["tables"]
    assert len(catalog) == 3
    tx = c.begin()
    good(c.put(tx, b"a\x00key", b"v\x00one"))
    good(c.put(tx, "a-other", "second", "beta"))
    assert good(c.execute(tx, "get", key=byte(b"a\x00key")))["value"] == byte(b"v\x00one")
    committed = good(c.commit(tx), "committed")
    assert c.value("a-other", "beta") == byte("second")
    outcome = good(c.admin(1, "outcome", request=tx["request"]), "outcome")
    assert outcome["position"] == committed["position"]
    error(c.clients[c.leader(1)].call({"kind": "begin", "group": 1, "request": tx["request"]}), "invalid")
    aborted = c.begin()
    good(c.put(aborted, "a-rollback", "absent"))
    good(c.put(aborted, "a-other", "discarded", "beta"))
    good(c.rollback(aborted))
    assert c.value("a-rollback") is None
    assert c.value("a-other", "beta") == byte("second")
    print("PASS same-DB atomic multi-table commit, read-your-writes, rollback, recorded outcome and duplicate request rejection", flush=True)

    for bad_group, bad_key in ((2, "a-other-db"), (1, "z-other-shard")):
        tx = c.begin()
        good(c.put(tx, "a-before-cross", "must-not-publish"))
        error(c.execute(tx, "put", group=bad_group, key=byte(bad_key), value=byte("bad")), "cross_shard")
        error(c.commit(tx), "cross_shard")
        assert c.value("a-before-cross") is None
    tx = c.begin()
    error(c.execute(tx, "scan", start=[], end=None, limit=1), "cross_shard")
    error(c.commit(tx), "cross_shard")
    print("PASS co-located DB/key/range cross-shard rejection poisons the whole transaction", flush=True)

    tx = c.begin()
    good(c.execute(tx, "insert", key=byte("a-order-1"), value=byte("one")))
    good(c.execute(tx, "insert", key=byte("a-order-2"), value=byte("two")))
    rows = good(c.execute(tx, "reverse_scan", start=byte("a-order-"), end=byte("a-order."), limit=10), "rows")["rows"]
    assert [row[0] for row in rows] == [byte("a-order-2"), byte("a-order-1")]
    good(c.commit(tx))
    tx = c.begin()
    error(c.execute(tx, "insert", key=byte("a-order-1"), value=byte("duplicate")), "conflict")
    error(c.commit(tx), "conflict")
    tx = c.begin()
    error(c.execute(tx, "get", key=[97] * 129), "too_large")
    error(c.commit(tx), "too_large")
    print("PASS Insert, reverse scan own intents and explicit bounded-operation errors", flush=True)

    for kind in ("read", "phantom", "aba"):
        stale = c.begin()
        if kind == "read":
            good(c.execute(stale, "get", key=byte("a-order-1")))
            c.write("a-order-1", "changed")
        elif kind == "phantom":
            assert good(c.execute(stale, "scan", start=byte("a-phantom-"), end=byte("a-phantom."), limit=10))["rows"] == []
            c.write("a-phantom-new", "inserted")
        else:
            assert good(c.execute(stale, "get", key=byte("a-aba")))["value"] is None
            c.write("a-aba", "inserted")
            remove = c.begin()
            good(c.execute(remove, "delete", key=byte("a-aba")))
            good(c.commit(remove))
        staged = c.put(stale, f"a-no-{kind}", "must-abort")
        if staged["kind"] == "error":
            error(staged, "conflict", "closed")
        error(c.commit(stale), "conflict", "closed")
        assert c.value(f"a-no-{kind}") is None
    print("PASS stale tracked read, empty-scan phantom and absent-key insert/delete ABA conflict", flush=True)

    node = c.leader(1)
    parallel = [Rpc(c.binaries / "rocksdb_raft_client", c.client_config, c.endpoint(node)) for _ in range(8)]
    try:
        attempts = []
        for index, rpc in enumerate(parallel):
            tx = c.begin(rpc=rpc)
            good(c.put(tx, f"a-pipeline-{index}", "committed"))
            attempts.append(tx)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(c.commit, attempts))
        for result in results:
            good(result, "committed")
    finally:
        for rpc in parallel:
            rpc.close()
    print("PASS eight independent admitted transactions commit concurrently without a quorum-held certification gate", flush=True)

    lost = Rpc(c.binaries / "rocksdb_raft_client", c.client_config, c.endpoint(node))
    try:
        tx_lost = c.begin(rpc=lost)
        good(c.put(tx_lost, "a-lost-reply", "durable"))
        lost.send({"kind": "commit", "handle": tx_lost["handle"]})
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            resolved = good(c.admin(1, "outcome", request=tx_lost["request"]), "outcome")
            if resolved["position"] is not None:
                break
            time.sleep(.05)
        else:
            raise AssertionError("discarded commit reply never resolved")
        # Deliberately never consume the commit response. Recovery uses only id.
    finally:
        lost.close()
    assert c.value("a-lost-reply") == byte("durable")
    print("PASS deliberately discarded commit response resolves by recorded request identity without replay", flush=True)

    old = c.leader(1)
    unproposed = c.begin()
    good(c.put(unproposed, "a-unproposed", "must-not-publish"))
    c.kill(old)
    replacement = c.leader(1, exclude=(old,))
    assert replacement != old
    assert c.value(b"a\x00key") == byte(b"v\x00one")
    assert c.value("a-other", "beta") == byte("second")
    assert c.value("a-unproposed") is None
    assert good(c.admin(1, "outcome", request=tx_lost["request"]))["position"] == resolved["position"]
    c.start(old)
    deadline = time.monotonic() + 30
    target = c.status(c.leader(1), 1)["committed"]
    while time.monotonic() < deadline:
        status = c.status(old, 1)
        if status["kind"] == "status" and status["applied"] >= target:
            break
        time.sleep(.1)
    else:
        raise AssertionError("restarted replica failed application catch-up")
    error(c.commit(unproposed), "invalid_handle", "not_leader", "transport", "unknown_outcome")
    print("PASS leader SIGKILL/failover preserves acknowledged data/outcomes; stale unproposed context never publishes", flush=True)

    lagging = next(node for node in c.processes if node != c.leader(1))
    c.kill(lagging)
    for i in range(8):
        c.write(f"a-checkpoint-{i}", f"value-{i}")
    checkpoint = good(c.admin(1, "checkpoint"), "checkpoint")["position"]
    c.start(lagging)
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        status = c.status(lagging, 1)
        if status["kind"] == "status" and status["applied"] >= checkpoint["index"]:
            break
        time.sleep(.1)
    else:
        raise AssertionError("lagging replica did not install checkpoint/catch up")
    for node in list(c.processes):
        c.kill(node)
    for node in (1, 2, 3):
        c.start(node)
    for group in (1, 2):
        c.leader(group)
    assert good(c.admin(1, "list_tables"))["tables"] == catalog
    assert c.value("a-checkpoint-7") == byte("value-7")
    assert c.value("a-lost-reply") == byte("durable")
    print("PASS compacted-prefix follower catch-up, full-cluster restart and stable catalog/data recovery", flush=True)


def application_failures(c):
    """Requires the explicit rocksdb-raft-service/test-hooks build feature."""
    for after_batch in (False, True):
        node = c.leader(1)
        tx = c.begin()
        key = "a-io-after" if after_batch else "a-io-before"
        good(c.put(tx, key, "committed-despite-local-failure"))
        good(c.clients[node].call({"kind": "inject_apply_failure", "group": 1, "after_batch": after_batch}))
        before = good(c.status(node, 1), "status")["applied"]
        error(c.commit(tx), "storage_failure")
        failed = good(c.status(node, 1), "status")
        assert failed["failure"] is not None, failed
        assert failed["applied"] == before, (before, failed)
        error(c.clients[node].call({"kind": "begin", "group": 1, "request": c.request_id()}), "storage_failure")
        c.kill(node)
        c.leader(1, exclude=(node,))
        assert c.value(key) == byte("committed-despite-local-failure")
        assert good(c.admin(1, "outcome", request=tx["request"]))["position"] is not None
        c.start(node)
        target = c.status(c.leader(1), 1)["committed"]
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            status = c.status(node, 1)
            if status["kind"] == "status" and status["failure"] is None and status["applied"] >= target:
                break
            time.sleep(.1)
        else:
            raise AssertionError("failed application did not reconcile on restart")
        print(f"PASS real {'post' if after_batch else 'pre'}-batch application failure fences admission/progress; committed command resolves on restart", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", required=True, type=Path)
    parser.add_argument("--data-dir", type=Path)
    parser.add_argument("--durability", choices=("sync", "async"), default="sync",
                        help="async omits foreground durability barriers; process-crash tests do not prove power-loss durability")
    parser.add_argument("--faults", action="store_true", help="requires explicit test-hooks build; test both ambiguous and definite local batch errors")
    parser.add_argument("--faults-only", action="store_true", help="only the test-hooks application failure scenarios")
    args = parser.parse_args()
    binaries = args.bin_dir.resolve()
    for name in ("rocksdb_raft_server", "rocksdb_raft_client"):
        if not (binaries / name).is_file():
            raise FileNotFoundError(binaries / name)
    temporary = tempfile.TemporaryDirectory(prefix="rocksdb-raft-smoke-") if args.data_dir is None else None
    root = Path(temporary.name) if temporary else args.data_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        raise ValueError("--data-dir must be empty (never overwrite an existing cluster)")
    cluster = None
    try:
        cluster = Cluster.__new__(Cluster)
        cluster.__init__(binaries, root, durability=args.durability)
        if args.durability == "async":
            print("ASYNC: WAL enabled without foreground sync; no power/machine-crash durability guarantee.", flush=True)
        if args.faults_only:
            create_tables(cluster)
        else:
            scenarios(cluster)
        if args.faults or args.faults_only:
            application_failures(cluster)
        print(json.dumps({"suite": "rocksdb_raft_smoke", "status": "passed", "root": str(root),
                          "durability": args.durability, "power_crash_durable": args.durability == "sync"}))
    except BaseException:
        for log in root.glob("node*.log"):
            print(f"--- {log} ---", flush=True)
            print(log.read_text(errors="replace")[-16000:], flush=True)
        raise
    finally:
        if cluster:
            cluster.close()
        if temporary:
            temporary.cleanup()


if __name__ == "__main__":
    main()
