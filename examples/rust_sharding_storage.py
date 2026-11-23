#!/usr/bin/env python3
"""Run the Rust-engine sharding storage fixture, one process per scenario.

Each scenario is one run of build/rust_sharding_storage_fixture against a real
Rust-engine database, the real sharding crate and real srpc. This driver only
starts the process and compares what it reported with what the scenario must
report: the exact set of failed checks, the summary line, the exit code and
the required standard-error lines. A scenario named red-* injects or keeps a
defect, and its expected result is the failure.

    python3 examples/rust_sharding_storage.py [--only SCENARIO]
"""
from pathlib import Path
import argparse
import os
import random
import re
import socket
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / os.environ.get("BUILD_DIR", "build_docker")
BINARY = BUILD / "rust_sharding_storage_fixture"
LOGS = BUILD / "rust-sharding-storage"
PROCESS_TIMEOUT_SECONDS = 180

CALLS = (r"rust_sharding_fixture: calls owner={owner}"
         r" scan_next={scan} scan_next_ok={scan_ok} scan_next_last={scan_last}"
         r" put={put} put_ok={put_ok} put_last={put_last}"
         r" remove={remove} remove_ok={remove_ok} remove_last={remove_last}$")


def calls(owner, scan, scan_ok, scan_last, put="0", put_ok="0", put_last="none",
          remove="0", remove_ok="0", remove_last="none"):
    """A pattern for the fixture's per-owner line of forwarded storage calls."""
    return CALLS.format(owner=owner, scan=scan, scan_ok=scan_ok, scan_last=scan_last,
                        put=put, put_ok=put_ok, put_last=put_last, remove=remove,
                        remove_ok=remove_ok, remove_last=remove_last)


# Per scenario:
#   exit     the exit code
#   summary  PASS or FAIL
#   failed   check name -> pattern its detail must match; the names are the
#            exact set of checks that may report FAILED
#   stderr   patterns that must each match one line of standard error
SCENARIOS = {
    # The C++ storage callbacks, unchanged, on a Rust-engine database.
    #
    # The C++ scan finds no MBTA table behind the handle and answers IO (6)
    # before it looks at a row. Precopy asks the source for its first row and
    # gets that IO, so the coordinator aborts. Abort makes the destination
    # clean its stage range, which needs the same scan, so that cleanup fails
    # and is repeated without end, and the poll answers "pending" for ever
    # because an abort is reported only after both sides are done.
    "red-baseline": {
        "exit": 1,
        "summary": "FAIL",
        "failed": {"outcome-committed": r"outcome=pending$"},
        "stderr": [
            # The source's scan was reached and refused; nothing was put or
            # removed. Without this line "pending" could come from a migration
            # that never got as far as the storage callbacks.
            calls(0, scan=r"[1-9]\d*", scan_ok="0", scan_last="6"),
            # The destination's cleanup scan, refused and repeated (ten times
            # at least; it is retried every 20 ms).
            calls(1, scan=r"[1-9]\d+", scan_ok="0", scan_last="6"),
        ],
    },
}

CHECK_LINE = re.compile(r"CHECK (\S+) (ok|FAILED|SKIPPED)(?: (.*))?$")
SUMMARY_LINE = re.compile(r"RUST_SHARD_STORAGE (PASS|FAIL) (\S+)$")


def ports():
    """A base port whose four derived ports are free, as the C++ smoke picks it."""
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
    raise RuntimeError("No free loopback port set for the fixture")


