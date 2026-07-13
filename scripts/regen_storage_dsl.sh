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
TRANSPILER="${1:-build_local/rusty-cpp-transpiler-a4bcff5f}"

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
  src/cluster/sharding_policy_builder.h
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
  # Raft snapshot metadata: value struct + thin methods delegated to C++
  # helpers; reader/writer/manager virtual interfaces stay hand-C++ for now.
  src/deptran/raft/snapshot_manager.hpp
  # Raft in-memory snapshot helper: value-style metadata constructor only;
  # virtual reader/writer/manager classes stay hand-C++.
  src/deptran/raft/memory_snapshot_manager.hpp
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
