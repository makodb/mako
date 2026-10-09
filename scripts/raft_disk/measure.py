#!/usr/bin/env python3
"""Measures disk mode against the cost model (docs/verus/disk-persistence-plan.md §5).

Paired rounds of a memory build and a disk build at one gate point
(scripts/raft_perf/rotation_trial.sh, alternating arms), with the injected
delay D and the store on tmpfs or the local disk; the memory arm's medians are
the model's baseline, the disk arm's are compared with its estimates
(scripts/raft_disk/model.py --compare).

  measure.py --point G1 --delay-us 1000 --fs tmpfs --rounds 5 \\
             [--mem build_rust] [--disk build_rust_disk] [--out DIR]
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
POINTS = {  # as scripts/verus/gate_point.sh runs them
    "G1": dict(PAYLOAD=4096, RATE=240, MAXOUT=4096, PARTS=1, GROUP="single", DUR=10),
    "G2": dict(PAYLOAD=4096, RATE=0, MAXOUT=4096, PARTS=1, GROUP="single", DUR=10),
    "G5": dict(PAYLOAD=1048576, RATE=55, MAXOUT=64, PARTS=1, GROUP="single", DUR=10),
    "G6": dict(PAYLOAD=1048576, RATE=0, MAXOUT=64, PARTS=1, GROUP="single", DUR=10),
}


def medians(out, arm):
    recs = [json.loads(p.read_text()) for p in sorted(out.glob(f"r*.{arm}.json"))]
    if not recs:
        raise SystemExit(f"measure: no records for {arm} in {out}")
    return {
        "p50": statistics.median(r["latency_p50_us"] for r in recs),
        "p99": statistics.median(r["latency_p99_us"] for r in recs),
        "X": statistics.median(r["applied_per_sec"] for r in recs),
        "n": len(recs),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--point", required=True, choices=POINTS)
    ap.add_argument("--delay-us", type=int, default=0)
    ap.add_argument("--fs", default="tmpfs", choices=["tmpfs", "ext4"])
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--mem", default="build_rust")
    ap.add_argument("--disk", default="build_rust_disk")
    ap.add_argument("--out")
    a = ap.parse_args()
    user = os.environ.get("USER", "raft")
    out = Path(a.out or Path(os.environ.get("RESULTS", "/var/tmp")) / "disk-model"
               / f"{a.point}-D{a.delay_us}-{a.fs}")
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.update({k: str(v) for k, v in POINTS[a.point].items()})
    env["MAKO_RAFT_FLUSH_DELAY_US"] = str(a.delay_us)
    env["MAKO_RAFT_DATA_ROOT"] = f"/dev/shm/raft-wal-{user}" if a.fs == "tmpfs" else f"/var/tmp/raft-wal-{user}"
    subprocess.run([str(ROOT / "scripts/raft_perf/rotation_trial.sh"), str(out), str(a.rounds), a.mem, a.disk],
                   cwd=ROOT, env=env, check=True)
    mem, disk = medians(out, a.mem), medians(out, a.disk)
    low = POINTS[a.point]["RATE"] != 0
    base = {f"{a.point}_p50": mem["p50"]} if low else {a.point: mem["X"]}
    measured = {"D": a.delay_us, (f"{a.point}_p50" if low else a.point): disk["p50"] if low else disk["X"]}
    (out / "baseline.json").write_text(json.dumps(base))
    (out / "measured.json").write_text(json.dumps(measured))
    print(f"measure: {a.point} D={a.delay_us}us fs={a.fs}: memory {mem} disk {disk}")
    r = subprocess.run([sys.executable, str(ROOT / "scripts/raft_disk/model.py"), "--fs", a.fs, "--baseline",
                        str(out / "baseline.json"), "--compare", str(out / "measured.json")])
    return r.returncode


if __name__ == "__main__":
    raise SystemExit(main())
