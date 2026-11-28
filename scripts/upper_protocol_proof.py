#!/usr/bin/env python3
"""Source correspondence audit and semantic controls for the production core.

The native adapters remain trusted. This checks their entry points, not their
semantics. See docs/developer/upper-layer-contracts.md for that boundary.
"""
from pathlib import Path
import re
import shutil
import tempfile
import tomllib
import shard_host_core_proof as host

ROOT = host.ROOT
CORE = ROOT / 'crates/sto-core/src/protocol/verified'
LEGACY = ROOT / 'crates/sto-core/verification'
RETENTION = ROOT / 'src/deptran/raft/core/src/retention.rs'
ENTRY = 'crates/sto-core/src/protocol/verified/mod.rs'
LEGACY_ENTRY = 'crates/sto-core/verification/legacy.rs'
RETENTION_ENTRY = str(RETENTION.relative_to(ROOT))
ADAPTERS = {
    'crates/mako-runtime/src/gateway_protocol.rs': ['core::Request', 'core::COMMITTED'],
    'crates/mako-runtime/src/migration_ranges.rs': ['impl contract::RangeLocks for Provider', 'impl contract::RangeGuard for Guard', 'self.inner.enter()', 'active.io.point(', 'active.io.range('],
    'crates/mako-runtime/src/migration_storage.rs': ['crate::migration_ranges::scan(', 'crate::migration_ranges::point(', 'commit::coordinate(&mut effects)'],
    'src/cluster/runtime.rs': ['self.host.acquire_range(plan)', 'self.host.reset_ranges()', 'guard.drained()', 'guard.finish()'],
    'crates/mako-rocksdb/src/runtime.rs': ['client.begin(', 'tx.get_routed(', 'tx.mutate_routed(', 'tx.scan_routed(', 'tx.commit()', 'client::resolve_transaction('],
    'crates/sto-masstree-api/src/native.rs': [
        'self.worker.phase(ParticipantPhase::Collecting)',
        'p.lock_writes()', 'p.validate()', 'p.install(time)',
        'self.worker.phase(ParticipantPhase::Validated)', 'self.worker.protocol.uncertain = true'],
    'crates/sto-masstree/src/lib.rs': ['impl TObject for Adapter', 'cx.try_lock(', 'self.check_native(',
        '.install(', 'fn intents(', 'txn.visit_items('],
    'src/mako/storage/mtree_rows.cc': ['mako_row_read(', 'mako_row_check(', 'mako_row_lock(',
        'mako_write_prepare(', 'mako_row_install(', 'mako_row_unlock('],
    'crates/masstree/src/native_rows.rs': ['mako_rows_abi()', 'mako_row_read(',
        'mako_row_lock(', 'mako_write_prepare(', 'mako_row_install(', 'mako_row_unlock('],
    'crates/sto-core/src/protocol/admission.rs': [
        'core::admit(', 'core::claim(', 'core::completed(',
        'self.last.as_ref() == Some(request) && self.response.is_some()'],
    'crates/sto-core/src/protocol/participant.rs': ['crate::protocol::verified::admission::release_protection(', 'self.engine.execute(', 'self.engine.retain_uncertain()'],
    'crates/mako-runtime/src/receiver.rs': ['Lane::new(', 'prepared::transaction(&mut lane, request)', 'entry.unresolved()'],
    'crates/mako-runtime/src/prepared.rs': ['impl ParticipantEngine for NativeEngine', 'core::begin_prepare(', 'core::prepared(', 'core::finish(', 'Record::Prepare', 'Record::Complete', 'participant.prepared_rows('],
    'crates/mako-storage-masstree/src/lib.rs': ['impl api::ParticipantBackend for Participant', 'self.0.prepared_rows(', 'self.0.validate()', 'self.0.install('],
    'crates/sto-core/src/protocol/commit.rs': ['Coordinator::new(', 'coordinator.record_locked(', 'coordinator.record_prepared(', 'coordinator.record_decision(', 'coordinator.acknowledge('],
    'crates/mako-runtime/src/rpc.rs': ['impl sto_core::protocol::contracts::Transport for Network'],
    'crates/mako-runtime/src/client.rs': ['remote::Remotes', 'impl Effects for NativeEffects', 'commit::coordinate(&mut effects)', 'TransactionAction::Lock', 'TransactionAction::Prepare', 'Record::Begin', 'Record::Decide'],
    'crates/mako-runtime/src/gateway.rs': ['client.run(', 'transaction.gateway_receipt('],
    'crates/mako-runtime/src/log.rs': ['impl<C: Into<Mode>> Journal for Log<C>', 'Mode::Local', 'Mode::Raft', 'journal::transition(', 'binding.host.propose(', 'ProposalStatus::Applied', 'journal::acknowledge(', 'recovery::materialized_replay(', 'recovery::materialized_checkpoint(', 'self.flush_locked('],
    'crates/mako-runtime/src/log/materialization.rs': ['recovery::materialization_batch(', 'application.apply_batches(', 'gate.materialization.clear()'],
    'crates/mako-runtime/src/application.rs': ['result.validate(owner, durable)?', 'journal::transition('],
    'crates/sto-rocksdb/src/checkpoint.rs': ['rocks.checkpoint(', 'sync_tree(', 'crate::directory::publish(', 'file.sync_all()', 'fs::rename('],
    'crates/sto-rocksdb/src/directory.rs': ['file.sync_all()', 'fs::rename(', 'publish_link('],
    'crates/raft-host/src/durable.rs': ['self.log.journal.persist(raw)?'],
    'crates/raft-host/src/local.rs': ['disk.journal.persist(', 'ProposalStatus::Applied', 'self.0.machine.apply(', 'raft_core::retention::may_trim('],
    'crates/mako-runtime/src/gateway_client.rs': ['crate::gateway_protocol::GatewayClient'],
}

