#!/usr/bin/env python3
"""Matched fixed-dataset native OCC/STO/one- and three-member service measurements.

Run inside Docker. Each case gets fresh DBs, untimed preload, a shared start gate,
retained logs/configs, and a JSON record. No transaction-body retries.
"""
import argparse
import collections
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import time

from smoke import Cluster


IO_FIELDS = ("rchar", "wchar", "syscr", "syscw", "read_bytes", "write_bytes", "cancelled_write_bytes")


def result(stdout):
    for line in reversed(stdout.splitlines()):
        try:
            value = json.loads(line)
            if isinstance(value, dict) and "transactions_per_second" in value:
                return value
        except ValueError:
            pass
    raise RuntimeError("benchmark produced no metrics record")


def selection(value, allowed, convert=str):
    values = [convert(part) for part in value.split(",")]
    if not values or len(set(values)) != len(values) or any(v not in allowed for v in values):
        raise argparse.ArgumentTypeError(f"select distinct comma-separated values from {allowed}")
    return values


def stat_values(contents):
    fields = contents.rsplit(")", 1)[1].split()
    ticks = os.sysconf("SC_CLK_TCK")
    return {"user_cpu_seconds": int(fields[11]) / ticks, "system_cpu_seconds": int(fields[12]) / ticks,
            "rss_bytes": int(fields[21]) * os.sysconf("SC_PAGE_SIZE"), "threads": int(fields[17])}


def process_sample(pid, read_io=True):
    root = Path("/proc") / str(pid)
    sample = stat_values((root / "stat").read_text())
    # Clients report authoritative before/after IO themselves. Reading their
    # /proc/PID/io during exit can return EACCES before waitpid reaps the zombie.
    # Keep sampling public RSS/thread counters; live server IO remains required.
    sample["io"] = ({key: int(value) for key, value in
                     (line.split(":", 1) for line in (root / "io").read_text().splitlines())}
                    if read_io else {})
    return sample


def storage(paths):
    """Actual apparent and allocated file sizes, excluding harness logs/configs."""
    apparent = allocated = files = 0
    for path in paths:
        for file in path.rglob("*"):
            try:
                if file.is_file():
                    info = file.stat()
                    apparent += info.st_size
                    allocated += info.st_blocks * 512
                    files += 1
            except FileNotFoundError:  # a compaction can unlink between listing/stat
                continue
    return {"apparent_bytes": apparent, "allocated_bytes": allocated, "files": files}


def resource_delta(before, after, peak, seconds):
    user = after["user_cpu_seconds"] - before["user_cpu_seconds"]
    system = after["system_cpu_seconds"] - before["system_cpu_seconds"]
    return {"user_cpu_seconds": user, "system_cpu_seconds": system,
            "cpu_percent_one_core": 100 * (user + system) / seconds,
            "rss_before_bytes": before["rss_bytes"], "rss_last_bytes": after["rss_bytes"],
            "sampled_peak_rss_bytes": peak["rss_bytes"], "sampled_peak_os_threads": peak["threads"],
            "io_delta": {key: after["io"].get(key, 0) - before["io"].get(key, 0) for key in IO_FIELDS}}


def merged_histogram(histograms):
    bins = collections.Counter()
    samples = sum(h["samples"] for h in histograms)
    maximum = max((h["max"] for h in histograms), default=0)
    for histogram in histograms:
        for index, count in histogram["sparse_bins"]:
            bins[index] += count
    percentiles = {}
    for percentile in (50, 95, 99):
        target = (samples * percentile + 99) // 100
        accumulated = 0
        upper = 0
        for index, count in sorted(bins.items()):
            accumulated += count
            if accumulated >= target:
                exponent, subdivision = divmod(index, 64)
                upper = min(maximum, ((1 << exponent) + (subdivision + 1) * (1 << max(0, exponent - 6)) - 1) / 1000)
                break
        percentiles[f"p{percentile}"] = upper
    return {"samples": samples, **percentiles, "max": maximum,
            "mean": sum(h["mean"] * h["samples"] for h in histograms) / samples if samples else 0,
            "sparse_bins": sorted(bins.items())}


