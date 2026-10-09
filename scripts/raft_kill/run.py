#!/usr/bin/env python3
"""raft_kill: process-kill tests for Raft disk mode (docs/verus/disk-persistence-plan.md P6).

Three raft_kill_node processes, one Raft replica each, on fresh stores in a
locked run directory under /var/tmp/raft-wal-$USER (MAKO_RAFT_DATA_ROOT moves
it). Each scenario SIGKILLs replicas, at random times or at named crash
points (some simulating a power cut), restarts them onto their stores, then
pauses proposals, lets the followers catch up, stops every replica cleanly
(each compares its WAL with its state: [DISK-VERIFY]) and runs check.py.

  run.py --build-dir build_rust_disk [--scenario NAME ...] [--rate HZ]
         [--delay-us US] [--segment-bytes N] [--keep]

Scenarios: follower, leader, majority, all, points, powercut, create.
On a pass the run directory is deleted; on a failure it is kept (and
compressed into $RESULTS/raft_kill/ when RESULTS is set).
"""
import argparse
import fcntl
import os
import random
import re
import shutil
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check as checker  # noqa: E402

PROCS = ["localhost", "p1", "p2"]
POINTS_SNAP = ["image.rename", "image.dirsync", "image.delete"]
POINTS = ["wal.write.half", "wal.write.done", "wal.sync.done", "wal.rotate.created",
          "wal.rotate.dirsync", "recover.segment", "recover.base", "base.write", "base.flush",
          "base.delete"]


def sweep(root):
    for d in root.glob("run-*"):
        lock = d / "RUN.lock"
        if not lock.exists():
            if time.time() - d.stat().st_mtime > 60:
                shutil.rmtree(d, ignore_errors=True)
            continue
        with open(lock, "a") as f:
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                continue
        shutil.rmtree(d, ignore_errors=True)
        print(f"raft_kill: swept {d}")


def free_ports(n):
    for _ in range(200):
        base = random.randrange(15000, 19900, 10)
        ports = [base + 100 * i for i in range(n)]
        if all(p < 20000 for p in ports) and all(_bindable(p) for p in ports):
            return ports
    raise RuntimeError("no free ports in 15000-19999")


def _bindable(port):
    s = socket.socket()
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


