#!/usr/bin/env python3
"""Run the Rust-engine sharding storage fixture, one process per scenario.

Each scenario is one run of build/rust_sharding_storage_fixture against a real
Rust-engine database, the real sharding crate and real srpc. This driver only
starts the process and compares what it reported with what the scenario must
report: the state of every check (ok, FAILED with its detail, or SKIPPED, and
no check missing), the summary line, the exit code, the standard-error lines
that must be there and those that must not. A scenario named red-* injects or
keeps a defect, and its expected result is the failure.

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

CALLS = (r"^rust_sharding_fixture: calls owner={owner}"
         r" scan_next={scan} scan_next_ok={scan_ok} scan_next_last={scan_last}"
         r" put={put} put_ok={put_ok} put_last={put_last}"
         r" remove={remove} remove_ok={remove_ok} remove_last={remove_last}$")


def calls(owner, scan, scan_ok, scan_last, put="0", put_ok="0", put_last="none",
          remove="0", remove_ok="0", remove_last="none"):
    """A pattern for the fixture's per-owner line of forwarded storage calls."""
    return CALLS.format(owner=owner, scan=scan, scan_ok=scan_ok, scan_last=scan_last,
                        put=put, put_ok=put_ok, put_last=put_last, remove=remove,
                        remove_ok=remove_ok, remove_last=remove_last)


def epochs(owner, cuts="0", busy="0", ok="0"):
    """A pattern for the fixture's per-owner line of forwarded epoch calls."""
    return (rf"^rust_sharding_fixture: epochs owner={owner} cuts={cuts}"
            rf" drained_busy={busy} drained_ok={ok} drained_other=0$")


AT_LEAST_FIVE = r"(?:[5-9]|[1-9]\d+)"


def moved(rows, rewritten=0):
    """Storage calls of a committed migration of `rows` source rows.

    The destination starts with two stale rows in the range: m0007, which the
    source also has, and m9999, which it has not.

    Source: precopy and the final capture each read every row and then get
    NOT_FOUND (1); the cleanup after the commit does the same and removes each
    row once. Nothing is ever put on the source.
    Destination: precopy puts every row, m0007 among them. The final copy then
    compares its copy with the source, writes the rows changed during drain,
    removes m9999, and ends on NOT_FOUND.
    """
    return [
        calls(0, scan=3 * (rows + 1), scan_ok=3 * rows, scan_last="1",
              remove=rows, remove_ok=rows, remove_last="0"),
        calls(1, scan=rows + 2, scan_ok=rows + 1, scan_last="1",
              put=rows + rewritten, put_ok=rows + rewritten, put_last="0", remove="1", remove_ok="1", remove_last="0"),
    ]


# Standard-error lines that only the Rust storage callbacks write: the C++
# glue when the crate's storage thread of an owner enters and leaves, and the
# Rust verbs the first time each runs on that thread. They show that the Rust
# functions ran, which the counts of the fixture's wrapper cannot.
ENTERED_AND_LEFT = [
    r"^rust_sharding_host: entered owner=0 slot=\d+$",
    r"^rust_sharding_host: entered owner=1 slot=\d+$",
    r"^rust_sharding_host: left owner=0$",
    r"^rust_sharding_host: left owner=1$",
    r"^rust_sharding_fixture: threads owner=0 enter=1 enter_ok=1 leave=1 attached_after_leave=0$",
    r"^rust_sharding_fixture: threads owner=1 enter=1 enter_ok=1 leave=1 attached_after_leave=0$",
    # Both table handles are registered before the nodes start, so the crate
    # never asks the host to open one.
    r"^rust_sharding_fixture: opens owner=0 open=0$",
    r"^rust_sharding_fixture: opens owner=1 open=0$",
]
FIRST_USE_COMMITTED = [
    r"^mako_sto_shard_host: first scan_next owner=0 table=1001$",
    r"^mako_sto_shard_host: first scan_next owner=1 table=1001$",
    r"^mako_sto_shard_host: first put owner=1 table=1001$",
    r"^mako_sto_shard_host: first remove owner=0 table=1001$",
    r"^mako_sto_shard_host: first remove owner=1 table=1001$",
]
# No passing run may print these: a contained panic, an engine handle that
# could not be released, a refusal or an indefinite outcome inside a verb, a
# requirement of the fixture that did not hold, or a put on the source.
NEVER_WHEN_PASSING = [
    r"panicked at",
    r"^rust_sharding_host: .* failed",
    r"^mako_sto_shard_host: .*(refused|contained a panic|not definite)",
    r"SETUP ERROR",
    r"^mako_sto_shard_host: first put owner=0 ",
]

