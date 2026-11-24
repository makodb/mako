#!/usr/bin/env python3
"""Relocate all governed tables of warehouse 1 between two Rust owner processes.

Run inside the build container. The first relocation happens at the load
barrier; subsequent relocations run during TPC-C. Tables move sequentially,
exercising transactions across mixed placements; this is not an atomic group
migration. Logs and parsed results stay under BUILD_DIR.
"""
from pathlib import Path
import argparse
import json
import os
import re
import subprocess
import tempfile
import time

TABLES = ("warehouse", "district", "customer", "customer_name_idx", "history",
          "new_order", "oorder", "oorder_c_id_idx", "order_line", "stock", "stock_data")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--runtime", type=int, default=240)
    args = parser.parse_args()
    if args.rounds < 2 or args.runtime < 30:
        parser.error("at least two rounds and 30 measurement seconds are required")
    root = Path(__file__).resolve().parents[1]
    build = root / os.environ.get("BUILD_DIR", "build_docker")
    out = Path(tempfile.mkdtemp(prefix="rust-warehouse-relocations-", dir=build))
    print(f"logs {out}", flush=True)
    for name in ("src", "config"):
        (out / name).symlink_to(root / name)
    sync = out / "sync"
    sync.mkdir()
    barriers = [out / f"nfs_sync_127.0.0.1_6001_load_phase_{n}" for n in (0, 1)]
    for barrier in barriers:
        os.mkfifo(barrier)
    logs = [out / f"owner-{n}.log" for n in (0, 1)]
    children = []
    pipes = []
    migrations = []
    client = time.time_ns() & ((1 << 63) - 1)
    base = [str(build / "sto_tpcc_bench"), "--num-threads", "1", "--shard-config",
            str(root / "src/mako/config/local-shards2-warehouses1.yml"),
            "--paxos-proc-name", "localhost", "--sync-dir", str(sync),
            "--runtime", str(args.runtime), "--storage-engine", "rust", "--slow-exit"]

    def contents():
        return [log.read_text(errors="replace") if log.exists() else "" for log in logs]

    def alive():
        for n, process in enumerate(children):
            if process.poll() is not None:
                raise RuntimeError(f"owner {n} exited during migration: {process.returncode}")

    def wait_for(marker, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            alive()
            if all(marker in text for text in contents()):
                return
            time.sleep(0.1)
        raise RuntimeError(f"owners did not reach {marker}")

    def admin(sequence, verb, *fields):
        result = subprocess.run([str(build / "mako_admin"), verb, "127.0.0.1:51000",
                                 str(client), str(sequence), *fields],
                                cwd=out, capture_output=True, text=True, timeout=30)
        text = result.stdout + result.stderr
        with (out / "admin.log").open("a") as log:
            log.write(f"sequence={sequence} {verb} exit={result.returncode}\n{text}")
        # The synchronous storage queue can keep an admin poll waiting past
        # the generic RPC deadline. Retry the same retained nonce and intent;
        # neither a transport timeout nor Busy says that migration failed.
        if re.search(r"Native sharding (?:begin|poll) failed: (?:retry|busy|io-error)", text):
            return None
        match = re.search(r"generation=(\d+) outcome=(\w+)", text)
        if not match or (result.returncode and match[2] != "aborted"):
            raise RuntimeError(text)
        return int(match[1]), match[2]

    try:
        for owner in (0, 1):
            with logs[owner].open("w") as log:
                children.append(subprocess.Popen(base + ["--shard-index", str(owner)],
                    cwd=out, stdout=log, stderr=subprocess.STDOUT,
                    env=dict(os.environ, MAKO_CLUSTER_CONFIG="1")))
        wait_for("Finish loading data and wait", 300)
        for round_number in range(args.rounds):
            source = round_number % 2
            destination = 1 - source
            for number, table in enumerate(TABLES):
                sequence = round_number * len(TABLES) + number + 1
                started = time.monotonic()
                deadline = started + 600
                result = None
                while result is None and time.monotonic() < deadline:
                    alive()
                    result = admin(sequence, "begin", table, "00000001", "00000002",
                                   str(source), str(destination))
                    if result is None:
                        time.sleep(0.05)
                if result is None:
                    raise RuntimeError(f"{table}: migration never started")
                generation, state = result
                while state == "pending" and time.monotonic() < deadline:
                    alive()
                    time.sleep(0.05)
                    result = admin(sequence, "poll")
                    if result is None:
                        continue
                    if result[0] != generation:
                        raise RuntimeError(f"{table}: generation changed")
                    state = result[1]
                repeated = None
                while state == "committed" and repeated is None and time.monotonic() < deadline:
                    alive()
                    repeated = admin(sequence, "poll")
                    if repeated is None:
                        time.sleep(0.05)
                if state != "committed" or repeated != (generation, state):
                    raise RuntimeError(f"{table}: outcome {state}")
                if round_number and any("TPCC_BENCH_MEASURE_END" in text for text in contents()):
                    raise RuntimeError("measurement ended before live relocations finished")
                record = dict(round=round_number, table=table, source=source,
                              destination=destination, generation=generation,
                              seconds=time.monotonic() - started)
                migrations.append(record)
                print(json.dumps(record), flush=True)
            if round_number == 0:
                if any("TPCC_BENCH_MEASURE_START" in text for text in contents()):
                    raise RuntimeError("measurement escaped the load barrier")
                for n, barrier in enumerate(barriers):
                    barrier.rename(out / f"held-{n}")
                    barrier.write_text("DONE")
                pipes = [os.open(out / f"held-{n}", os.O_RDWR | os.O_NONBLOCK) for n in (0, 1)]
                wait_for("TPCC_BENCH_MEASURE_START", 30)
                time.sleep(2)
        results = []
        for owner, process in enumerate(children):
            if process.wait(timeout=args.runtime + 60) != 0:
                raise RuntimeError(f"owner {owner}: exit {process.returncode}")
            parsed = [json.loads(line.split("TPCC_BENCH_RESULT ", 1)[1])
                      for line in logs[owner].read_text(errors="replace").splitlines()
                      if line.startswith("TPCC_BENCH_RESULT ")]
            if len(parsed) != 1:
                raise RuntimeError(f"owner {owner}: missing unique result")
            result = parsed[0]
            if not (result["engine"] == "rust" and result["commits"] > 0
                    and result["attempts"] == result["commits"] + result["aborts"]
                    and sum(result["mix"].values()) == result["commits"]
                    and result["mix"]["NewOrder"] > 0 and result["mix"]["Payment"] > 0):
                raise RuntimeError(f"owner {owner}: invalid transaction result {result}")
            results.append(result)
            print(json.dumps(dict(owner=owner, result=result)), flush=True)
        (out / "results.json").write_text(json.dumps(dict(migrations=migrations, owners=results), indent=2))
        print(f"PASS {len(migrations)} table migrations, two owner processes", flush=True)
    finally:
        for process in children:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for pipe in pipes:
            os.close(pipe)
        for n, process in enumerate(children):
            if process.returncode:
                print(f"owner {n} tail:\n{contents()[n][-2500:]}", flush=True)


if __name__ == "__main__":
    main()
