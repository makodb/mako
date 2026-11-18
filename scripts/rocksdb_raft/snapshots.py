#!/usr/bin/env python3
"""Transfer/recover a real >64MiB RocksDB checkpoint; test-hooks build required."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

from boundaries import catch_up, until
from smoke import Cluster, byte, create_tables, good

ROWS = 288
VALUE_BYTES = 256 * 1024


def key(index):
    return f"a-large-{index:04d}".encode()


def value(index):
    # Incompressible deterministic data: repeated-byte padding would not prove
    # that the native RocksDB checkpoint exceeds the SRPC single-frame limit.
    return hashlib.shake_256(key(index)).digest(VALUE_BYTES)


def inventory_bytes(path):
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())


def checkpoint(cluster):
    target = good(cluster.status(cluster.leader(1), 1), "status")["committed"]
    for node in sorted(cluster.processes):
        until(lambda node=node: good(cluster.status(node, 1), "status")["applied"] >= target,
              f"replica {node} has not applied checkpoint prefix")
    positions = []
    sizes = []
    # Every electable survivor must compact, otherwise a leadership change can
    # let an uncompacted replica send the full log and bypass snapshot recovery.
    for node in sorted(cluster.processes):
        result = good(cluster.clients[node].call({"kind": "checkpoint", "group": 1}, timeout=60), "checkpoint")
        positions.append(result["position"]["index"])
        directory = cluster.root / f"node{node}/group1/raft/checkpoints"
        sizes.append(max(inventory_bytes(generation) for generation in directory.iterdir() if generation.is_dir()))
    assert min(sizes) > 64 * 1024 * 1024, sizes
    return min(positions), min(sizes)


def verify_replica(cluster, node):
    # Test-only snapshot inspection intentionally bypasses leader routing; it
    # compares actual follower DB contents rather than inferring them from an ACK.
    start = b"a-large-"
    observed = 0
    digest = hashlib.sha256()
    while True:
        response = good(cluster.clients[node].call({"kind": "test_local_scan", "group": 1,
            "table": "alpha", "start": byte(start), "end": byte("a-large."), "limit": 4}, timeout=30), "rows")
        rows = response["rows"]
        if not rows:
            break
        for encoded_key, encoded_value in rows:
            actual_key, actual_value = bytes(encoded_key), bytes(encoded_value)
            assert actual_key == key(observed), (node, observed, actual_key)
            assert actual_value == value(observed), (node, observed, "value mismatch")
            digest.update(actual_key)
            digest.update(actual_value)
            observed += 1
        start = bytes(rows[-1][0]) + b"\x00"
    assert observed == ROWS, (node, observed)
    return digest.hexdigest()


def start_crashing(cluster, node, point):
    log = open(cluster.root / f"node{node}.log", "ab", buffering=0)
    environment = dict(os.environ, RAFT_HOST_CRASH_POINT=point, RAFT_HOST_CRASH_GROUP="1")
    cluster.processes[node] = subprocess.Popen([
        str(cluster.binaries / "rocksdb_raft_server"), "--config", str(cluster.configs[node])],
        stdout=log, stderr=log, env=environment)
    cluster.logs[node] = log


def scenarios(cluster):
    create_tables(cluster)
    lagging = next(node for node in cluster.processes if node != cluster.leader(1))
    cluster.kill(lagging)
    for first in range(0, ROWS, 8):
        tx = cluster.begin()
        for index in range(first, min(first + 8, ROWS)):
            good(cluster.put(tx, key(index), value(index)))
        good(cluster.commit(tx), "committed")
    target, image_bytes = checkpoint(cluster)
    print(json.dumps({"snapshot_user_bytes": ROWS * VALUE_BYTES, "checkpoint_bytes": image_bytes,
                      "checkpoint_index": target, "lagging_node": lagging}), flush=True)

    cluster.start(lagging)
    incoming = cluster.root / f"node{lagging}/group1/raft/checkpoints"
    def partial():
        if not incoming.exists():
            return None
        for directory in incoming.glob("incoming-*"):
            size = inventory_bytes(directory)
            if 4 * 1024 * 1024 <= size < image_bytes:
                return size
        return None
    interrupted_bytes = until(partial, "no partial native checkpoint transfer observed", seconds=90)
    cluster.kill(lagging)
    cluster.start(lagging)
    catch_up(cluster, lagging)
    expected_digest = verify_replica(cluster, lagging)
    print(f"PASS interrupted native snapshot after {interrupted_bytes} bytes; resumed >64MiB checkpoint has every expected row ({expected_digest})", flush=True)

    for index, point in enumerate(("snapshot.staged", "snapshot.authority", "snapshot.published")):
        # Create a newer compacted prefix without rewriting the large dataset.
        cluster.kill(lagging)
        cluster.write(f"a-snapshot-marker-{index}", point)
        target, image_bytes = checkpoint(cluster)
        start_crashing(cluster, lagging, point)
        process = cluster.processes[lagging]
        until(lambda: process.poll() is not None, f"process did not abort at {point}", seconds=120)
        assert process.returncode == -signal.SIGABRT, (point, process.returncode)
        marker = f"raft-host group 1 crash point {point}"
        assert marker in (cluster.root / f"node{lagging}.log").read_text(errors="replace"), point
        cluster.processes.pop(lagging)
        cluster.logs.pop(lagging).close()
        cluster.start(lagging)
        catch_up(cluster, lagging)
        assert verify_replica(cluster, lagging) == expected_digest, point
        result = good(cluster.clients[lagging].call({"kind": "test_local_scan", "group": 1,
            "table": "alpha", "start": byte(f"a-snapshot-marker-{index}"),
            "end": byte(f"a-snapshot-marker-{index}\x00"), "limit": 1}), "rows")
        assert result["rows"] == [[byte(f"a-snapshot-marker-{index}"), byte(point)]], (point, result)
        print(f"PASS real RocksDB generation recovery after SIGABRT at {point}, checkpoint {target}, full dataset digest {expected_digest}", flush=True)

    for node in list(cluster.processes):
        cluster.kill(node)
    for node in sorted(cluster.configs):
        cluster.start(node)
    cluster.leader(1)
    for node in sorted(cluster.processes):
        catch_up(cluster, node)
        assert verify_replica(cluster, node) == expected_digest, node
    print("PASS all replica user rows agree after full-cluster restart of compacted native RocksDB generations", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--data-dir", type=Path)
    args = parser.parse_args()
    temporary = tempfile.TemporaryDirectory(prefix="rocksdb-raft-snapshots-") if args.data_dir is None else None
    root = Path(temporary.name) if temporary else args.data_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        raise ValueError("--data-dir must be empty")
    cluster = Cluster.__new__(Cluster)
    try:
        cluster.__init__(args.bin_dir.resolve(), root)
        scenarios(cluster)
        print(json.dumps({"suite": "rocksdb_raft_native_snapshots", "status": "passed"}))
    except BaseException:
        for log in root.glob("node*.log"):
            print(f"--- {log} ---\n{log.read_text(errors='replace')[-20000:]}", flush=True)
        raise
    finally:
        cluster.close()
        if temporary:
            temporary.cleanup()


if __name__ == "__main__":
    main()
