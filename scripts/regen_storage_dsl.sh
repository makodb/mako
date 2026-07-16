#!/usr/bin/env bash
# Regenerate the storage-layer inline-Rust DSL headers
# (docs/storage-interface.md). This wrapper is THE regeneration
# path — not bare `inline-rust --rewrite` — because the transpiler
# emits out-of-line method definitions without `inline` (correct for
# its single-TU module precedent, an ODR violation in our multi-TU
# headers); the post-pass below adds the keyword inside the GEN
# regions. Deterministic: same input → same committed output.
#
# Usage: scripts/regen_storage_dsl.sh [--check] [path/to/rusty-cpp-transpiler]
#   --check: regenerate into a temp copy and diff against the committed
#            file (drift guard for CI); non-zero exit on drift.
set -euo pipefail

cd "$(dirname "$0")/.."

CHECK=0
if [[ "${1:-}" == "--check" ]]; then CHECK=1; shift; fi
DEFAULT_TRANSPILER="build_local/rusty-cpp-transpiler-a4bcff5f"
if [[ ! -x "$DEFAULT_TRANSPILER" && -x "third-party/rusty-cpp/target/release/rusty-cpp-transpiler" ]]; then
  DEFAULT_TRANSPILER="third-party/rusty-cpp/target/release/rusty-cpp-transpiler"
fi
TRANSPILER="${1:-$DEFAULT_TRANSPILER}"

FILES=(
  src/mako/storage/abstract_ordered_index.h
  src/mako/storage/mbta_sharded_ordered_index.hh
  src/mako/storage/masstree_ordered_index.hh
  src/mako/storage/mbta_wrapper.hh
  # Cluster metadata port authored in the DSL (namespaced trait —
  # generates cleanly under the a4bcff5f transpiler).
  # src/cluster/kv_store.h
  # Sharding-policy value types (copyable aggregates + inherent-impl
  # methods; KeyExtractor stays hand-C++ for its `type` keyword field).
  src/cluster/sharding_policy.h
  # ConfigManager: struct + ~25 methods inline (kv calls via the
  # (*(*self).kv).m() deref form); cm_* kernels for no-throw parse etc.
  src/cluster/config_manager.h
  # RemoteKvStore: EXCLUDED from regen after the C++23 module conversion. It
  # uses `#[cpp_inherit] impl KvStore`, which needs the KvStore trait DEFINITION
  # visible to the transpiler. KvStore now lives in the sibling
  # `cluster:kv_store` module partition, reached via `import :kv_store;` — and
  # the transpiler does not follow module imports, so a regen drops
  # `: public KvStore` and the base constructors (verified). The committed GEN
  # block (generated pre-conversion, when kv_store.h was #included) is correct
  # and now hand-maintained. Re-enable if the transpiler learns to resolve
  # imported traits.
  # src/cluster/remote_kv_store.h
  # ShardingPolicyCache: rusty::Mutex/Cell/Option state, lock+guard
  # methods inline; spc_* kernels for pointer/raw-byte surgery.
  src/cluster/sharding_policy_cache.h
  # ClusterConfig: rusty::Mutex<ClusterConfigState> guarded state;
  # cc_* kernels for map/routing surgery.
  src/cluster/cluster_config.h
  # ConfigWatcher: struct + Poll + accessors DSL; thread lifecycle
  # in a CwPollThread RAII helper (DSL can't emit a join-on-drop dtor).
  src/cluster/config_watcher.h
  # ShardingPolicyBuilder: DSL-friendly reshape (value struct +
  # Result-based build; fluent TablePolicyBuilder deleted).
  #src/cluster/sharding_policy_builder.h
  # Shard: stub in-memory KV shard (stand-in for a masstree shard) — the
  # data holder the ShardManager migrates on KillShard.
  src/cluster/shard.h
  # ShardManager: drives the real ConfigManager/ClusterConfig reconfiguration
  # verbs against a map of stub Shards (add/kill/remove + route/put/get).
  src/cluster/shard_manager.h
  # ShardRouter: the four routing free fns as DSL pub fns; generated
  # definitions land in the .cc (compiled once), header stays decls.
  src/cluster/shard_router.cc
  # ClusterConfig routing math (cc_hash_key/extract/follow/route) as DSL pub
  # fns; generated defs in the .cc (header keeps decls). cc_load_from_cm stays
  # a hand-C++ kernel (reads through a complete ConfigManager).
  src/cluster/cluster_config.cc
  # Raft vote message PODs: first low-risk Raft DSL island; larger request
  # structs with janus::Command payloads stay hand-C++ for now.
  src/deptran/raft/messages.hpp
  # Raft persistent log entry: struct shape + thin methods in DSL, archive
  # save/load bodies stay delegated to C++ helpers to preserve storage format.
  src/deptran/raft/log_storage.hpp
  # Raft in-memory log storage helper predicates only; storage map/mutex
  # operations stay hand-C++.
  src/deptran/raft/memory_log_storage.hpp
  # Raft in-process test transport fault state; channel workers/adapters stay
  # hand-C++ because they own closures, channels, and blocking worker loops.
  src/deptran/raft/channel_transport.hpp
  # Raft async AppendEntries response value only; RaftCommo and callback/Future
  # lifetimes stay hand-C++.
  src/deptran/raft/commo.h
  # Raft server enums and tiny POD helpers only; RaftServer itself remains
  # hand-C++ because it owns threading, persistence, and consensus state.
  src/deptran/raft/server.h
  # Raft snapshot metadata: value struct + thin methods delegated to C++
  # helpers; reader/writer/manager virtual interfaces stay hand-C++ for now.
  src/deptran/raft/snapshot_manager.hpp
  # Raft in-memory snapshot helper: value-style metadata constructor only;
  # virtual reader/writer/manager classes stay hand-C++.
  src/deptran/raft/memory_snapshot_manager.hpp
  # Raft file snapshot path helpers only; file I/O and virtual manager
  # implementation stay hand-C++.
  src/deptran/raft/file_snapshot_manager.hpp
  # Raft snapshot binary header value type only; CRC and serialization
  # routines stay hand-C++ to preserve the exact wire/storage format.
  src/deptran/raft/snapshot_format.hpp
  # Raft recovery config value type only; RecoveryManager filesystem/storage
  # orchestration stays hand-C++.
  src/deptran/raft/recovery_manager.hpp
  # Raft RocksDB log storage string/key helper boundaries only; RocksDB C API
  # operations and storage orchestration stay hand-C++.
  src/deptran/raft/rocksdb_log_storage.hpp
  # Raft replicated DB batch operation value type only; command
  # serialization/RocksDB/Raft integration stay hand-C++.
  src/deptran/raft/replicated_db.h
)