BEFORE_MIGRATION = ["tick-running", "registration-guarded", "open-registered-only",
                    "unentered-thread-refused"]
AFTER_COMMIT = ["route-moved", "destination-image", "stale-collide-replaced",
                "stale-extra-removed", "destination-outside-untouched", "source-range-empty",
                "source-outside-untouched", "callbacks-ran", "no-pin-left"]
AFTER_ABORT = ["route-unmoved", "source-intact", "destination-stage-clean", "callbacks-ran",
               "no-pin-left"]

# Per scenario:
#   exit       the exit code
#   summary    PASS or FAIL
#   ok         the checks that must report ok
#   failed     check name -> pattern its detail must match
#   skipped    the checks that must report SKIPPED
#              (ok, failed and skipped together are every check the run may
#              print; a check that is missing is a difference like any other)
#   stderr     patterns that must each match exactly one line of standard error
#   forbidden  patterns that no line of standard error may match
SCENARIOS = {
    "warehouses": {
        "exit": 0,
        "summary": "PASS",
        "ok": ["warehouse-open", "warehouse-lazy-catalog", "warehouse-committed", "warehouse-bytes"],
        "failed": {},
        "skipped": [],
        "stderr": [
            r"^mako_sto_shard_host: first scan_next owner=0 table=4$",
            r"^mako_sto_shard_host: first put owner=1 table=4$",
            r"^mako_sto_shard_host: first remove owner=0 table=4$",
        ],
        "forbidden": NEVER_WHEN_PASSING,
    },
    # ["", "n") moves from owner 0 to owner 1 on the Rust storage callbacks:
    # 605 rows in two capture pages.
    "migrate": {
        "exit": 0,
        "summary": "PASS",
        "ok": BEFORE_MIGRATION + ["outcome-committed"] + AFTER_COMMIT
              + ["ordered-scan-agrees", "threads-left"],
        "failed": {},
        "skipped": [],
        "stderr": ENTERED_AND_LEFT + FIRST_USE_COMMITTED + moved(605) + [
            # One cut on the source at Freeze, and one OK answer once it has
            # drained; the destination takes no cut.
            epochs(0, cuts="1", busy=r"\d+", ok="1"),
            epochs(1),
        ],
        "forbidden": NEVER_WHEN_PASSING,
    },
    # As migrate, while another thread holds an STO epoch older than the cut:
    # the source must answer BUSY until the pin is released.
    "pinned": {
        "exit": 0,
        "summary": "PASS",
        "ok": BEFORE_MIGRATION + ["receiver-replayed-own-reply", "mixed-peer-refused", "new-owner-read", "former-owner-transaction",
                                  "pinned-stays-pending", "stale-write-refused",
                                  "drain-observed", "outcome-committed"]
              + AFTER_COMMIT + ["threads-left"],
        "failed": {},
        "skipped": [],
        "stderr": ENTERED_AND_LEFT + FIRST_USE_COMMITTED + moved(605, rewritten=1) + [
            # While the pin was held the source answered BUSY, at least the
            # five times the fixture waits for; OK came once, after the release.
            epochs(0, cuts="1", busy=AT_LEAST_FIVE, ok="1"),
            epochs(1),
        ],
        "forbidden": NEVER_WHEN_PASSING,
    },
    # ["c", "n") moves: 601 rows. On both owners 5,000 deleted keys lie in
    # the range below the first live row, so the first lookahead of every
    # capture, of the destination's final copy and of the source's cleanup
    # must cross them. tombstones-block-one-scan is the scenario's premise:
    # the adapter's own scan, one transaction, cannot.
    "deleted-keys": {
        "exit": 0,
        "summary": "PASS",
        "ok": ["tombstones-block-one-scan", "concurrent-directory-migration"] + BEFORE_MIGRATION + ["outcome-committed"]
              + AFTER_COMMIT + ["threads-left"],
        "failed": {},
        "skipped": [],
        "stderr": ENTERED_AND_LEFT + FIRST_USE_COMMITTED + moved(601) + [
            epochs(0, cuts="1", busy=r"\d+", ok="1"),
            epochs(1),
        ],
        "forbidden": NEVER_WHEN_PASSING,
    },
    # As pinned, but the migration is aborted while the pin holds it in the
    # drain loop, with every row already copied to the destination. "Aborted"
    # is this scenario's success: the source keeps every row and removes
    # nothing, and the destination removes its whole stage range, which is
    # the 605 copied rows and the stale m9999.
    "aborted": {
        "exit": 0,
        "summary": "PASS",
        "ok": BEFORE_MIGRATION + ["pinned-stays-pending", "stale-write-refused", "outcome-aborted"] + AFTER_ABORT
              + ["threads-left"],
        "failed": {},
        "skipped": [],
        "stderr": ENTERED_AND_LEFT + [
            r"^mako_sto_shard_host: first scan_next owner=0 table=1001$",
            r"^mako_sto_shard_host: first scan_next owner=1 table=1001$",
            r"^mako_sto_shard_host: first put owner=1 table=1001$",
            r"^mako_sto_shard_host: first remove owner=1 table=1001$",
            # The source: one precopy pass, no cleanup.
            calls(0, scan=606, scan_ok=605, scan_last="1"),
            # The destination: 605 puts, then the cleanup reads and removes
            # 606 rows and ends on NOT_FOUND.
            calls(1, scan=607, scan_ok=606, scan_last="1", put=605, put_ok=605, put_last="0",
                  remove=606, remove_ok=606, remove_last="0"),
            # A drain round that was already on its way when the abort was
            # recorded may still be answered after the release.
            epochs(0, cuts="1", busy=AT_LEAST_FIVE, ok="[01]"),
            epochs(1),
        ],
        "forbidden": NEVER_WHEN_PASSING + [r"^mako_sto_shard_host: first remove owner=0 "],
    },
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
        "ok": ["tick-running"],
        "failed": {"outcome-committed": r"outcome=pending$"},
        "skipped": ["registration-guarded", "open-registered-only", "unentered-thread-refused"]
                   + AFTER_COMMIT,
        "stderr": [
            # The source's scan was reached and refused; nothing was put or
            # removed. Without this line "pending" could come from a migration
            # that never got as far as the storage callbacks.
            calls(0, scan=r"[1-9]\d*", scan_ok="0", scan_last="6"),
            # The destination's cleanup scan, refused and repeated (ten times
            # at least; it is retried every 20 ms).
            calls(1, scan=r"[1-9]\d+", scan_ok="0", scan_last="6"),
        ],
        # The Rust storage callbacks are not installed here.
        "forbidden": [r"^rust_sharding_host: ", r"^mako_sto_shard_host: "],
    },
}

