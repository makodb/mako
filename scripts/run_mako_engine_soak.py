#!/usr/bin/env python3
"""Paired bounded-queue cache runs, complete drains, and fresh-process reopens.

Uses the same zoo-002 placement and lock as run_mako_engine_compare.py.
Successful scratch databases are removed; failed databases and every report
are retained. The caller chooses the duration; 120 seconds is not a GC soak.
"""

import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

from run_mako_engine_compare import cpu_list, digest, machine, parse_worker_cpus, write_json


def events(path):
    return [json.loads(line) for line in path.read_text().splitlines()
            if line.startswith('{"event":')]


def checked_machine(baseline):
    current = machine()
    if current["boost"] != "0" or current["governors"] != baseline["governors"]:
        raise RuntimeError("boost/governor changed during paired soak")
    return current


def validate_cell(protocol, drained, verified, arm, workers, seconds, worker_cpus=None, writeback_cpu=32):
    engine = "sto"
    worker_cpus = list(range(workers)) if worker_cpus is None else worker_cpus
    expected = {"version": 2, "engine": engine, "workers": workers,
                "seconds": seconds, "keys_per_worker": 256,
                "value_bytes": 128, "rate_per_worker": 0,
                "queue_capacity_per_worker": 1024, "max_batch_records": 64,
                "worker_cpus": worker_cpus, "writeback_cpu": writeback_cpu,
                "checksum": "crc32c", "wal": True, "sync": False}
    if (any(protocol.get(key) != value for key, value in expected.items())
            or not isinstance(protocol.get("worker_cpus"), list)
            or any(type(cpu) is not int for cpu in protocol["worker_cpus"])
            or type(protocol.get("writeback_cpu")) is not int
            or drained.get("engine") != engine or verified.get("engine") != engine
            or drained["commits"] != verified["commits"]
            or drained["commits"] <= 0 or verified["workers"] != workers
            or verified["keys"] != workers * 256
            or not math.isfinite(drained["ack_seconds"])
            or not math.isfinite(drained["applied_seconds"])
            or drained["applied_seconds"] < drained["ack_seconds"]
            or not math.isfinite(verified["open_seconds"])
            or verified["open_seconds"] < 0
            or drained["ack_seconds"] < seconds):
        raise RuntimeError("paired soak protocol/count mismatch")


def cell_events(directory):
    run, verify = events(directory / "run.jsonl"), events(directory / "verify.jsonl")
    records = [[event for event in source if event["event"] == name]
               for source, name in ((run, "protocol"), (run, "drained"), (verify, "verified"))]
    if any(len(group) != 1 for group in records):
        raise RuntimeError(f"missing or duplicate terminal events: {directory}")
    return tuple(group[0] for group in records)


def load_resume(output, protocol, planned):
    """Reuse only fully recorded, verified cells from the same experiment."""
    saved = json.loads((output / "protocol.json").read_text())
    stable = lambda record: {key: value for key, value in record.items() if key != "initial_machine"}
    if stable(saved) != stable(protocol):
        raise RuntimeError("resume binary/protocol/environment differs from the saved experiment")
    if (saved["initial_machine"]["boost"] != "0"
            or protocol["initial_machine"]["boost"] != "0"
            or saved["initial_machine"]["governors"] != protocol["initial_machine"]["governors"]):
        raise RuntimeError("resume boost/governor differs from the saved experiment")
    path = output / "results.json"
    results = json.loads(path.read_text()) if path.exists() else []
    if not isinstance(results, list) or len(results) > len(planned):
        raise RuntimeError("invalid saved result collection")
    for index, result in enumerate(results):
        key = result["repetition"], result["workers"], result["arm"]
        directory = Path(result["directory"]).resolve(strict=True)
        if (key != planned[index] or directory.parent != output
                or not directory.is_dir() or not (directory / "expected.txt").is_file()
                or result.get("drained_and_verified") is not True
                or result.get("scratch_cleaned") is not True
                or result["run"]["returncode"] != 0 or result["verify"]["returncode"] != 0
                or (directory / "rocks").exists()):
            raise RuntimeError(f"incomplete, reordered, or unexpected saved cell: {directory}")
        if json.loads((directory / "result.json").read_text()) != result:
            raise RuntimeError(f"saved aggregate differs from the per-cell record: {directory}")
        observed = cell_events(directory)
        if observed != (result["protocol"], result["drained"], result["verified"]):
            raise RuntimeError(f"saved cell differs from its raw terminal events: {directory}")
        validate_cell(*observed, result["arm"], result["workers"], protocol["seconds"],
                      protocol["worker_cpus"][:result["workers"]], protocol["writeback_cpu"])
        manifest = (directory / "expected.txt").read_text().split()
        header = ["gc-soak-v1", str(result["workers"]), "256", "128"]
        if (manifest[:4] != header or len(manifest) != 4 + result["workers"]
                or any(not count.isdecimal() or int(count) < 256 for count in manifest[4:])
                or sum(map(int, manifest[4:])) != result["drained"]["commits"]):
            raise RuntimeError(f"saved expected-state manifest mismatch: {directory}")
        for name in ("before.json", "after.json"):
            environment = json.loads((directory / name).read_text())
            if (environment["boost"] != "0"
                    or environment["governors"] != saved["initial_machine"]["governors"]):
                raise RuntimeError(f"saved cell environment mismatch: {directory}")
    return results


