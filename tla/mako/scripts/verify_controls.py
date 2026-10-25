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
        "epoch point admission ignores receiver owner",
        "src/cluster/epoch_admission.rs",
        "if route.grant.owner != self.owner {return Status::Retry;}\n        let table =",
        "let table =",
        ("epoch_admission", "AdmissionSnapshot::point"),
    ),
    (
        "epoch point admission permits Frozen authority",
        "src/cluster/epoch_admission.rs",
        "Some(meta) if matches!(meta.role,Role::Serving) && meta.ownership_version==route.grant.ownership_version",
        "Some(meta) if meta.ownership_version==route.grant.ownership_version",
        ("epoch_admission", "AdmissionSnapshot::point"),
    ),
    (
        "epoch point admission ignores ownership incarnation",
        "src/cluster/epoch_admission.rs",
        "Some(meta) if matches!(meta.role,Role::Serving) && meta.ownership_version==route.grant.ownership_version",
        "Some(meta) if matches!(meta.role,Role::Serving)",
        ("epoch_admission", "AdmissionSnapshot::point"),
    ),
    (
        "epoch range admission ignores receiver owner",
        "src/cluster/epoch_admission.rs",
        "if route.grant.owner != self.owner {return Status::Retry;}\n        let metadata =",
        "let metadata =",
        ("epoch_admission", "AdmissionSnapshot::range"),
    ),
    (
        "epoch range admission omits complete captured interval",
        "src/cluster/epoch_admission.rs",
        "if metadata.serving_range(lo,hi,route.grant.ownership_version) {Status::Ok}",
        "if true {Status::Ok}",
        ("epoch_admission", "AdmissionSnapshot::range"),
    ),
    (
        "native commit reuses a published version for changed routes",
        "src/cluster/migration.rs",
        "published_version: if r is Commit { b.records[i as int].plan.generation } else { b.published_version },",
        "published_version: b.published_version,",
        ("migration_invariants", "request_preserves"),
    ),
    (
        "direct native exhaustion silently omits a callback row",
        "src/cluster/full_scan_proofs.rs",
        "EngineExit::Exhausted => d.delivered.len() == d.raw.len()",
        "EngineExit::Exhausted => d.delivered.len() <= d.raw.len()",
        ("full_scan_proofs", "direct_engine_delivery"),
    ),
    (
        "exact mirror elides a changed value merely because identity matches",
        "src/cluster/storage.rs",
        "comparison == 0 && compare(&row.value,&destination.value) == 0",
        "comparison == 0",
        ("storage", "mirror"),
    ),
    (
        "exact mirror elides a missing insertion merely because another row has equal bytes",
        "src/cluster/storage.rs",
        "comparison == 0 && compare(&row.value,&destination.value) == 0",
        "compare(&row.value,&destination.value) == 0",
        ("storage", "mirror"),
    ),
    (
        "exact mirror preserves a destination-only row instead of deleting it",
        "src/cluster/storage.rs",
        "let status = store.delete(plan,owner,&row,false);",
        "let status = Status::Ok;",
        ("storage", "mirror"),
    ),
    (
        "private precopy bypasses its full metadata envelope and round guard",
        "src/cluster/transfer.rs",
        "if !node.background_destination(plan) { return Status::Retry; }",
        "if false { return Status::Retry; }",
        ("transfer", "precopy_put"),
    ),
    (
        "private precopy mutates a coordinate outside the selected window",
        "src/cluster/transfer.rs",
        "if !crate::bytes::contains(&plan.range,plan.range.table,&row.coordinate) { return Status::Invalid; }",
        "if false { return Status::Invalid; }",
        ("transfer", "precopy_put"),
    ),
    (
        "native frozen capture bypasses full Frozen metadata authorization",
        "src/cluster/transfer.rs",
        "if !node.frozen_source(plan) { return Err(Status::Retry); }",
        "if false { return Err(Status::Retry); }",
        ("transfer", "capture_frozen"),
    ),
    (
        "private precopy authorizes a destination whose Final round is already installed",
        "src/cluster/participant.rs",
        "self.envelope(plan) && self.owner==plan.destination && self.all_role(plan,Role::Stage,Some(0),false)",
        "self.envelope(plan) && self.owner==plan.destination && self.all_role(plan,Role::Stage,Some(1),false)",
        ("participant", "Participant::background_destination"),
    ),
    # Check the direct fence lemma, not a caller which could trust that mutated
    # lemma's unverified postcondition under a negative-only function filter.
    (
        "private precopy authority forgets the installed generation fence",
        "src/cluster/transfer.rs",
        "node.envelope_view(plan) && node.owner_view() == plan.destination\n        && node.range_role(plan,Role::Stage,Some(0),false)",
        "node.envelope_view(plan) && node.owner_view() == plan.destination",
        ("transfer", "precopy_fenced_at"),
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
    (
        "commit-time Freeze fails to change the authority epoch",
        "sharding_commit_validation_authority.rs",
        "Snapshot { epoch: s.epoch + 1, serving, fenced, ..s }",
        "Snapshot { epoch: s.epoch, serving, fenced, ..s }",
        ("sharding_commit_validation::authority", "mutation_invalidates"),
    ),
    (
        "operational Freeze retains a transaction's fast epoch",
        "sharding_commit_validation.rs",
        "Action::Freeze { .. } => State {\n"
        "            serving: s.serving.insert(s.movement.unwrap().source.owner,None),\n"
        "            epochs: s.epochs.insert(s.movement.unwrap().source.owner,s.epochs[s.movement.unwrap().source.owner] + 1),",
        "Action::Freeze { .. } => State {\n"
        "            serving: s.serving.insert(s.movement.unwrap().source.owner,None),",
        ("sharding_commit_validation::proofs", "migration_preserves"),
    ),
    (
        "commit-time fast check accepts changed authority",
        "sharding_commit_validation_authority.rs",
        "&& (old.epoch == now.epoch || exact(now,owner,footprint))",
        "&& (true || exact(now,owner,footprint))",
        ("sharding_commit_validation::authority", "validation_establishes_authority"),
    ),
    (
        "commit-time authority aliases a restarted incarnation",
        "sharding_commit_validation_authority.rs",
        "!now.fenced && old.incarnation == now.incarnation",
        "!now.fenced",
        ("sharding_commit_validation::authority", "validation_establishes_authority"),
    ),
    (
        "commit-time scan footprint checks only its first coordinate",
        "sharding_commit_validation_authority.rs",
        "Map::new(|k: Key| interval(table,lo,hi).contains(k), |k: Key| routes[k])",
        "Map::new(|k: Key| interval(table,lo,hi).contains(k) && k.1 == lo, |k: Key| routes[k])",
        ("sharding_commit_validation::authority", "scan_checks_every_coordinate"),
    ),
    (
        "ownership scope omits warehouse row aliases",
        "sharding_commit_validation_authority.rs",
        "Map::new(|k: Key| owned.contains_key((k.0,k.1)), |k: Key| owned[(k.0,k.1)])",
        "Map::new(|k: Key| owned.contains_key((k.0,k.1)) && k.2 == 0, |k: Key| owned[(k.0,k.1)])",
        ("sharding_commit_validation::authority", "every_row_alias_is_covered"),
    ),
    (
        "directory publication moves only one row alias",
        "sharding_commit_validation_proofs.rs",
        "if keys.contains(k) { apply(s[k],Action::Publish { generation }) } else { s[k] }",
        "if keys.contains(k) && k.2 == 0 { apply(s[k],Action::Publish { generation }) } else { s[k] }",
        ("sharding_commit_validation::proofs", "whole_scope_publication"),
    ),
    (
        "new migration reuses an obsolete generation",
        "sharding_commit_validation.rs",
        "&& generation >= s.next_generation",
        "&& true",
        ("sharding_commit_validation::proofs", "migration_preserves"),
    ),
    (
        "unchanged epoch authorizes an initially Frozen footprint",
        "sharding_commit_validation_authority.rs",
        "exact(h[i],owner,footprint), accepted(h[i],h[j],owner,footprint)",
        "accepted(h[i],h[j],owner,footprint)",
        ("sharding_commit_validation::authority", "validation_establishes_authority"),
    ),
    (
        "commit-time validation bypasses Frozen authority",
        "sharding_commit_validation.rs",
        "&& (s.epochs[s.txns[id].grant.owner] == s.txns[id].epoch\n"
        "            || s.serving[s.txns[id].grant.owner] == Some(s.txns[id].grant.version))",
        "&& true",
        ("sharding_commit_validation::proofs", "transaction_preserves"),
    ),
    (
        "commit-time validation starts before its write intent is locked",
        "sharding_commit_validation.rs",
        "&& (!s.txns[id].writes || s.txns[id].locked)",
        "&& true",
        ("sharding_commit_validation::proofs", "transaction_preserves"),
    ),
    (
        "writer unlocks before its logical effects settle",
        "sharding_commit_validation.rs",
        "&& s.txns[id].locked && effects_settled(s.txns[id]),",
        "&& s.txns[id].locked,",
        ("sharding_commit_validation::proofs", "transaction_preserves"),
    ),
    (
        "lease-free copy ignores a locked absent-key intent",
        "sharding_commit_validation.rs",
        "&& copy_conflict_free(s)",
        "&& true",
        ("sharding_commit_validation::proofs", "copy_preserves"),
    ),
    (
        "lease-free copy retains destination-only values",
        "sharding_commit_validation.rs",
        "data: s.data.insert(m.destination,s.data[m.source.owner])",
        "data: if s.data[m.source.owner].value is Some { "
        "s.data.insert(m.destination,s.data[m.source.owner]) } else { s.data }",
        ("sharding_commit_validation::proofs", "copy_preserves"),
    ),
    (
        "lease-free retirement omits copy coverage",
        "sharding_commit_validation.rs",
        "&& s.movement.unwrap().phase is Frozen && s.movement.unwrap().copied",
        "&& s.movement.unwrap().phase is Frozen",
        ("sharding_commit_validation::proofs", "migration_preserves"),
    ),
    (
        "lease-free reclamation ignores outstanding storage references",
        "sharding_commit_validation.rs",
        "&& pins_clear(s,s.movement.unwrap().source.owner)",
        "&& true",
        ("sharding_commit_validation::proofs", "migration_preserves"),
    ),
    (
        "lease-free handoff accepts a delayed older control",
        "sharding_commit_validation.rs",
        "s.movement is Some && s.movement.unwrap().generation == generation",
        "s.movement is Some",
        ("sharding_commit_validation::proofs", "stale_control_rejected"),
    ),
    (
        "sealed transaction adds a late read",
        "sharding_commit_validation.rs",
        "Action::Observe { id } => live(s,id) && !s.txns[id].sealed,",
        "Action::Observe { id } => live(s,id),",
        ("sharding_commit_validation::proofs", "sealed_rejects_late_access"),
    ),
    (
        "epoch enrollment omits the announcement recheck",
        "sharding_epoch_admission.rs",
        "&& s.readers[id].epoch == s.epoch,",
        "&& true,",
        ("sharding_epoch_admission::proofs", "validate_preserves"),
    ),
    (
        "epoch cache reuses an object from an obsolete entry epoch",
        "sharding_epoch_admission.rs",
        "&& s.caches[r.worker].epoch == r.epoch",
        "",
        ("sharding_epoch_admission::proofs", "capture_provenance"),
    ),
    (
        "engine retry advances a retained old-view pin",
        "sharding_epoch_admission.rs",
        "epoch: s.readers[id].epoch,",
        "epoch: s.epoch,",
        ("sharding_epoch_admission::proofs", "retry_preserves"),
    ),
    (
        "epoch completion skips outstanding old announcements",
        "sharding_epoch_admission.rs",
        "&& drained(s,cut),",
        "&& true,",
        ("sharding_epoch_admission::proofs", "complete_preserves"),
    ),
    (
        "worker slot reports the newest rather than oldest retained guard",
        "sharding_epoch_admission.rs",
        "rest.unwrap() < s.readers[id].epoch",
        "rest.unwrap() > s.readers[id].epoch",
        ("sharding_epoch_admission::proofs", "minimum_epoch_characterization"),
    ),
    (
        "epoch-cached scan certification omits physical row aliases",
        "sharding_epoch_admission.rs",
        "IMap::new(|key: Key| interval(table,lo,hi).contains(key), |key: Key| routes[key])",
        "IMap::new(|key: Key| interval(table,lo,hi).contains(key) && key.2 == 0, |key: Key| routes[key])",
        ("sharding_epoch_admission::proofs", "interval_certificate_covers_absence_and_aliases"),
    ),
    (
        "migration reuses a cut established before Freeze publication",
        "sharding_epoch_drain.rs",
        "cut: Some(s.local[m.source.owner].epoch + 1)",
        "cut: Some(s.local[m.source.owner].epoch)",
        ("sharding_epoch_drain::proofs", "cut_follows_freeze_publication"),
    ),
    (
        "epoch-drained copy starts before its source barrier completes",
        "sharding_epoch_drain.rs",
        "&& s.local[s.movement.unwrap().source.owner].completed >= s.movement.unwrap().cut.unwrap(),",
        "&& true,",
        ("sharding_epoch_drain::proofs", "copy_waits_for_real_drain"),
    ),
    (
        "pending engine effects release their epoch protection",
        "sharding_epoch_drain.rs",
        "!s.txns.contains_key(id) || s.txns[id].grant.owner != node || s.txns[id].cleaned",
        "true",
        ("sharding_epoch_drain::proofs", "pin_covers_effects_and_cleanup"),
    ),
    (
        "settled engine effects release protection before cleanup",
        "sharding_epoch_drain.rs",
        "!s.txns.contains_key(id) || s.txns[id].grant.owner != node || s.txns[id].cleaned",
        "!s.txns.contains_key(id) || s.txns[id].grant.owner != node || !(s.txns[id].outcome is Pending)",
        ("sharding_epoch_drain::proofs", "exit_requires_terminal_cleanup"),
    ),
    (
        "epoch-drained copy preserves stale destination contents",
        "sharding_epoch_drain.rs",
        "State { data: s.data.insert(m.destination,s.data[m.source.owner]),",
        "State { data: s.data.insert(m.destination,s.data[m.destination]),",
        ("sharding_epoch_drain::proofs", "exact_copy_covers_absence"),
    ),
    (
        "epoch-drained enrollment checks only the first row",
        "sharding_epoch_drain.rs",
        "forall|k: Key| #[trigger] c.scope.contains(k) ==> admission::authorized(s.local[node],id,k,grant)",
        "admission::authorized(s.local[node],id,c.anchor,grant)",
        ("sharding_epoch_drain::proofs", "enrollment_covers_entire_scope"),
    ),
    (
        "epoch-drained control accepts an obsolete migration generation",
        "sharding_epoch_drain.rs",
        "s.movement is Some && s.movement.unwrap().generation == generation",
        "s.movement is Some",
        ("sharding_epoch_drain::proofs", "stale_control_rejected"),
    ),
    (
        "delayed participant enrollment bypasses its captured authority",
        "sharding_epoch_drain.rs",
        "&& scope_authorized(c,s,node,id,grant),",
        "&& true,",
        ("sharding_epoch_drain::proofs", "late_old_request_rejected"),
    ),
    (
        "epoch-drained publication skips exact-copy completion",
        "sharding_epoch_drain.rs",
        "&& s.movement.unwrap().phase is Copied,",
        "&& (s.movement.unwrap().phase is Frozen || s.movement.unwrap().phase is Copied),",
        ("sharding_epoch_drain::proofs", "publication_preserves_image"),
    ),
    (
        "private precopy accepts an obsolete generation",
        "sharding_epoch_drain.rs",
        "s.movement is Some && p.generation == s.movement.unwrap().generation",
        "s.movement is Some",
        ("sharding_epoch_drain::precopy", "accepted_packet_is_current"),
    ),
    (
        "private precopy accepts an obsolete round",
        "sharding_epoch_drain.rs",
        "&& p.round == s.movement.unwrap().round",
        "",
        ("sharding_epoch_drain::precopy", "accepted_packet_is_current"),
    ),
    (
        "private precopy admits effects after closure",
        "sharding_epoch_drain.rs",
        "&& s.movement.unwrap().precopy_open && precopy_phase(s.movement.unwrap())\n"
        "            && !s.effects.contains(id),",
        "&& precopy_phase(s.movement.unwrap()) && !s.effects.contains(id),",
        ("sharding_epoch_drain::precopy", "accepted_packet_is_current"),
    ),
    (
        "precopy join ignores outstanding storage effects",
        "sharding_epoch_drain.rs",
        "&& !s.movement.unwrap().precopy_open && s.effects.is_empty(),",
        "&& !s.movement.unwrap().precopy_open,",
        ("sharding_epoch_drain::precopy", "join_waits_for_effects"),
    ),
    (
        "exact final overtakes unjoined precopy",
        "sharding_epoch_drain.rs",
        "&& s.movement.unwrap().precopy_joined\n"
        "            && s.local[s.movement.unwrap().source.owner].completed",
        "&& s.local[s.movement.unwrap().source.owner].completed",
        ("sharding_epoch_drain::precopy", "final_requires_join"),
    ),
    (
        "shared process projection permits a younger physical guard",
        "sharding_epoch_drain_projection.rs",
        "physical.readers[embedding[(owner,id)]].epoch <= owners[owner].readers[id].epoch",
        "physical.readers[embedding[(owner,id)]].epoch >= owners[owner].readers[id].epoch",
        ("sharding_epoch_drain::projection", "every_colocated_guard_bounds_slot"),
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