class Cluster:
    def __init__(self, args, label):
        user = os.environ.get("USER", "raft")
        root = Path(os.environ.get("MAKO_RAFT_DATA_ROOT", f"/var/tmp/raft-wal-{user}"))
        root.mkdir(parents=True, exist_ok=True)
        sweep(root)
        self.dir = Path(tempfile.mkdtemp(prefix=f"run-kill-{label}.", dir=root))
        self.lock = open(self.dir / "RUN.lock", "w")
        fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        (self.dir / "events").mkdir()
        (self.dir / "logs").mkdir()
        self.args = args
        self.bin = ROOT / args.build_dir / "raft_kill_node"
        self.pause = self.dir / "pause"
        ports = free_ports(3)
        servers = ", ".join(f'"s{101 + 100 * i}:{ports[i]}"' for i in range(3))
        self.topology = self.dir / "topology.yml"
        self.topology.write_text(
            "site:\n  server:\n    - [" + servers + "]\n"
            "process:\n  s101: localhost\n  s201: p1\n  s301: p2\n"
            "host:\n  localhost: 127.0.0.1\n  p1: 127.0.0.1\n  p2: 127.0.0.1\n")
        self.inc = {p: -1 for p in PROCS}
        self.procs = {}
        self.log = open(self.dir / "run.log", "a")

    def note(self, msg):
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(f"raft_kill: {line}", flush=True)
        self.log.write(line + "\n")
        self.log.flush()

    def launch(self, proc, create=False, crash=None):
        self.inc[proc] += 1
        inc = self.inc[proc]
        env = dict(os.environ)
        for k in ("MAKO_RAFT_CREATE", "MAKO_RAFT_CRASH"):
            env.pop(k, None)
        env["MAKO_RAFT_DATA_DIR"] = str(self.dir)
        env["MAKO_RAFT_DISK_VERIFY"] = "1"
        env["MAKO_RAFT_FLUSH_DELAY_US"] = str(self.args.delay_us)
        env["MAKO_RAFT_SEGMENT_BYTES"] = str(self.args.segment_bytes)
        # Checkpoints often, so the base deletes segments during the run.
        env["MAKO_RAFT_CHECKPOINT_BYTES"] = str(4 * self.args.segment_bytes)
        env["MAKO_RAFT_CHECKPOINT_SECS"] = "1"
        if self.args.snapshots:
            env["MAKO_RAFT_SNAPSHOTS"] = "1"
            env["MAKO_RAFT_SNAPSHOT_INTERVAL"] = "300"
        if create:
            env["MAKO_RAFT_CREATE"] = "1"
        if crash:
            env["MAKO_RAFT_CRASH"] = crash
        log = open(self.dir / "logs" / f"{proc}.{inc}.log", "w")
        cmd = [str(self.bin), "--proc", proc, "--topology", str(self.topology),
               "--mode", str(ROOT / "config" / "raft.yml"),
               "--events", str(self.dir / "events" / f"{proc}.events"),
               "--incarnation", str(inc), "--rate", str(self.args.rate),
               "--pause-file", str(self.pause)] + (["--snapshots"] if self.args.snapshots else [])
        self.procs[proc] = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env,
                                            cwd=ROOT, start_new_session=True)
        self.note(f"launch {proc}.{inc}" + (" create" if create else "") + (f" crash={crash}" if crash else ""))

    def lines(self, proc):
        p = self.dir / "events" / f"{proc}.events"
        if not p.exists():
            return []
        incs = checker.incarnations(p.read_text())
        return incs[-1][1] if incs and incs[-1][0] == self.inc[proc] else []

    def alive(self, proc):
        p = self.procs.get(proc)
        return p is not None and p.poll() is None

    def ready(self, proc):
        return any(l[0] == "ready" for l in self.lines(proc))

    def leader(self):
        for proc in PROCS:
            if not self.alive(proc):
                continue
            roles = [l[1] for l in self.lines(proc) if l[0] == "role"]
            if roles and roles[-1] == "leader":
                return proc
        return None

    def acks(self):
        n = 0
        for proc in PROCS:
            p = self.dir / "events" / f"{proc}.events"
            if p.exists():
                n += sum(1 for l in p.read_text().splitlines() if l.startswith("ack "))
        return n

    def wait(self, what, cond, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if cond():
                return
            time.sleep(0.2)
        raise RuntimeError(f"timed out after {timeout} s waiting for {what}")

    def progress(self, n=20, timeout=60):
        start = self.acks()
        self.wait(f"{n} more acknowledgements (from {start})", lambda: self.acks() >= start + n, timeout)

    def kill(self, proc):
        p = self.procs[proc]
        if p.poll() is None:
            p.send_signal(signal.SIGKILL)
        p.wait()
        self.note(f"SIGKILL {proc}.{self.inc[proc]}")

    def start_all(self, create=True):
        for proc in reversed(PROCS):
            self.launch(proc, create=create)
        self.wait("all ready", lambda: all(self.ready(p) for p in PROCS), 60)
        self.wait("a leader", lambda: self.leader() is not None, 60)
        self.progress()

    def finish(self):
        self.pause.touch()
        time.sleep(3)  # followers catch up with the last commit
        for proc in PROCS:
            if self.alive(proc):
                self.procs[proc].send_signal(signal.SIGTERM)
        deadline = time.time() + 60
        for proc in PROCS:
            p = self.procs.get(proc)
            if p is None:
                continue
            try:
                p.wait(max(1, deadline - time.time()))
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
                self.note(f"{proc} did not stop on SIGTERM; killed")
        return checker.check(self.dir)

    def abandon(self):
        for proc in PROCS:
            if self.alive(proc):
                self.procs[proc].kill()
                self.procs[proc].wait()


def scenario(c, name):
    c.start_all()
    if name in ("follower", "leader", "majority", "all"):
        for _ in range(2):
            lead = c.leader()
            followers = [p for p in PROCS if p != lead]
            victims = {"follower": [random.choice(followers)], "leader": [lead],
                       "majority": [lead, random.choice(followers)], "all": list(PROCS)}[name]
            for v in victims:
                c.kill(v)
            if len(victims) == 1:
                c.progress()  # the survivors carry on
            else:
                time.sleep(1)
            for v in victims:
                c.launch(v)
            c.wait("restarted replicas ready", lambda: all(c.ready(v) for v in victims), 60)
            c.wait("a leader", lambda: c.leader() is not None, 60)
            c.progress()
    elif name in ("points", "powercut"):
        for point in POINTS + (POINTS_SNAP if c.args.snapshots else []):
            lead = c.leader()
            for target in (random.choice([p for p in PROCS if p != lead]), lead):
                c.kill(target)
                # A rotation happens once a segment fills (seconds apart at
                # the test's rate); a write or sync on every flush.
                nth = (1 if point.startswith("recover") else
                       random.randint(1, 2) if (".rotate." in point or point in ("base.flush", "base.delete"))
                       else random.randint(2, 20))
                spec = f"{point}:{nth}" + (":powercut" if name == "powercut" else "")
                c.launch(target, crash=spec)
                try:
                    c.wait(f"{target} to die at {spec}", lambda: not c.alive(target), 30)
                except RuntimeError:
                    c.note(f"{spec} not reached in 30 s; killing {target}")
                    c.kill(target)
                c.launch(target)
                c.wait(f"{target} ready", lambda: c.ready(target), 60)
                c.wait("a leader", lambda: c.leader() is not None, 60)
                c.progress()
                lead = c.leader()
    elif name == "create":
        # A creating launch killed at each creation step, then relaunched
        # with the flag (a half-made store must not block it).
        for point in ("create.files", "create.rename"):
            for spec in (point, point + ":powercut"):
                victim = random.choice(PROCS)
                c.kill(victim)
                shutil.rmtree(c.dir / f"{PROCS.index(victim)}-0", ignore_errors=True)
                c.launch(victim, create=True, crash=spec)
                c.wait(f"{victim} to die at {spec}", lambda: not c.alive(victim), 30)
                c.launch(victim, create=True)
                c.wait(f"{victim} ready", lambda: c.ready(victim), 60)
                c.progress()
    else:
        raise ValueError(f"unknown scenario {name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="build_rust_disk")
    ap.add_argument("--scenario", action="append")
    ap.add_argument("--rate", type=float, default=100.0)
    ap.add_argument("--delay-us", type=int, default=0)
    ap.add_argument("--segment-bytes", type=int, default=1 << 16)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--seed", type=int)
    ap.add_argument("--snapshots", action="store_true",
                    help="snapshot images (plan P8): callbacks on, a small interval")
    args = ap.parse_args()
    seed = args.seed if args.seed is not None else random.randrange(1 << 30)
    random.seed(seed)
    names = args.scenario or ["follower", "leader", "majority", "all", "points", "powercut", "create"]
    failed = []
    for name in names:
        c = Cluster(args, name)
        c.note(f"scenario {name} seed {seed} in {c.dir}")
        try:
            scenario(c, name)
            problems, summary = c.finish()
        except Exception as e:  # a hang or a refusal: keep the evidence
            c.abandon()
            problems, summary = [f"driver: {e}"], "aborted"
        for p in problems:
            c.note(f"FAIL {p}")
        c.note(f"{name}: {'FAIL' if problems else 'pass'}: {summary}")
        if problems or args.keep:
            failed += [name] if problems else []
            results = os.environ.get("RESULTS")
            if problems and results:
                out = Path(results) / "raft_kill"
                out.mkdir(parents=True, exist_ok=True)
                tgz = out / f"{c.dir.name}.tar.gz"
                with tarfile.open(tgz, "w:gz") as t:
                    t.add(c.dir, arcname=c.dir.name, filter=lambda ti: None if "/wal/" in ti.name else ti)
                c.note(f"evidence: {tgz} (kept: {c.dir})")
        else:
            c.lock.close()
            shutil.rmtree(c.dir, ignore_errors=True)
    print(f"raft_kill: {len(names) - len(failed)}/{len(names)} scenario(s) passed"
          + (f"; failed: {' '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
