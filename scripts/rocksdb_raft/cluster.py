#!/usr/bin/env python3
"""Generate or run a selected one/three-replica, one/two-shard cluster."""
import argparse
import json
from pathlib import Path
import time
from smoke import Cluster, create_tables


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", required=True, type=Path)
    parser.add_argument("--data-dir", required=True, type=Path)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--replicas", type=int, choices=(1, 3), default=3)
    parser.add_argument("--shards", type=int, choices=(1, 2), default=2)
    parser.add_argument("--server-workers", type=int, default=40, help="transaction workers per group per replica")
    parser.add_argument("--durability", choices=("sync", "async"), default="sync",
                        help="async is an unsafe persistence experiment: acknowledged writes can be lost on power/machine failure")
    args = parser.parse_args()
    root = args.data_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        raise ValueError("data directory must be empty; restart existing servers directly without --bootstrap")
    cluster = Cluster.__new__(Cluster)
    try:
        cluster.__init__(args.bin_dir.resolve(), root, start=not args.generate_only,
                         replicas=args.replicas, shards=args.shards, server_workers=args.server_workers,
                         short_timeouts=False, durability=args.durability)
        print(json.dumps({"configs": [str(p) for p in cluster.configs.values()],
                          "client_config": str(cluster.client_config),
                          "endpoints": [cluster.endpoint(n) for n in range(1, args.replicas + 1)],
                          "replica_processes": args.replicas, "active_shards": args.shards,
                          "server_workers_per_group_per_replica": args.server_workers,
                          "server_workers_total": args.replicas * args.shards * args.server_workers,
                          "durability": args.durability, "power_crash_durable": args.durability == "sync",
                          "restart": "rocksdb_raft_server --config NODE.json (no --bootstrap)",
                          "bootstrap": "rocksdb_raft_server --config NODE.json --bootstrap"}), flush=True)
        if not args.generate_only:
            create_tables(cluster)
            print(json.dumps({"ready": True, "client_config": str(cluster.client_config)}), flush=True)
            while True:
                time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        cluster.close()


if __name__ == "__main__":
    main()