def audit_file(path, source):
    code = host.core_code(source)
    host.audit_core_tokens(path.with_name('lib.rs') if path.name == 'mod.rs' else path, code)
    checked = code.replace('#![forbid(unsafe_code)]', '')
    if re.search(r'verifier::|\bassume\s*\(|(?<!fn )\badmit\s*\(|\bassume_specification\b|\bexternal|\brlimit\b|\bunsafe', checked):
        raise SystemExit(f'{path}: verifier escape')
    if path.name == 'mod.rs':
        if re.sub(r'pub mod \w+;', '', checked).strip():
            raise SystemExit(f'{path}: executable code outside verus!')
        return
    start = checked.find('verus! {')
    if start < 0 or re.sub(r'use [^;]+;', '', checked[:start]).strip():
        raise SystemExit(f'{path}: executable code before verus!')
    depth = 0
    for index in range(checked.index('{', start), len(checked)):
        depth += {'{':1, '}':-1}.get(checked[index], 0)
        if depth == 0:
            if checked[index+1:].strip():
                raise SystemExit(f'{path}: executable code after verus!')
            break
    else:
        raise SystemExit(f'{path}: unmatched verus! block')

def audit_configuration():
    crate = ROOT/'crates/sto-core'
    manifest = tomllib.loads((crate/'Cargo.toml').read_text())
    if manifest.get('lib') != {'path':'src/lib.rs'} or (crate/'build.rs').exists() or 'build' in manifest['package'] or 'target' in manifest:
        raise SystemExit('STO canonical source/build configuration changed')
    for name in ('vstd','verus_builtin','verus_builtin_macros'):
        if manifest['dependencies'][name] != {'version':'='+host.CRATES_VERSION, 'optional':True}:
            raise SystemExit(f'{name}: STO Verus pin/configuration drift')
        if 'dep:'+name not in manifest['features']['protocol']:
            raise SystemExit('STO protocol must use pinned Verus dependencies')
    root = host.core_code((crate/'src/lib.rs').read_text())
    parent = host.core_code((crate/'src/protocol/mod.rs').read_text())
    if not re.search(r'#\[cfg\(feature = ""\)\]\s*pub mod protocol;', root):
        raise SystemExit('STO production protocol module mapping changed')
    if '#[cfg(feature = "protocol")]\npub mod protocol;' not in (crate/'src/lib.rs').read_text():
        raise SystemExit('STO protocol feature mapping changed')
    declaration = re.search(r'((?:#\[[^\]]*\]\s*)*)pub mod protocol;', root)
    if not declaration or re.sub(r'\s+', '', declaration[1]) != '#[cfg(feature="")]':
        raise SystemExit('STO protocol declaration has alternate attributes')
    if root.count('mod protocol') != 1 or parent.count('pub mod verified;') != 1:
        raise SystemExit('STO verified module mapping changed')
    # Module declarations cannot replace the canonical files via cfg/path/include.
    expected = {'admission','commit','contracts','epoch','journal','participant','range_lock','redo','remote','wire','verified'}
    stripped = parent.replace('#![forbid(unsafe_code)]','')
    stripped = stripped.replace('#[allow(clippy::too_many_arguments)]\npub mod verified;', 'pub mod verified;')
    if set(re.findall(r'pub mod (\w+);',stripped)) != expected or re.sub(r'pub mod \w+;', '', stripped).strip():
        raise SystemExit('STO protocol module declarations changed')
    if 'mod coordinator' in parent or 'legacy_coordinator' in root:
        raise SystemExit('Historical coordinator reentered production')