def machine(root):
    cpuinfo = Path("/proc/cpuinfo").read_text()
    models = sorted(set(line.split(":", 1)[1].strip() for line in cpuinfo.splitlines() if line.startswith("model name")))
    cgroup = {}
    for name in ("cpu.max", "cpuset.cpus.effective", "memory.max", "io.max"):
        path = Path("/sys/fs/cgroup") / name
        if path.exists():
            cgroup[name] = path.read_text().strip()
    return {"platform": platform.platform(), "cpu_models": models, "logical_cpus": os.cpu_count(),
            "cpu_affinity": sorted(os.sched_getaffinity(0)), "cgroup": cgroup,
            "memory": Path("/proc/meminfo").read_text(), "mountinfo": Path("/proc/self/mountinfo").read_text(),
            "data_device": os.stat(root).st_dev, "data_root": str(root),
            "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"), "page_bytes": os.sysconf("SC_PAGE_SIZE")}


def run_case(args, root, mode, replicas, shards, threads, workload, case_number):
    name = f"{case_number:03d}-{mode}-r{replicas}-s{shards}-t{threads}-{workload}"
    case = root / name
    case.mkdir()
    cluster = None
    processes = []
    handles = []
    commands = []
    preload_commands = []
    server_configs = []
    storage_paths = []
    # A local DB cannot have two writer processes. One shard uses a single
    # aggregate-concurrency process; two shards use two independent DB owners.
    client_count = 2 if mode == "replicated" or shards == 2 else 1
    client_threads = threads if client_count == 2 else threads * 2
    workers_per_shard = threads * 2 // shards
    binary = args.bin_dir.resolve() / "rocksdb_raft_bench"
    client_seed = (time.time_ns() & ((1 << 62) - 1)) + case_number * 100
    try:
        if mode == "replicated":
            cluster_root = case / "cluster"
            cluster_root.mkdir()
            cluster = Cluster.__new__(Cluster)
            cluster.__init__(args.bin_dir.resolve(), cluster_root, replicas=replicas,
                             shards=shards, server_workers=args.server_workers, short_timeouts=False,
                             durability=args.durability)
            server_configs = [json.loads(path.read_text()) for path in cluster.configs.values()]
            storage_paths = [cluster_root / f"node{node}" for node in range(1, replicas + 1)]
            # Stop idle RPC helper processes outside the measurement window.
            for helper in cluster.clients.values():
                helper.close()
            cluster.clients.clear()
        else:
            storage_paths = [case / f"db{group}" for group in range(1, shards + 1)]

        def command(group, process_threads, offset, client_id, phase):
            values = [str(binary), "--mode", mode, "--phase", phase, "--workload", workload,
                      "--threads", str(process_threads), "--seconds", str(args.seconds),
                      "--client-id", str(client_id), "--group", str(group), "--table", args.table,
                      "--prefix", "a/" if group == 1 else "z/", "--value-bytes", str(args.value_bytes),
                      "--operations", str(args.operations), "--hot-keys", str(args.hot_keys),
                      "--dataset-keys", str(args.dataset_keys), "--scan-limit", str(args.scan_limit),
                      "--total-workers", str(workers_per_shard), "--worker-offset", str(offset),
                      "--durability", args.durability]
            if mode == "replicated":
                values += ["--config", str(cluster.client_config)]
            else:
                values += ["--path", str(case / f"db{group}")]
            return values

        # Each shard has exactly the same number and shape of records. Preload
        # once per shard, not once per client; no preload conflicts or retries.
        for group in range(1, shards + 1):
            argv = command(group, 1, 0, client_seed + group, "preload")
            preload_commands.append(argv)
            with (case / f"preload{group}.stdout").open("w") as out, (case / f"preload{group}.stderr").open("w") as err:
                subprocess.run(argv, stdout=out, stderr=err, timeout=args.preload_timeout, check=True)
        start_file = case / "start"
        ready_files = []
        for client in range(client_count):
            group = client + 1 if shards == 2 else 1
            offset = client * threads if shards == 1 and client_count == 2 else 0
            argv = command(group, client_threads, offset, client_seed + 10 + client, "measure")
            ready_file = case / f"ready{client}"
            argv += ["--ready-file", str(ready_file), "--start-file", str(start_file)]
            ready_files.append(ready_file)
            commands.append(argv)
            out = (case / f"client{client}.stdout").open("w")
            err = (case / f"client{client}.stderr").open("w")
            handles.extend((out, err))
            processes.append(subprocess.Popen(argv, stdout=out, stderr=err, text=True))
        deadline = time.monotonic() + args.ready_timeout
        while not all(path.exists() for path in ready_files):
            if any(p.poll() is not None for p in processes):
                raise RuntimeError(f"client exited before start gate; inspect {case}")
            if cluster and any(p.poll() is not None for p in cluster.processes.values()):
                raise RuntimeError(f"server exited before start gate; inspect {case}")
            if time.monotonic() >= deadline:
                raise TimeoutError(f"clients did not become ready; inspect {case}")
            time.sleep(.02)
        all_processes = [("client", index, p) for index, p in enumerate(processes)]
        if cluster:
            all_processes += [("server", node, p) for node, p in cluster.processes.items()]
        before = {p.pid: process_sample(p.pid, read_io=role == "server") for role, _, p in all_processes}
        last = dict(before)
        peak = {pid: dict(sample) for pid, sample in before.items()}
        storage_before = storage(storage_paths)
        started = time.monotonic()
        start_file.touch()
        deadline = started + args.seconds + args.finish_timeout
        while any(p.poll() is None for p in processes):
            for role, _, process in all_processes:
                if role == "server" and process.poll() is not None:
                    raise RuntimeError(f"server exited during measurement; inspect {case}")
                try:
                    sample = process_sample(process.pid, read_io=role == "server")
                except (FileNotFoundError, ProcessLookupError):
                    if role == "server":
                        raise
                    continue
                last[process.pid] = sample
                for field in ("rss_bytes", "threads"):
                    peak[process.pid][field] = max(peak[process.pid][field], sample[field])
            if time.monotonic() >= deadline:
                raise TimeoutError(f"benchmark did not finish; inspect {case}")
            time.sleep(args.sample_ms / 1000)
        elapsed = time.monotonic() - started
        storage_after = storage(storage_paths)
        for role, _, process in all_processes:
            if role == "server":
                if process.poll() is not None:
                    raise RuntimeError(f"server exited at measurement end; inspect {case}")
                last[process.pid] = process_sample(process.pid)
        for handle in handles:
            handle.close()
        handles.clear()
        results = [result((case / f"client{client}.stdout").read_text()) for client in range(client_count)]
        if any(measured["durability"] != args.durability for measured in results):
            raise RuntimeError("benchmark durability does not match the requested comparison")
        resources = []
        for role, index, process in all_processes:
            seconds = elapsed
            if role == "client":
                measured = results[index]
                if not measured["process_stat_before"] or not measured["process_stat_after"]:
                    raise RuntimeError("Linux /proc process counters unavailable")
                before[process.pid] = {**stat_values(measured["process_stat_before"]), "io": measured["process_io_before"]}
                last[process.pid] = {**stat_values(measured["process_stat_after"]), "io": measured["process_io_after"]}
                seconds = measured["seconds"]
            resources.append({"role": role, "index": index, "pid": process.pid,
                              **resource_delta(before[process.pid], last[process.pid], peak[process.pid], seconds)})
        counters = {key: sum(r[key] for r in results) for key in
                    ("attempted", "committed", "conflicts", "busy", "conflicts_or_busy", "unknown", "errors", "logical_write_bytes")}
        # Common start gate; max client elapsed includes draining its last attempt.
        measured_seconds = max(r["seconds"] for r in results)
        write_bytes = sum(r["io_delta"]["write_bytes"] for r in resources)
        storage_growth = {key: storage_after[key] - storage_before[key] for key in storage_before}
        actual_options = []
        for path in storage_paths:
            for options_file in sorted(path.rglob("OPTIONS-*")):
                actual_options.append({"path": str(options_file), "contents": options_file.read_text()})
        phases = results[0]["latency_us"]
        record = {
            "schema_version": 3, "case": name, "mode": mode, "workload": workload,
            "durability": args.durability, "power_crash_durable": args.durability == "sync",
            "replicas_per_shard": replicas, "active_shards": shards,
            "client_processes": client_count, "client_threads_per_process": client_threads,
            "requested_client_shape": {"processes": 2, "threads_per_process": threads},
            "aggregate_client_workers": 2 * threads, "client_workers_per_shard": workers_per_shard,
            "replica_processes": replicas if mode == "replicated" else 0,
            "server_workers_per_group_per_replica": args.server_workers if cluster else 0,
            "server_transaction_workers_total": replicas * shards * args.server_workers if cluster else 0,
            "server_admin_workers_per_replica": 4 if cluster else 0,
            "local_apply_threads": shards if mode == "sto" else 0,
            "measured_seconds": measured_seconds, "monitor_window_seconds": elapsed,
            "aggregate_transactions_per_second": counters["committed"] / measured_seconds,
            **counters,
            "rates_per_second": {key: counters[key] / measured_seconds for key in ("committed", "conflicts", "busy", "unknown", "errors")},
            "fractions_of_attempts": {key: counters[key] / counters["attempted"] if counters["attempted"] else 0
                                     for key in ("committed", "conflicts", "busy", "unknown", "errors")},
            "latency_us": {phase: merged_histogram([r["latency_us"][phase] for r in results]) for phase in phases},
            "histogram": results[0]["histogram"], "clients": results, "resources": resources,
            "read_only": workload in ("point", "scan"),
            "resource_sampling_ms": args.sample_ms,
            "storage": {"paths": [str(p) for p in storage_paths], "before": storage_before, "after": storage_after, "growth": storage_growth},
            "write_amplification": {"process_write_bytes": write_bytes, "logical_committed_key_value_bytes": counters["logical_write_bytes"],
                "physical_to_logical": write_bytes / counters["logical_write_bytes"] if counters["logical_write_bytes"] else None,
                "storage_growth_to_logical": storage_growth["allocated_bytes"] / counters["logical_write_bytes"] if counters["logical_write_bytes"] else None},
            "dataset": {"keys_per_shard": args.dataset_keys, "value_bytes": args.value_bytes, "key_bytes": 10,
                        "preload_outside_timing": True, "hot_keys_per_shard": args.hot_keys, "multi_key_width": args.operations,
                        "scan_rows": args.scan_limit, "prefixes": ["a/", "z/"][:shards]},
            "rocksdb_options": {"durability": "sync WAL" if args.durability == "sync" else "async WAL (sync=false)",
                "unordered_write": False,
                "wal_recovery": "absolute consistency", "cache_write_buffer_compaction": "linked RocksDB defaults; actual OPTIONS files below",
                "native_optimistic_snapshot_at_begin": True, "native_point_reads": "GetForUpdate",
                "mako_silo_value_cache": False, "actual_options_files": actual_options},
            "server_configs": server_configs, "commands": commands, "preload_commands": preload_commands,
            "storage_layout": "shared sharded service" if mode == "replicated" else
                ("one local DB owner process with aggregate workers (not two client processes)" if shards == 1 else "two independent local DB owner processes, one per shard"),
            "semantics": {"native": "native RocksDB OptimisticTransactionDB; tracked snapshot reads, optimistic atomic commit; no replication",
                          "sto": "local retained STO certification and ordered RocksDB application; no Raft",
                          "replicated": "SRPC + STO + Raft + ordered application; one replica is not fault tolerant"}[mode],
            "durability_contract": ("WAL-enabled synchronous persistence before successful commit" if args.durability == "sync" else
                "WAL-enabled writes without foreground sync; OS/RocksDB background persistence; acknowledged writes may be lost after power/machine failure"),
            "caveats": [results[0]["phase_caveat"], results[0]["scan_caveat"],
                "No internal CPU attribution: CPU is measured per process; operation/commit/begin splits are client wall time, not isolated RocksDB/STO/quorum CPU.",
                "Write amplification is /proc process physical write_bytes divided by committed logical key+value bytes; includes all replicas, Raft, metadata, WAL, compaction and server logs. Not device-level or steady-state engine-only amplification.",
                "RSS peaks are sampled; CPU and IO use cumulative counters. Server window includes start-gate skew and client drain; background work after the window is excluded.",
                "Storage growth is live file allocation difference and can be negative during compaction. Read-only amplification has no write denominator.",
                "Local preload and measurement use separate DB opens; service stays open. Cache residency is not normalized. Local post-window storage sizes can include DB-close flushing, excluded from timed CPU/IO.",
                "All replicas and clients share this machine/container; no imposed WAN latency, dedicated per-process CPU partition, or cache coldness guarantee.",
                "Each case uses fresh DBs with fixed preloaded rows; persistent STO outcomes/metadata grow with successful writes. Native has no matching service metadata.",
                "Disjoint/multikey reads verify each worker's committed per-key counters; point/scan reads verify zero counters, padding and exact scan keys/order. Hot reads verify value shape and perform actual tracked read-modify-write; no body retries.",
                "Single-shard local modes aggregate both client thread pools into one DB-owning process; two-shard local modes use two independent DBs. Process-layout difference is explicit.",
                "All transactions stay within one shard; this is a synthetic workload, not unchanged cross-shard TPC-C or existing Mako+Raft."],
            "service_deadlines": "production defaults; fault-fixture timeout overrides are disabled",
        }
        failures = [p.returncode for p in processes if p.returncode != 0]
        record["status"] = "failed" if failures or counters["unknown"] or counters["errors"] or not counters["committed"] else "passed"
        (case / "result.json").write_text(json.dumps(record, indent=2) + "\n")
        if record["status"] != "passed":
            raise RuntimeError(f"benchmark failed (exit codes {failures}); evidence retained in {case}")
        return record
    except BaseException as error:
        (case / "failure.json").write_text(json.dumps({"case": name, "error": repr(error), "commands": commands,
                                                      "preload_commands": preload_commands}, indent=2) + "\n")
        raise
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait()
        for handle in handles:
            handle.close()
        if cluster:
            cluster.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", required=True, type=Path)
    parser.add_argument("--data-dir", type=Path, help="empty evidence/data directory; otherwise create and retain a temporary directory")
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--durability", choices=("sync", "async"), default="sync",
                        help="matched application/Raft/native WAL policy; async does not guarantee power-loss durability")
    parser.add_argument("--replicas", type=lambda s: selection(s, (1, 3), int), default=[1, 3])
    parser.add_argument("--shards", type=lambda s: selection(s, (1, 2), int), default=[1, 2])
    parser.add_argument("--threads", type=lambda s: selection(s, (8, 16), int), default=[8, 16], help="workers per requested client process")
    parser.add_argument("--server-workers", type=int, default=40)
    parser.add_argument("--workloads", "--workload", type=lambda s: selection(s, ("point", "disjoint", "hot", "multikey", "scan")),
                        default=["point", "disjoint", "hot", "multikey", "scan"])
    parser.add_argument("--modes", type=lambda s: selection(s, ("native", "sto", "replicated")), default=["native", "sto", "replicated"])
    parser.add_argument("--table", default="bench", choices=("alpha", "beta", "bench"))
    parser.add_argument("--value-bytes", type=int, default=100)
    parser.add_argument("--operations", type=int, default=4, help="keys per multikey read-modify-write transaction")
    parser.add_argument("--hot-keys", type=int, default=8)
    parser.add_argument("--dataset-keys", type=int, default=4096)
    parser.add_argument("--scan-limit", type=int, default=16)
    parser.add_argument("--sample-ms", type=int, default=50)
    parser.add_argument("--preload-timeout", type=int, default=600)
    parser.add_argument("--ready-timeout", type=int, default=90)
    parser.add_argument("--finish-timeout", type=int, default=120)
    parser.add_argument("--output", type=Path, help="aggregate JSON file, updated after each successful case")
    args = parser.parse_args()
    if not 1 <= args.seconds <= 86400 or not 1 <= args.server_workers <= 1024 or not 1 <= args.sample_ms <= 1000:
        parser.error("invalid seconds, server-workers or sample-ms")
    if not 8 <= args.value_bytes <= 262144 or not 1 <= args.operations <= 128 or not 1 <= args.dataset_keys <= 1_000_000:
        parser.error("invalid dataset/value/operation bounds")
    if not 1 <= args.hot_keys <= args.dataset_keys or not 1 <= args.scan_limit <= min(128, args.dataset_keys):
        parser.error("invalid hot key or scan bounds")
    if args.dataset_keys // (2 * max(args.threads) // min(args.shards)) < args.operations:
        parser.error("dataset must have at least --operations keys per worker")
    if min(args.preload_timeout, args.ready_timeout, args.finish_timeout) <= 0 or args.ready_timeout > 110:
        parser.error("timeouts must be positive and ready-timeout <=110 (benchmark gate timeout is 120s)")
    root = args.data_dir.resolve() if args.data_dir else Path(tempfile.mkdtemp(prefix="rocksdb-raft-performance-"))
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        parser.error("--data-dir must be empty; never overwrite an existing measurement")
    if args.output:
        args.output = args.output.resolve()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if args.output.exists():
            parser.error("--output must not already exist")
    metadata = {"schema_version": 3, "machine": machine(root), "selection": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()}}
    (root / "machine.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps({"evidence_root": str(root), "selection": metadata["selection"]}), flush=True)
    records = []
    with (root / "results.jsonl").open("w") as stream:
        for mode in args.modes:
            for replicas in (args.replicas if mode == "replicated" else [0]):
                for shards in args.shards:
                    for threads in args.threads:
                        for workload in args.workloads:
                            record = run_case(args, root, mode, replicas, shards, threads, workload, len(records))
                            records.append(record)
                            encoded = json.dumps(record)
                            stream.write(encoded + "\n")
                            stream.flush()
                            print(encoded, flush=True)
                            if args.output:
                                args.output.write_text(json.dumps({**metadata, "records": records}, indent=2) + "\n")
    (root / "results.json").write_text(json.dumps({**metadata, "records": records}, indent=2) + "\n")


if __name__ == "__main__":
    main()
