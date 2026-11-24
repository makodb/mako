#!/usr/bin/env python3
"""Run a small native transaction/workload topology and join only its children."""
import argparse
import json
import os
from pathlib import Path
import queue
import random
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def reserve():
    # This band is reserved from ephemeral allocation by the build containers.
    # Also retain sockets until each owning process starts.
    for _ in range(1000):
        listener = socket.socket()
        try:
            listener.bind(("127.0.0.1", random.SystemRandom().randrange(40000, 64000)))
            return listener
        except OSError:
            listener.close()
    raise RuntimeError("cannot reserve a local test port")


def tables(workload):
    if workload == "transactions":
        return [dict(id=1, name="records", kind="governed", warehouse=True, initial_owner=0)]
    names = ["customer", "customer_name_idx", "district", "history", "new_order",
             "oorder", "oorder_c_id_idx", "order_line", "stock", "stock_data",
             "warehouse", "item", "tpcc_load_state"]
    return [dict(id=i + 1, name=name, kind="governed" if i < 11 else "replicated",
                 warehouse=i < 11, initial_owner=0) for i, name in enumerate(names)]


def stop(processes):
    for child in processes:
        if child.stdin:
            child.stdin.close()
            child.stdin = None
    deadline = time.monotonic() + 15
    for child in processes:
        try:
            child.wait(timeout=max(0.01, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGTERM)
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workload", choices=["transactions", "tpcc", "micro"])
    parser.add_argument("--shards", type=int, default=2)
    parser.add_argument("--replicas", type=int, choices=[1, 3], default=1)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--colocated", action="store_true")
    parser.add_argument("--persistent", action="store_true", help="local RocksDB persistence without Raft")
    parser.add_argument("--count", type=int, default=100, help="commits per transaction worker, or attempts per benchmark worker")
    parser.add_argument("--seconds", type=int, default=10, help="benchmark duration limit")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--build", type=Path, default=Path(os.environ.get("BUILD_DIR", "build")))
    parser.add_argument("--data-dir", type=Path, help="retain data/config/logs here; reopen unless --create is passed")
    parser.add_argument("--create", action="store_true", help="allow creating databases in --data-dir")
    parser.add_argument("--keep", action="store_true", help="retain the automatically created test directory")
    args = parser.parse_args()
    if not (1 <= args.shards <= 8 and 1 <= args.workers <= 64 and args.count > 0
            and 1 <= args.seconds <= 86400 and 1 <= args.timeout <= 86400):
        parser.error("invalid topology, count or duration")
    if args.persistent and args.replicas != 1:
        parser.error("choose local persistence or replicated persistence")
    binary = args.build / ("mako_transactions" if args.workload == "transactions" else "mako_bench")
    binary = (ROOT / binary).resolve()
    if not binary.is_file():
        parser.error(f"build the native executable first: {binary}")
    if args.data_dir:
        directory = args.data_dir.resolve()
    else:
        runs = binary.parent / "runs"
        runs.mkdir(exist_ok=True)
        directory = Path(tempfile.mkdtemp(prefix="mako-native-", dir=runs))
    directory.mkdir(parents=True, exist_ok=True)
    create = args.create or args.data_dir is None
    reservations = {}
    processes = []
    logs = []
    threads = []
    completed = False
    events = queue.Queue()
    try:
        for owner in range(args.shards):
            for replica in range(args.replicas):
                for kind in ["app"] + (["raft"] if args.replicas > 1 else []):
                    reservations[owner, replica, kind] = reserve()
        addresses = {key: f"127.0.0.1:{value.getsockname()[1]}" for key, value in reservations.items()}
        groups = {owner: dict(group=owner + 1, endpoints=[addresses[owner, replica, "app"] for replica in range(args.replicas)])
                  for owner in range(args.shards)} if args.replicas > 1 else {}
        batches = [(list(range(args.shards)), replica) for replica in range(args.replicas)] if args.colocated else [
            ([owner], replica) for owner in range(args.shards) for replica in range(args.replicas)]
        for index, (owners, replica) in enumerate(batches):
            nodes = []
            for owner in owners:
                node = dict(owner=owner, nodes=args.shards, listen=addresses[owner, replica, "app"],
                            workers=args.workers, warehouses_per_shard=1, tables=tables(args.workload),
                            peers={other: addresses[other, 0, "app"] for other in range(args.shards)}, replica_groups=groups)
                data = directory / f"shard-{owner}-replica-{replica}"
                if args.replicas > 1:
                    node["replication"] = dict(group=owner + 1, id=replica + 1,
                        listen=addresses[owner, replica, "raft"], directory=str(data), create=create,
                        peers=[dict(id=peer + 1, address=addresses[owner, peer, "raft"]) for peer in range(args.replicas)])
                elif args.persistent:
                    node["persistence"] = dict(directory=str(data), create=create)
                nodes.append(node)
            config = {"nodes": nodes}
            if args.workload == "transactions":
                config["transactions"] = dict(rounds_per_worker=args.count, timeout_seconds=args.timeout)
            else:
                config["benchmark"] = dict(scale=dict(districts=2, customers=20, items=50, seed=51),
                    seconds=args.seconds, transactions_per_worker=args.count, startup_timeout_seconds=args.timeout)
                if args.workload == "micro":
                    config["benchmark"]["micro"] = dict(keys=100, operations=4, write_percent=50, remote_percent=50)
            path = directory / f"config-{index}.json"
            path.write_text(json.dumps(config, indent=2) + "\n")
            for key in list(reservations):
                if key[0] in owners and key[1] == replica:
                    reservations.pop(key).close()
            log = open(directory / f"process-{index}.log", "w")
            logs.append(log)
            child = subprocess.Popen([str(binary), "--config", str(path), "--exit-on-stdin-close"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, start_new_session=True)
            processes.append(child)

            def collect(child=child, log=log, index=index):
                for line in child.stdout:
                    log.write(line)
                    log.flush()
                    events.put((index, line.rstrip()))

            reader = threading.Thread(target=collect)
            reader.start()
            threads.append(reader)
        expected = 1 if args.workload == "transactions" else args.shards
        results = {}
        deadline = time.monotonic() + args.timeout
        while len(results) < expected:
            if time.monotonic() >= deadline:
                raise RuntimeError("native topology completion timed out")
            for child in processes:
                if child.poll() is not None:
                    raise RuntimeError(f"native process {child.pid} exited early: {child.returncode}")
            try:
                index, line = events.get(timeout=0.1)
            except queue.Empty:
                continue
            if line.startswith(("MAKO_TRANSACTIONS_RESULT ", "MAKO_BENCH_RESULT ")):
                result = json.loads(line.split(" ", 1)[1])
                owner = result.get("owner", 0)
                if owner in results:
                    raise RuntimeError(f"duplicate completed workload for shard {owner}")
                count = result["committed"]
                if (sum(count) if isinstance(count, list) else count) == 0:
                    raise RuntimeError(f"shard {owner} committed no transactions")
                results[owner] = result
                print(json.dumps(result), flush=True)
        stop(processes)
        if any(child.returncode != 0 for child in processes):
            raise RuntimeError("native process shutdown failed")
        completed = True
        print(f"Native {args.workload}: {args.shards} shards, {args.replicas} replicas, all processes joined")
    finally:
        stop(processes)
        for reservation in reservations.values():
            reservation.close()
        for thread in threads:
            thread.join(timeout=5)
        for log in logs:
            log.close()
        if completed and not args.keep and args.data_dir is None:
            shutil.rmtree(directory)
        else:
            print(f"Native run files: {directory}", flush=True)
            if not completed:
                for path in sorted(directory.glob("process-*.log")):
                    print(f"{path.name}:\n" + "\n".join(path.read_text(errors="replace").splitlines()[-30:]), flush=True)


if __name__ == "__main__":
    main()