def audit():
    audit_configuration()
    files = sorted(CORE.glob('*.rs'))
    declared = set(re.findall(r'pub mod (\w+);', (CORE/'mod.rs').read_text()))
    if declared != {p.stem for p in files if p.name != 'mod.rs'}:
        raise SystemExit('Core module graph mismatch')
    for path in files:
        audit_file(path, path.read_text())
    audit_file(LEGACY/'legacy_coordinator.rs', (LEGACY/'legacy_coordinator.rs').read_text())
    audit_file(RETENTION, RETENTION.read_text())
    legacy_root = host.core_code((LEGACY/'legacy.rs').read_text()).strip()
    if legacy_root != '#[path = ""]\npub mod coordinator;' or '#[path = "legacy_coordinator.rs"]' not in (LEGACY/'legacy.rs').read_text():
        raise SystemExit('Historical proof target mapping changed')
    raft_root = host.core_code((RETENTION.parent/'lib.rs').read_text())
    if not re.search(r'(?:^|;)\s*pub mod retention;', raft_root) or raft_root.count('mod retention') != 1:
        raise SystemExit('Raft production retention mapping changed')
    # Audit controls use the same parser as the unchanged crate.
    path = CORE/'admission.rs'
    for bad in ('#[cfg(verus_keep_ghost)]\n', '#[verifier::external_body]\n'):
        try:
            audit_file(path, path.read_text().replace('pub fn active(', bad+'pub fn active(', 1))
        except SystemExit:
            pass
        else:
            raise SystemExit('Core source audit accepted a proof bypass')
    for name, sites in ADAPTERS.items():
        source = (ROOT/name).read_text()
        for site in sites:
            if re.sub(r'\s+', '', site) not in re.sub(r'\s+', '', source):
                raise SystemExit(f'Trusted adapter mapping changed: {name}: {site}')
    for crate in ('shard-host-core',):
        deps = tomllib.loads((ROOT/'crates'/crate/'Cargo.toml').read_text())['dependencies']
        if deps['vstd'] != '='+host.CRATES_VERSION:
            raise SystemExit(f'{crate}: Verus pin drift')
    print('Protocol source audit: canonical verified modules, historical coordinator and Raft retention; explicit trusted adapter mappings; bypass controls rejected', flush=True)