CHECK_LINE = re.compile(r"CHECK (\S+) (ok|FAILED|SKIPPED)(?: (.*))?$")
SUMMARY_LINE = re.compile(r"RUST_SHARD_STORAGE (PASS|FAIL) (\S+)$")


def ports():
    """A base port with room for both migration listeners and the ordinary shard helper."""
    for _ in range(200):
        base = random.randrange(20000, 29000)
        sockets = []
        try:
            for offset in (0, 100, 106, 20000, 20100):
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
    # Keep an ordinary scan's read-set budget below the adversarial tombstone
    # run, independently of the benchmark's production default.
    environment = {**os.environ, "MAKO_CLUSTER_CONFIG": "1",
                   "MAKO_STO_TPCC_MAX_TRANSACTION_ITEMS": "1024"}
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
    # Every other check has one expected state, and no other check may appear.
    for state, names in (("ok", expected["ok"]), ("SKIPPED", expected["skipped"])):
        for name in names:
            seen = checks.get(name, ("not reported", ""))[0]
            if seen not in (state, "FAILED"):
                differences.append(f"check {name} is {seen}, expected {state}")
    known = set(expected["ok"]) | set(expected["skipped"]) | set(expected["failed"])
    for name in sorted(set(checks) - set(failed) - known):
        differences.append(f"check {name} is {checks[name][0]}, expected it not to be reported")

    for pattern in expected["stderr"]:
        matches = sum(1 for line in err_lines if re.search(pattern, line))
        if matches == 0:
            differences.append(f"no standard-error line matches '{pattern}'")
        elif matches > 1:
            differences.append(f"{matches} standard-error lines match '{pattern}', expected one")
    for pattern in expected["forbidden"]:
        for line in err_lines:
            if re.search(pattern, line):
                differences.append(f"standard error has '{line.strip()[:160]}', "
                                   f"which matches the forbidden '{pattern}'")
                break

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