def execute(command, directory, phase, timeout, baseline):
    started = time.monotonic()
    samples = []
    attempt = {"command": command, "timeout_seconds": timeout, "completed": False}
    write_json(directory / f"{phase}-attempt.json", attempt)
    with (directory / f"{phase}.jsonl").open("w") as stdout, \
            (directory / f"{phase}.stderr").open("w") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        try:
            while process.poll() is None:
                elapsed = time.monotonic() - started
                if elapsed > timeout:
                    raise TimeoutError(f"{phase} exceeded {timeout} seconds")
                if not samples or elapsed - samples[-1]["elapsed_seconds"] >= 10:
                    try:
                        status = Path(f"/proc/{process.pid}/status").read_text()
                    except FileNotFoundError:
                        break
                    memory = {}
                    for line in status.splitlines():
                        if line.startswith(("VmRSS:", "VmHWM:", "VmSize:")):
                            name, value, unit = line.split()
                            assert unit == "kB"
                            memory[name.rstrip(":")] = int(value) * 1024
                    samples.append({"elapsed_seconds": elapsed, **memory})
                    write_json(directory / f"{phase}-memory.json", samples)
                    checked_machine(baseline)
                time.sleep(1)
            returncode = process.wait()
        except BaseException as error:
            attempt["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            attempt.update({"returncode": process.returncode,
                            "wall_seconds": time.monotonic() - started,
                            "completed": process.returncode == 0 and "error" not in attempt})
            write_json(directory / f"{phase}-attempt.json", attempt)
    if returncode:
        raise RuntimeError(f"{phase} exited {returncode}; inspect {directory}")
    return {"returncode": returncode, "wall_seconds": time.monotonic() - started,
            "command": command, "memory_samples": samples}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", type=int, nargs="+", default=[1, 4, 8, 16, 32])
    parser.add_argument("--worker-cpus", type=parse_worker_cpus,
                        help="ordered CPU pool; each cell uses its worker-count prefix")
    parser.add_argument("--helper-affinity", default="33-63", help="taskset CPU numbers/ranges")
    parser.add_argument("--writeback-cpu", type=int, default=32)
    parser.add_argument("--seconds", type=int, required=True)
    parser.add_argument("--repetitions", type=int, default=2)
    parser.add_argument("--run-timeout", type=int, default=1800)
    parser.add_argument("--reopen-timeout", type=int, default=3600)
    parser.add_argument("--resume", action="store_true",
                        help="skip only matching, fully recorded completed cells; retain incomplete attempts")
    args = parser.parse_args()
    args.output = args.output.resolve(strict=True)
    if socket.gethostname() != "zoo-002":
        parser.error("this placement protocol is specific to zoo-002")
    if not args.output.is_dir() or (not args.resume and any(args.output.iterdir())):
        parser.error("output must be an existing empty directory")
    if (args.seconds <= 0 or args.repetitions <= 0 or args.run_timeout <= args.seconds
            or args.reopen_timeout <= 0 or any(w < 1 or w > 32 for w in args.workers)
            or len(set(args.workers)) != len(args.workers)):
        parser.error("invalid duration, timeout, repetition, or worker setting")
    worker_cpus = list(range(max(args.workers))) if args.worker_cpus is None else args.worker_cpus
    if len(worker_cpus) < max(args.workers):
        parser.error("worker CPU pool is shorter than the largest worker count")
    try:
        helpers = cpu_list(args.helper_affinity)
        online = cpu_list(Path("/sys/devices/system/cpu/online").read_text().strip())
        allowed = os.sched_getaffinity(0) & online
        if not helpers or any(cpu >= 1024 for cpu in helpers) or not 0 <= args.writeback_cpu < 1024:
            parser.error("invalid helper/writeback CPU set")
        workers = set(worker_cpus)
        if workers & helpers or args.writeback_cpu in workers | helpers:
            parser.error("worker, helper and writeback CPU sets must be disjoint")
        required = workers | helpers | {args.writeback_cpu}
        if not required <= allowed:
            parser.error(f"requested CPUs unavailable: {sorted(required - allowed)}")
    except (OSError, ValueError) as error:
        parser.error(f"cannot establish CPU placement: {error}")
    binaries = {"baseline": args.baseline.resolve(strict=True), "candidate": args.candidate.resolve(strict=True)}
    with open("/var/tmp/mako-zoo2-perf-shuai.lock", "r") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        baseline = machine()
        checked_machine(baseline)
        protocol = {
            "schema": "mako-paired-engine-soak-v1", "seconds": args.seconds,
            "workers": args.workers, "repetitions": args.repetitions,
            "binary_sha256": {arm: digest(path) for arm, path in binaries.items()},
            "binary_paths": {arm: str(path) for arm, path in binaries.items()},
            "helper_affinity": args.helper_affinity, "worker_cpus": worker_cpus,
            "worker_affinity": "ordered worker_cpus prefix; individually pinned",
            "writeback_cpu": args.writeback_cpu, "initial_machine": baseline,
            "LD_LIBRARY_PATH": os.environ.get("LD_LIBRARY_PATH"),
            "LD_PRELOAD": os.environ.get("LD_PRELOAD"),
            "scope": "production bounded queues, complete drain, fresh-process verified reopen",
        }
        planned = [(repetition, workers, arm)
                   for repetition in range(1, args.repetitions + 1)
                   for workers in args.workers
                   for arm in (["baseline", "candidate"] if repetition % 2 else ["candidate", "baseline"])]
        results = load_resume(args.output, protocol, planned) if args.resume else []
        if args.resume:
            fd, attempt = tempfile.mkstemp(prefix="resume-", suffix=".json", dir=args.output)
            os.close(fd)
            write_json(Path(attempt), {"completed_cells_reused": len(results),
                                      "saved_protocol_sha256": digest(args.output / "protocol.json"),
                                      "launcher_sha256": digest(Path(__file__)),
                                      "run_timeout_seconds": args.run_timeout,
                                      "reopen_timeout_seconds": args.reopen_timeout,
                                      "initial_machine": baseline,
                                      "time_unix": time.time()})
        else:
            write_json(args.output / "protocol.json", protocol)
        completed = {(result["repetition"], result["workers"], result["arm"]) for result in results}
        for repetition in range(1, args.repetitions + 1):
            arms = ["baseline", "candidate"] if repetition % 2 else ["candidate", "baseline"]
            for workers in args.workers:
                for arm in arms:
                    if (repetition, workers, arm) in completed:
                        continue
                    directory = Path(tempfile.mkdtemp(
                        prefix=f"r{repetition}-w{workers}-{arm}.", dir=args.output))
                    database, manifest = directory / "rocks", directory / "expected.txt"
                    write_json(directory / "before.json", checked_machine(baseline))
                    common = ["taskset", "-c", args.helper_affinity, str(binaries[arm])]
                    paths = ["--path", str(database), "--manifest", str(manifest),
                             "--writeback-cpu", str(args.writeback_cpu)]
                    placement = (["--worker-cpus", ",".join(map(str, worker_cpus[:workers]))]
                                 if args.worker_cpus is not None else [])
                    run = execute(common + ["run"] + paths + placement + [
                        "--workers", str(workers), "--seconds", str(args.seconds),
                        "--sample-seconds", "10", "--keys-per-worker", "256",
                        "--value-bytes", "128", "--rate-per-worker", "0",
                        "--disk-limit-bytes", str(96 << 30),
                        "--minimum-free-bytes", str(80 << 30)],
                        directory, "run", args.run_timeout, baseline)
                    verify = execute(common + ["verify"] + paths, directory, "verify",
                                     args.reopen_timeout, baseline)
                    cell_protocol, drained, verified = cell_events(directory)
                    validate_cell(cell_protocol, drained, verified, arm, workers, args.seconds,
                                  worker_cpus[:workers], args.writeback_cpu)
                    write_json(directory / "after.json", checked_machine(baseline))
                    result = {"arm": arm, "workers": workers, "repetition": repetition,
                              "directory": str(directory), "run": run, "verify": verify,
                              "protocol": cell_protocol, "drained": drained, "verified": verified,
                              "drained_and_verified": True}
                    # Only this exact successfully verified generated DB is
                    # disposable; all source reports and manifests remain.
                    if database.resolve() != directory / "rocks" or not manifest.is_file():
                        raise RuntimeError("unexpected scratch database path")
                    shutil.rmtree(database)
                    result["scratch_cleaned"] = True
                    write_json(directory / "result.json", result)
                    results.append(result)
                    write_json(args.output / "results.json", results)
                    print(json.dumps({key: result[key] for key in
                          ("arm", "workers", "repetition", "directory", "drained_and_verified")}), flush=True)


if __name__ == "__main__":
    main()