# name, source, unique executable mutation, replacement, module, function.
CONTROLS = [
    ('materialization skips a log position', 'recovery.rs', '    materialized < u64::MAX && first == materialized + 1\n        && first <= last && last <= applied\n}', '    materialized < u64::MAX\n        && first <= last && last <= applied\n}', 'recovery', 'materialization_batch'),
    ('materialization publishes unapplied effects', 'recovery.rs', '    materialized < u64::MAX && first == materialized + 1\n        && first <= last && last <= applied\n}', '    materialized < u64::MAX && first == materialized + 1\n        && first <= last\n}', 'recovery', 'materialization_batch'),
    ('checkpoint accepts a lagging RocksDB image', 'recovery.rs', '    validated && materialized == applied\n}', '    validated && materialized <= applied\n}', 'recovery', 'materialized_checkpoint'),
    ('recovery accepts materialization beyond commitment', 'recovery.rs', '    if materialized <= committed { Some(materialized) } else { None }\n}', '    Some(materialized)\n}', 'recovery', 'materialized_replay'),
    ('checkpoint ignores row cut', 'recovery.rs', '    paused && row_cut == applied && metadata_cut == applied\n        && saved_prepares == required_prepares\n}', '    paused && metadata_cut == applied\n        && saved_prepares == required_prepares\n}', 'recovery', 'checkpoint_ready'),
    ('checkpoint forgets prepared obligations', 'recovery.rs', '    paused && row_cut == applied && metadata_cut == applied\n        && saved_prepares == required_prepares\n}', '    paused && row_cut == applied && metadata_cut == applied\n}', 'recovery', 'checkpoint_ready'),
    ('checkpoint runs across publication', 'recovery.rs', '    paused && row_cut == applied && metadata_cut == applied\n        && saved_prepares == required_prepares\n}', '    row_cut == applied && metadata_cut == applied\n        && saved_prepares == required_prepares\n}', 'recovery', 'checkpoint_ready'),
    ('trim precedes checkpoint publication', 'recovery.rs', '    files_validated && files_published && requested <= checkpoint\n}', '    files_validated && requested <= checkpoint\n}', 'recovery', 'may_trim'),
    ('recovery trusts lost RAM progress', 'recovery.rs', '    checkpoint\n}', '    previous_ram\n}', 'recovery', 'replay_start'),
    ('stale sequence executes', 'admission.rs', 'request.sequence <= prior.sequence', 'request.sequence < prior.sequence', 'admission', 'admit'),
    ('install bypasses validation', 'admission.rs', 'if matches!(operation, Operation::Install) && !matches!(phase, Phase::Sealed)', 'if false && matches!(operation, Operation::Install) && !matches!(phase, Phase::Sealed)', 'admission', 'admit'),
    ('unknown releases migration protection', 'admission.rs', 'matches!(phase, Phase::Committed | Phase::Aborted) }', 'matches!(phase, Phase::Committed | Phase::Aborted | Phase::Unknown) }', 'admission', 'release_protection'),
    ('failed install becomes aborted', 'admission.rs', 'Operation::Install => if success { Phase::Committed } else { Phase::Unknown }', 'Operation::Install => if success { Phase::Committed } else { Phase::Aborted }', 'admission', 'completed'),
    ('timestamp equals dependency', 'coordinator.rs', 'if proposal <= dependency {', 'if proposal < dependency {', 'coordinator', 'prepare'),
    ('local install failure permits retry', 'coordinator.rs', 'if effects.install_local() != OK {\n        effects.quarantine();\n        return UNKNOWN;', 'if effects.install_local() != OK {\n        effects.quarantine();\n        return RETRY;', 'coordinator', 'coordinate'),
    ('remote validation omitted', 'coordinator.rs', '    effects.validate_remote()\n}', '    OK\n}', 'coordinator', 'prepare'),
    ('coordinator rejects all available attempts', 'coordinator.rs', '    let prepared = prepare(effects);', '    return RETRY;\n    let prepared = prepare(effects);', 'coordinator', 'coordinate'),
    ('2PC skips global lock barrier', 'two_phase.rs', '|| !self.all_locked() { return false; }', '|| false { return false; }', 'two_phase', 'Coordinator::record_prepared'),
    ('2PC commits without every Yes vote', 'two_phase.rs', 'if matches!(decision, Decision::Commit) && (self.refused || !self.all_prepared()) { return false; }', 'if false { return false; }', 'two_phase', 'Coordinator::record_decision'),
    ('2PC treats unsaved decision as authoritative', 'two_phase.rs', 'if !saved || matches!(decision, Decision::Pending) { return false; }', 'if matches!(decision, Decision::Pending) { return false; }', 'two_phase', 'Coordinator::record_decision'),
    ('2PC changes a saved decision', 'two_phase.rs', 'if !matches!(self.decision, Decision::Pending) { return same_decision(self.decision, decision); }', 'if false { return same_decision(self.decision, decision); }', 'two_phase', 'Coordinator::record_decision'),
    ('2PC releases prepared protection', 'two_phase.rs', '{ !matches!(phase, Phase::Committed) && !matches!(phase, Phase::Aborted) }', '{ !matches!(phase, Phase::Committed | Phase::Prepared) && !matches!(phase, Phase::Aborted) }', 'two_phase', 'retains_protection'),
    ('2PC permits autonomous prepared abort', 'two_phase.rs', '{ matches!(phase, Phase::Executing) || matches!(phase, Phase::Locked) }', '{ matches!(phase, Phase::Executing | Phase::Prepared) || matches!(phase, Phase::Locked) }', 'two_phase', 'may_abort_locally'),
    ('2PC acknowledges unsaved prepare', 'two_phase.rs', '{ if matches!(phase, Phase::Preparing) && saved { Phase::Prepared } else { phase } }', '{ if matches!(phase, Phase::Preparing) { Phase::Prepared } else { phase } }', 'two_phase', 'prepared'),
    ('2PC rejects every prepared transaction', 'two_phase.rs', 'if !self.refused && self.all_prepared() { Decision::Commit } else { Decision::Abort }', 'Decision::Abort', 'two_phase', 'Coordinator::proposed'),
]

def controls(run_proof):
    for name, filename, before, after, module, function in CONTROLS:
        with tempfile.TemporaryDirectory(prefix='mako-upper-control-') as tmp:
            dest = Path(tmp)
            if filename == 'coordinator.rs':
                (dest/'src').mkdir()
                shutil.copy2(LEGACY/'legacy_coordinator.rs', dest/'src/coordinator.rs')
                (dest/'src/mod.rs').write_text('pub mod coordinator;\n')
            elif function == 'may_trim':
                (dest/'src').mkdir()
                shutil.copy2(RETENTION, dest/'src/recovery.rs')
                (dest/'src/mod.rs').write_text('pub mod recovery;\n')
            else:
                shutil.copytree(CORE, dest/'src')
            path = dest/'src'/filename
            source = path.read_text()
            if source.count(before) != 1:
                raise SystemExit(f'{name}: mutation site changed')
            path.write_text(source.replace(before,after))
            run_proof(name, 'src/mod.rs', cwd=dest, reject=True, module=module, function=function)