post_pass() {
  # Within RUSTYCPP:GEN regions, prefix `inline ` onto column-0
  # out-of-line definitions (`Ret Owner::name(...)`, `Owner Owner::new_`,
  # `Owner::~Owner()` from impl Drop, free fns emitted by the DSL). Class
  # bodies/virtuals are indented and class/template/namespace/comment lines
  # are excluded, so they are untouched.
  # NOTE: the `\w+::` has NO leading `\b` on purpose — a `\b` before it makes
  # the pattern fail to match destructors (`Owner::~Owner()`), which then
  # emit a non-inline out-of-line dtor and blow up with a multiple-definition
  # link error in any header included by >1 TU. `^\S` already anchors these
  # to column 0, so dropping `\b` is safe (statements inside bodies are
  # indented and skipped).
  python3 - "$1" <<'EOF'
import re, sys
p = sys.argv[1]
lines = open(p).read().split('\n')
out, in_gen = [], False
is_header = not p.endswith(('.cc', '.cpp', '.cxx'))
methodpat = re.compile(r'^(?!inline\b|class\b|struct\b|template\b|namespace\b|/\*|//|\})\S.*\w+::~?\w+\s*\(')
freepat = re.compile(r'^(?!inline\b|class\b|struct\b|template\b|namespace\b|/\*|//|\})\S.*\s+\w+\s*\(')
for ln in lines:
    if ln.startswith('/*RUSTYCPP:GEN-BEGIN'):
        in_gen = True
    elif ln.startswith('/*RUSTYCPP:GEN-END'):
        in_gen = False
    elif in_gen and (methodpat.match(ln) or (is_header and freepat.match(ln))):
        ln = 'inline ' + ln
    if in_gen:
        # The a4bcff5f transpiler parses #[repr(...)] on Rust enums but does
        # not emit the matching C++ fixed underlying type yet. Keep Raft
        # wire/storage enum layout stable until that lands upstream.
        enum_reprs = {
            'SnapshotCompression': 'uint8_t',
            'SnapshotChecksumType': 'uint8_t',
            'ReplicatedDBOp': 'uint8_t',
            'AckType': 'uint64_t',
        }
        for name, repr_type in enum_reprs.items():
            ln = ln.replace(f'enum class {name};',
                            f'enum class {name} : {repr_type};')
            ln = ln.replace(f'enum class {name} {{',
                            f'enum class {name} : {repr_type} {{')
        # The transpiler lowers simple scalar while-loop comparisons through
        # rusty::detail::deref_if_pointer_like, which would require
        # rusty/slice.hpp in this header and conflicts with the module build.
        # Keep this generated CRC32 loop as ordinary scalar C++.
        ln = ln.replace(
            'while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(size)) {',
            'while (i < size) {')
        ln = ln.replace(
            'snapshot_crc32_read_byte_cpp(data, std::move(i))',
            'snapshot_crc32_read_byte_cpp(data, i)')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(offset) + rusty::detail::deref_if_pointer_like(size);',
            'return offset + size;')
        ln = ln.replace(
            'auto remaining = rusty::detail::deref_if_pointer_like(payload_size) - rusty::detail::deref_if_pointer_like(offset);',
            'auto remaining = payload_size - offset;')
        ln = ln.replace(
            'if (rusty::detail::deref_if_pointer_like(buffer_size) < rusty::detail::deref_if_pointer_like(remaining)) {',
            'if (buffer_size < remaining) {')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(offset) >= rusty::detail::deref_if_pointer_like(payload_size);',
            'return offset >= payload_size;')
        ln = ln.replace(
            'auto remaining = rusty::detail::deref_if_pointer_like(data_size) - rusty::detail::deref_if_pointer_like(offset);',
            'auto remaining = data_size - offset;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(valid) && (rusty::detail::deref_if_pointer_like(offset) >= rusty::detail::deref_if_pointer_like(data_size));',
            'return valid && offset >= data_size;')
        ln = ln.replace(
            'return (rusty::detail::deref_if_pointer_like(mode) == rusty::clone(RecoveryMode::FRESH_START)) || (rusty::detail::deref_if_pointer_like(mode) == rusty::clone(RecoveryMode::FORCED_FRESH));',
            'return mode == RecoveryMode::FRESH_START || mode == RecoveryMode::FORCED_FRESH;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(mode) == rusty::clone(RecoveryMode::NORMAL_RECOVERY);',
            'return mode == RecoveryMode::NORMAL_RECOVERY;')
        ln = ln.replace(
            'return std::string("meta:") + rusty::detail::deref_if_pointer_like(key);',
            'return std::string("meta:") + key;')
        ln = ln.replace(
            'return std::move(is_open);',
            'return is_open;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(start) < rusty::detail::deref_if_pointer_like(end);',
            'return start < end;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(size) == static_cast<size_t>(0);',
            'return size == 0;')
        ln = ln.replace(
            'return std::move(found);',
            'return found;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(is_open) && rusty::detail::deref_if_pointer_like(has_db);',
            'return is_open && has_db;')
        ln = ln.replace(
            'return std::move(has_value);',
            'return has_value;')
        ln = ln.replace(
            'return (rusty::detail::deref_if_pointer_like(key) < rusty::detail::deref_if_pointer_like(end_key)) && rusty::detail::deref_if_pointer_like(is_log_key);',
            'return key < end_key && is_log_key;')
        ln = ln.replace(
            'return (rusty::detail::deref_if_pointer_like(mode) == rusty::clone(RecoveryMode::FORCED_FRESH)) && rusty::detail::deref_if_pointer_like(clear_on_forced_fresh);',
            'return mode == RecoveryMode::FORCED_FRESH && clear_on_forced_fresh;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(offset) + rusty::detail::deref_if_pointer_like(size);',
            'return offset + size;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(snapshot_index) < rusty::detail::deref_if_pointer_like(keep_after_index);',
            'return snapshot_index < keep_after_index;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(snapshot_count) > 0;',
            'return snapshot_count > 0;')
        ln = ln.replace(
            'if (rusty::detail::deref_if_pointer_like(snapshot_count) > rusty::detail::deref_if_pointer_like(max_snapshots)) {',
            'if (snapshot_count > max_snapshots) {')
        ln = ln.replace(
            'return (rusty::detail::deref_if_pointer_like(snapshot_count) > rusty::detail::deref_if_pointer_like(max_snapshots)) && (rusty::detail::deref_if_pointer_like(position) >= rusty::detail::deref_if_pointer_like(max_snapshots));',
            'return snapshot_count > max_snapshots && position >= max_snapshots;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(magic) == static_cast<uint32_t>(1347305811);',
            'return magic == 0x504E4153;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(version) == static_cast<uint32_t>(1);',
            'return version == 1;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(compression) == rusty::clone(SnapshotCompression::NONE);',
            'return compression == SnapshotCompression::NONE;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(checksum_type) == rusty::clone(SnapshotChecksumType::CRC32);',
            'return checksum_type == SnapshotChecksumType::CRC32;')
        ln = ln.replace(
            'if (::snapshot_checksum_enabled(std::move(checksum_type))) {',
            'if (snapshot_checksum_enabled(checksum_type)) {')
        ln = ln.replace(
            'return (static_cast<size_t>(52) + rusty::detail::deref_if_pointer_like(data_size)) + rusty::detail::deref_if_pointer_like(checksum_size);',
            'return static_cast<size_t>(52) + data_size + checksum_size;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(index) <= rusty::detail::deref_if_pointer_like(last_applied_index);',
            'return index <= last_applied_index;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(kind) == rusty::detail::deref_if_pointer_like(expected_kind);',
            'return kind == expected_kind;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(op) == rusty::clone(janus::ReplicatedDBOp::PUT);',
            'return op == janus::ReplicatedDBOp::PUT;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(op) == rusty::clone(janus::ReplicatedDBOp::DELETE);',
            'return op == janus::ReplicatedDBOp::DELETE;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(op) == rusty::clone(janus::ReplicatedDBOp::BATCH);',
            'return op == janus::ReplicatedDBOp::BATCH;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(status) == rusty::clone(NotifyRestartStatus::PENDING);',
            'return status == NotifyRestartStatus::PENDING;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(status) == rusty::clone(NotifyRestartStatus::ACKNOWLEDGED);',
            'return status == NotifyRestartStatus::ACKNOWLEDGED;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(status) == rusty::clone(NotifyRestartStatus::DOWN);',
            'return status == NotifyRestartStatus::DOWN;')
        ln = ln.replace(
            'return !rusty::detail::deref_if_pointer_like(has_cmd);',
            'return !has_cmd;')
        ln = ln.replace(
            'return ((rusty::detail::deref_if_pointer_like(ok) == static_cast<uint64_t>(0)) && (rusty::detail::deref_if_pointer_like(term) == static_cast<uint64_t>(0))) && (rusty::detail::deref_if_pointer_like(last_log_index) == static_cast<uint64_t>(0));',
            'return ok == 0 && term == 0 && last_log_index == 0;')
        ln = ln.replace(
            'return !::commo_append_entries_reply_lost(std::move(ok), std::move(term), std::move(last_log_index));',
            'return !commo_append_entries_reply_lost(ok, term, last_log_index);')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(ack_type) == (static_cast<uint64_t>(AckType::Memory));',
            'return ack_type == static_cast<uint64_t>(AckType::Memory);')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(ack_type) == (static_cast<uint64_t>(AckType::Durable));',
            'return ack_type == static_cast<uint64_t>(AckType::Durable);')
        ln = ln.replace(
            'return (rusty::detail::deref_if_pointer_like(drop_from) == rusty::detail::deref_if_pointer_like(from)) && (rusty::detail::deref_if_pointer_like(drop_to) == rusty::detail::deref_if_pointer_like(to));',
            'return drop_from == from && drop_to == to;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(from_partition) != rusty::detail::deref_if_pointer_like(to_partition);',
            'return from_partition != to_partition;')
        ln = ln.replace(
            'return rusty::detail::deref_if_pointer_like(envelope_to) == rusty::detail::deref_if_pointer_like(site);',
            'return envelope_to == site;')
    out.append(ln)
open(p, 'w').write('\n'.join(out))
EOF
}

status=0
for f in "${FILES[@]}"; do
  if [[ $CHECK -eq 1 ]]; then
    tmp="$(mktemp --suffix=.hh)"
    cp "$f" "$tmp"
    "$TRANSPILER" inline-rust --rewrite --files "$tmp" >/dev/null
    post_pass "$tmp"
    if ! diff -q "$f" "$tmp" >/dev/null; then
      echo "DRIFT: $f (Rust block and committed GEN region disagree)" >&2
      status=1
    fi
    rm -f "$tmp"
  else
    "$TRANSPILER" inline-rust --rewrite --files "$f" >/dev/null
    post_pass "$f"
    echo "regenerated $f"
  fi
done
exit $status