def run_fixture(scenario):
    """Start one fixture process; return (exit code or None on timeout, out, err)."""
    LOGS.mkdir(parents=True, exist_ok=True)
    out_path = LOGS / f"{scenario}.out"
    err_path = LOGS / f"{scenario}.err"
    environment = {**os.environ, "MAKO_CLUSTER_CONFIG": "1"}
    # Live-only: retained sharding metadata is not part of this fixture.
    environment.pop("MAKO_SHARD_METADATA_DIR", None)
    environment.pop("MAKO_SHARD_METADATA_INIT", None)
    base = ports()
    with tempfile.TemporaryDirectory(prefix="mako-rust-shard-") as directory:
        config = Path(directory) / "config.yml"
        # The old-format transport::Configuration schema dbtest reads.
        config.write_text(f"shards: 2\nwarehouses: 1\n"
                          f"memlocalhost: 0\nmemlearner: 0\nmemp1: 0\nmemp2: 0\nlocalhost:\n"
                          f"  - ip: 127.0.0.1\n    port: {base}\n"
                          f"  - ip: 127.0.0.1\n    port: {base + 100}\n")
        with out_path.open("wb") as out, err_path.open("wb") as err:
            process = subprocess.Popen([str(BINARY), str(config), scenario],
                                       stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                       env=environment)
            try:
                code = process.wait(timeout=PROCESS_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                code = None
    return code, out_path, err_path


def compare(scenario, expected, code, out_path, err_path):
    """Return (differences, what the run reported)."""
    out_lines = out_path.read_text(errors="replace").splitlines()
    err_lines = err_path.read_text(errors="replace").splitlines()
    differences = []
    if code is None:
        differences.append(f"no exit within {PROCESS_TIMEOUT_SECONDS} seconds (killed)")
    elif code != expected["exit"]:
        differences.append(f"exit code {code}, expected {expected['exit']}")

    checks = {}
    summaries = []
    for line in out_lines:
        check = CHECK_LINE.match(line)
        if check:
            if check[1] in checks:
                differences.append(f"check {check[1]} reported twice")
            checks[check[1]] = (check[2], check[3] or "")
        summary = SUMMARY_LINE.match(line)
        if summary:
            summaries.append((summary[1], summary[2]))
    if summaries != [(expected["summary"], scenario)]:
        seen = ", ".join(f"{word} {name}" for word, name in summaries) or "none"
        differences.append(f"summary line: {seen}; expected {expected['summary']} {scenario}")

    failed = {name: detail for name, (state, detail) in checks.items() if state == "FAILED"}
    for name in sorted(set(failed) - set(expected["failed"])):
        differences.append(f"check {name} FAILED ({failed[name]}), expected it not to fail")
    for name, pattern in sorted(expected["failed"].items()):
        if name not in failed:
            state = checks.get(name, ("not reported", ""))[0]
            differences.append(f"check {name} is {state}, expected FAILED")
        elif not re.search(pattern, failed[name]):
            differences.append(f"check {name} FAILED with '{failed[name]}', "
                               f"expected a detail matching '{pattern}'")

    for pattern in expected["stderr"]:
        if not any(re.search(pattern, line) for line in err_lines):
            differences.append(f"no standard-error line matches '{pattern}'")

    reported = "; ".join(f"{name} FAILED {detail}" for name, detail in sorted(failed.items()))
    return differences, reported or "no failed check"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", choices=sorted(SCENARIOS), metavar="SCENARIO",
                        help="run one scenario: " + ", ".join(sorted(SCENARIOS)))
    arguments = parser.parse_args()
    if not (Path("/.dockerenv").exists() or Path("/run/.containerenv").exists()):
        raise SystemExit("Run inside the build container: ./docker_build.sh enter")
    if not os.access(BINARY, os.X_OK):
        raise SystemExit(f"Missing Docker-built executable: {BINARY}")

    names = [arguments.only] if arguments.only else list(SCENARIOS)
    unexpected = 0
    for name in names:
        code, out_path, err_path = run_fixture(name)
        differences, reported = compare(name, SCENARIOS[name], code, out_path, err_path)
        if differences:
            unexpected += 1
            print(f"UNEXPECTED {name}: " + "; ".join(differences))
            print(f"  logs: {out_path} {err_path}")
        else:
            print(f"ok {name}: exit {code}, {reported}")
        sys.stdout.flush()
    print(f"rust sharding storage: {len(names)} scenario(s), {unexpected} unexpected")
    return 1 if unexpected else 0


if __name__ == "__main__":
    sys.exit(main())
