#!/usr/bin/env python3
"""Run paired fresh-process engine and ACK diagnostics on zoo-002.

The archive fingerprint and source snapshot belong beside these raw results.
This launcher records exact executable hashes and keeps every attempted cell.
It does not measure sustained RocksDB throughput or reopen time.
"""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def machine():
    root = Path("/sys/devices/system/cpu")
    return {
        "hostname": socket.gethostname(),
        "boost": (root / "cpufreq/boost").read_text().strip(),
        "governors": {
            str(cpu): (root / f"cpu{cpu}/cpufreq/scaling_governor").read_text().strip()
            for cpu in range(64)
        },
        "load_average": Path("/proc/loadavg").read_text().strip(),
        "processes": subprocess.check_output(
            ["ps", "-eo", "user,pid,pcpu,comm", "--sort=-pcpu"], text=True
        ).splitlines()[:25],
    }


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--c-abi", type=Path, required=True)
    parser.add_argument("--sto", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", type=int, nargs="+", default=[1, 4])
    parser.add_argument("--modes", nargs="+", default=["engine", "engine-logged", "cache-ack"],
                        choices=["engine", "engine-logged", "cache-ack"])
    parser.add_argument("--iterations", type=int, default=262144)
    parser.add_argument("--warmup", type=int, default=65536)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--perf", type=int, choices=[0, 1], default=1)
    args = parser.parse_args()
    if socket.gethostname() != "zoo-002":
        parser.error("this CPU-placement protocol is specific to zoo-002")
    if (not args.output.is_dir() or any(args.output.iterdir())
            or any(worker < 1 or worker > 32 for worker in args.workers)
            or args.iterations < 1 or args.warmup < 256 or args.repetitions < 1):
        parser.error("output must be an empty directory; workers must be in 1..32, iterations and repetitions positive, warmup at least 256")
    binaries = {"c-abi": args.c_abi.resolve(strict=True), "sto": args.sto.resolve(strict=True)}
    with open("/var/tmp/mako-zoo2-perf-shuai.lock", "r") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        baseline = machine()
        if baseline["boost"] != "0":
            parser.error("CPU boost must already be disabled")
        write_json(args.output / "protocol.json", {
            "binary_sha256": {arm: digest(path) for arm, path in binaries.items()},
            "binary_paths": {arm: str(path) for arm, path in binaries.items()},
            "source_policy": "one common benchmark source; engine selected by Cargo feature",
            "workers": args.workers, "modes": args.modes,
            "iterations_per_worker": args.iterations, "warmup_per_worker": args.warmup,
            "repetitions": args.repetitions, "helper_affinity": "33-63",
            "worker_affinity": "0..workers-1", "writeback_cpu": 32,
            "LD_LIBRARY_PATH": os.environ.get("LD_LIBRARY_PATH"),
            "LD_PRELOAD": os.environ.get("LD_PRELOAD"), "initial_machine": baseline,
            "performance_acceptance": False,
            "scope": "foreground diagnostics; unapplied ACK tail discarded after value verification",
        })
        results = []
        for repetition in range(args.repetitions):
            arms = ["c-abi", "sto"] if repetition % 2 == 0 else ["sto", "c-abi"]
            for mode in args.modes:
                for workers in args.workers:
                    for arm in arms:
                        sample = Path(tempfile.mkdtemp(
                            prefix=f"r{repetition + 1}-{mode}-w{workers}-{arm}.", dir=args.output))
                        before = machine()
                        if before["boost"] != "0" or before["governors"] != baseline["governors"]:
                            raise RuntimeError("boost/governor changed before the next sample")
                        command = ["taskset", "-c", "33-63", str(binaries[arm]), mode,
                                   "--workers", str(workers), "--iterations", str(args.iterations),
                                   "--warmup", str(args.warmup), "--perf", str(args.perf)]
                        database = sample / "rocks"
                        if mode == "cache-ack":
                            command += ["--path", str(database)]
                        write_json(sample / "before.json", before)
                        started = time.time()
                        with (sample / "stdout.txt").open("w") as stdout, (sample / "stderr.txt").open("w") as stderr:
                            completed = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=600)
                        after = machine()
                        write_json(sample / "after.json", after)
                        entry = {"arm": arm, "mode": mode, "workers": workers,
                                 "repetition": repetition + 1, "directory": str(sample),
                                 "returncode": completed.returncode, "command": command,
                                 "elapsed_seconds": time.time() - started}
                        if completed.returncode != 0:
                            write_json(sample / "result.json", entry)
                            raise RuntimeError(f"sample failed, inspect {sample}")
                        records = [json.loads(line) for line in (sample / "stdout.txt").read_text().splitlines()
                                   if line.startswith("{")]
                        if len(records) != 1:
                            raise RuntimeError(f"expected one result record in {sample}")
                        entry["measurement"] = records[0]
                        valid = (after["boost"] == "0" and after["governors"] == baseline["governors"]
                                 and records[0]["workers"] == workers and records[0]["mode"] == mode
                                 and records[0]["commits"] == args.iterations * workers
                                 and records[0]["verified_keys"] == workers * 256
                                 and records[0]["engine"] == ("sto" if arm == "sto" else "mako-local/sto-masstrans"))
                        entry["environment_and_counts_valid"] = valid
                        entry["pmu_valid"] = (not args.perf or records[0]["pmu_min_running_ratio"] >= 0.995)
                        if valid and entry["pmu_valid"] and database.is_dir():
                            # This exact scratch database was created above by
                            # the successful value-checking ACK subprocess.
                            shutil.rmtree(database)
                        write_json(sample / "result.json", entry)
                        results.append(entry)
                        write_json(args.output / "results.json", results)
                        print(json.dumps(entry), flush=True)
                        if not valid:
                            raise RuntimeError(f"sample environment/count verification failed: {sample}")
                        if not entry["pmu_valid"]:
                            raise RuntimeError(f"sample PMU running ratio is below 0.995: {sample}")


if __name__ == "__main__":
    main()
