#!/usr/bin/env python3
"""Require semantic proof failures when native or independent invariants weaken.

Run in the same container as verify.sh. Each mutant must fail its named
obligation after the unchanged complete crate passes; --native selects the
same-source production entry point and its correspondence controls.
"""
from pathlib import Path
import os
import argparse
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
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--native", action="store_true")
NATIVE = parser.parse_args().native
REPOSITORY = ROOT.parents[1]

NATIVE_CONTROLS = [
    (
        "canonical byte encoding forgets byte contents",
        "tla/mako/src/sharding_bytes.rs",
        "256 * value_code(value.drop_last()) + value.last() as int",
        "256 * value_code(value.drop_last())",
        ("sharding_bytes", "value_code_injective"),
    ),
    (
        "unknown callback authorizes transaction completion",
        "src/cluster/source_engine_history.rs",
        "EngineRecord::Completion { id,outcome } => if outcome is Unknown { s }",
        "EngineRecord::Completion { id,outcome } => if outcome is Unknown { "
        "SourceState { completed:s.completed.insert(id),..s } }",
        ("execution_refinement::source_execution::causality", "unknown_callback_cannot_complete"),
    ),
    (
        "registration discards another owner's held keys",
        "src/cluster/transfer_lease_execution.rs",
        "held:overlay(s.sessions[transaction(id)].held,target,target.dom()),resolved:false",
        "held:target,resolved:false",
        ("execution_refinement::lease_execution", "registered"),
    ),
    (
        "private mirror bytes bypass source scan provenance",
        "src/cluster/source_transfer_history.rs",
        "&& s.private_packets.contains(self.packet) && self.packet.0==self.plan.generation",
        "&& self.packet.0==self.plan.generation",
        ("execution_refinement::source_execution::causality", "private_copy_origin"),
    ),
    (
        "durable system keys alias different shard owners",
        "src/cluster/durable_kv.rs",
        "word64(KEY_TAG) + word32(ns.owner)",
        "word64(KEY_TAG) + word32(0)",
        ("durable_kv", "key_injective"),
    ),
    (
        "unknown durable batch permits a partial or unrelated effect",
        "src/cluster/durable_kv.rs",
        "Some(batch) => after == before || after == apply_batch(before,batch)",
        "Some(batch) => true",
        ("durable_kv", "batch_settlement"),
    ),
    (
        "native authority delivery retains its fast certificate",
        "src/cluster/participant.rs",
        "self.invalidate_snapshot();\n        if !self.envelope(plan) {",
        "if !self.envelope(plan) {",
        ("participant", "Participant::deliver"),
    ),
    (
        "native snapshot certification skips local interval validation",
        "src/cluster/participant_admission.rs",
        "if !local.serving_range(&boundary.start,hi,boundary.grant.ownership_version) { return false; }",
        "",
        ("participant::admission", "Participant::certify_snapshot"),
    ),
    (
        "native point admission treats a stale version as certified",
        "src/cluster/participant_admission.rs",
        "if self.fast_version != Some(r.snapshot_version) || r.snapshot_version == NO_SNAPSHOT {\n"
        "            return self.acquire(id,table,coordinate,r.grant);",
        "if r.snapshot_version == NO_SNAPSHOT {\n"
        "            return self.acquire(id,table,coordinate,r.grant);",
        ("participant::admission", "Participant::acquire_routed"),
    ),
    (
        "native commit reuses a published version for changed routes",
        "src/cluster/migration.rs",
        "published_version: if r is Commit { b.records[i as int].plan.generation } else { b.published_version },",
        "published_version: b.published_version,",
        ("migration_invariants", "request_preserves"),
    ),
]

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
    (
        "restarted authority accepts a stale admission incarnation",
        "sharding_recovery.rs",
        "registered(s,a) && head(s,a).incarnation == incarnation",
        "registered(s,a)",
        ("sharding_recovery::proofs", "theorem_stale_incarnation"),
    ),
    (
        "collector discards an uncheckpointed recovery dependency",
        "sharding_retention.rs",
        "&& d.records[client].generation <= d.recovery_floor",
        "&& true",
        ("sharding_retention", "theorem_no_active_or_checkpoint_collection"),
    ),
    (
        "authority mutation retains a matching fast certificate",
        "sharding_fast_path.rs",
        "let certified = if changes_authority(a) { Map::empty() } else { s.certified };",
        "let certified = s.certified;",
        ("sharding_fast_path::proofs", "placement_preserves_certificate"),
    ),
    (
        "receiver certifies without checking local Serving authority",
        "sharding_fast_path.rs",
        "certified: if certifiable(c,s.placement,owner,snapshot) {",
        "certified: if true {",
        ("sharding_fast_path::proofs", "certify_preserves_authority"),
    ),
    (
        "capture relabels the current grant with an older snapshot",
        "sharding_fast_path.rs",
        "grant: s.placement.directory[version as int][representative], snapshot: Some(version)",
        "grant: p::directory(s.placement)[representative], snapshot: Some(version)",
        ("sharding_fast_path::proofs", "capture_preserves_pair"),
    ),
]

# A mutation failure is evidence only if the unchanged complete crate passes.
# Exercise the public entry point, rather than maintaining a second baseline.
try:
    baseline_command = (
        [VERUS, *SYSROOT_ARGS, "--crate-type=lib", "--edition=2021",
         "src/cluster/lib.rs", "--no-cheating", "--num-threads", "8", "--triggers-mode", "silent"]
        if NATIVE else [str(ROOT / "scripts" / "verify.sh")]
    )
    baseline = subprocess.run(
        baseline_command,
        cwd=REPOSITORY if NATIVE else ROOT, env={**os.environ, "VERUS_PATH": VERUS},
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1800,
    )
except subprocess.TimeoutExpired:
    raise SystemExit("Unchanged crate verification timed out; controls were not run")
if baseline.returncode != 0:
    raise SystemExit(f"Unchanged crate verification failed; controls were not run\n{baseline.stdout}")
print(baseline.stdout, end="", flush=True)

# Isolate each mutant's safety obligation. Unrelated constructive
# traces or SMT resource failures must not masquerade as rejecting the bug.
for name, filename, before, after, (module, function) in (NATIVE_CONTROLS if NATIVE else CONTROLS):
    with tempfile.TemporaryDirectory(prefix="mako-proof-control-") as directory:
        dest = Path(directory)
        if NATIVE:
            shutil.copytree(REPOSITORY / "src" / "cluster", dest / "src" / "cluster")
            shutil.copytree(ROOT / "src", dest / "tla" / "mako" / "src")
            path = dest / filename
        else:
            shutil.copytree(ROOT / "src", dest / "src")
            path = dest / "src" / filename
        text = path.read_text()
        if text.count(before) != 1:
            raise SystemExit(f"{name}: mutation site is not unique; no control was run")
        path.write_text(text.replace(before, after))
        try:
            result = subprocess.run(
                [VERUS, *SYSROOT_ARGS, "--crate-type=lib", "--edition=2021",
                 "src/cluster/lib.rs" if NATIVE else "src/lib.rs", "--no-cheating",
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
            "invariant not satisfied",
        ))
        invalid_run = any(message in output for message in (
            "error[E", "Resource limit", "resource limit", "internal compiler error",
        ))
        if (result.returncode != 1 or summary is None or int(summary[2]) == 0
                or not proof_failure or invalid_run):
            raise SystemExit(f"{name}: expected a semantic proof rejection\n{output}")
        print(f"PASS: Verus rejects {name} in {module}::{function} "
              f"({summary[2]} failed obligations)", flush=True)
