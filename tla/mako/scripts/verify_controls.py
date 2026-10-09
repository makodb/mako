#!/usr/bin/env python3
"""Require semantic proof failures when MakoV2 or sharding safety rules are weakened.

Run in the same container as verify.sh. No model checker or execution simulator
is used: each mutant must fail its named safety lemma after the
unchanged complete crate passes.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VERUS = shutil.which(os.environ.get("VERUS_PATH", "verus"))
if VERUS is None:
    raise SystemExit("Set VERUS_PATH to the pinned Verus executable")
SYSROOT_ARGS = (["--sysroot", os.environ["VERUS_SYSROOT"]]
                if os.environ.get("VERUS_SYSROOT") else [])

CONTROLS = [
    (
        "barrier crosses an unresolved obligation",
        "normal.rs",
        "==> through < s.obligations[p].ts",
        "==> true",
        ("proofs_replication", "lemma_marker_step_at"),
    ),
    (
        "final response precedes safe watermark coverage",
        "normal.rs",
        "&&& below_view(s.views, c, s.txns[id].coord, s.txns[id].epoch, s.txns[id].ts)",
        "&&& true",
        ("proofs_occ", "lemma_final_step"),
    ),
    (
        "OCC accepts a stale read version",
        "normal.rs",
        "r.reads.dom().contains(k) && top_writer(s.versions, k) == r.reads[k].writer",
        "r.reads.dom().contains(k)",
        ("proofs_occ", "lemma_read_order_prepare"),
    ),
    (
        "new timestamp ignores participant floors",
        "normal.rs",
        "==> ts > s.shards[i].clock",
        "==> ts > 0",
        ("proofs_replication", "lemma_marker_step_at"),
    ),
    (
        "maximum committed transaction timestamp replaces a closed frontier",
        "normal.rs",
        "Report { shard: i, epoch, through: frontier(s.logs[i].durable, epoch), closed:",
        "Report { shard: i, epoch, through: clock_floor(s.logs[i].durable), closed:",
        ("proofs_replication", "lemma_reports_step"),
    ),
    (
        "delayed report is relabeled with the receiver's current epoch",
        "normal.rs",
        "s.views.insert((observer, r.epoch, r.shard),",
        "s.views.insert((observer, s.shards[observer].epoch, r.shard),",
        ("proofs_replication", "lemma_reports_step"),
    ),
    (
        "epoch closure uses log maximum instead of finite certified cutoff",
        "recovery.rs",
        "let cut = frontier(entries(s.logs[shard]), epoch);",
        "let cut = clock_floor(entries(s.logs[shard]));",
        ("proofs_replication", "lemma_close_step_at"),
    ),
    (
        "range mirror retains destination-only keys",
        "sharding_mirror.rs",
        "else { destination.remove(k) }",
        "else { destination }",
        ("sharding_mirror", "lemma_mirror_point"),
    ),
    (
        "migrating transactions accept stale physical observations",
        "sharding_transactions.rs",
        "&&& forall|k: int| r.reads.dom().contains(k) ==>\n"
        "        p::read(s.placement, txn, k) == Some(#[trigger] r.reads[k])",
        "&&& true",
        ("sharding_transactions", "lemma_validated"),
    ),
    (
        "frozen source admits new access leases",
        "sharding_placement.rs",
        "&& replica(s,grant.owner,key).role is Serving",
        "&& (replica(s,grant.owner,key).role is Serving || replica(s,grant.owner,key).role is Frozen)",
        ("sharding_placement::proofs", "frozen_rejects_admission"),
    ),
    (
        "old background copy overwrites final-round data",
        "sharding_placement.rs",
        "&& r.round == packet.round",
        "",
        ("sharding_placement::proofs::keys", "key_copy_delivery"),
    ),
    (
        "destination seals without complete copy coverage",
        "sharding_placement.rs",
        "r.round == 1 && r.covered",
        "r.round == 1",
        ("sharding_placement::proofs::keys", "key_seal"),
    ),
    (
        "old abort clears a newer generation",
        "sharding_placement.rs",
        "&& (r.fence < g || r.fence == g && !r.terminal)",
        "&& true",
        ("sharding_placement::proofs", "old_command_rejected"),
    ),
]

# A mutation failure is evidence only if the unchanged complete crate passes.
# Exercise the public entry point, rather than maintaining a second baseline.
try:
    baseline = subprocess.run(
        [str(ROOT / "scripts" / "verify.sh")],
        cwd=ROOT, env={**os.environ, "VERUS_PATH": VERUS},
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1800,
    )
except subprocess.TimeoutExpired:
    raise SystemExit("Unchanged MakoV2 verification timed out; controls were not run")
if baseline.returncode != 0:
    raise SystemExit(f"Unchanged MakoV2 verification failed; controls were not run\n{baseline.stdout}")
print(baseline.stdout, end="", flush=True)

# Isolate each mutant's safety obligation. Unrelated constructive
# traces or SMT resource failures must not masquerade as rejecting the bug.
for name, filename, before, after, (module, function) in CONTROLS:
    with tempfile.TemporaryDirectory(prefix="makov2-proof-control-") as directory:
        dest = Path(directory)
        shutil.copytree(ROOT / "src", dest / "src")
        path = dest / "src" / filename
        text = path.read_text()
        if text.count(before) != 1:
            raise SystemExit(f"{name}: mutation site is not unique; no control was run")
        path.write_text(text.replace(before, after))
        try:
            result = subprocess.run(
                [VERUS, *SYSROOT_ARGS, "--crate-type=lib", "src/lib.rs", "--no-cheating",
                 "--num-threads", "4", "--triggers-mode", "silent",
                 "--verify-only-module", module, "--verify-function", function],
                cwd=dest, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, timeout=1800,
            )
        except subprocess.TimeoutExpired:
            raise SystemExit(f"{name}: timeout is not a semantic proof rejection")
        output = result.stdout
        summary = re.search(r"verification results:: (\d+) verified, (\d+) errors", output)
        proof_failure = any(message in output for message in (
            "assertion failed", "postcondition not satisfied", "precondition not satisfied",
        ))
        invalid_run = any(message in output for message in (
            "error[E", "Resource limit", "resource limit", "internal compiler error",
        ))
        if (result.returncode != 1 or summary is None or int(summary[2]) == 0
                or not proof_failure or invalid_run):
            raise SystemExit(f"{name}: expected a semantic proof rejection\n{output}")
        print(f"PASS: Verus rejects {name} in {module}::{function} "
              f"({summary[2]} failed obligations)", flush=True)
