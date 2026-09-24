#!/usr/bin/env bash
# Regenerate the storage-layer inline-Rust DSL headers
# (docs/storage-interface.md). This wrapper is THE regeneration
# path — not bare `inline-rust --rewrite` — because the transpiler
# emits out-of-line method definitions without `inline` (correct for
# its single-TU module precedent, an ODR violation in the four
# multi-TU src/mako/storage headers); the post-pass below adds the
# keyword inside the GEN regions. Deterministic for a fixed
# transpiler: same input → same output.
#
# The eleven src/cluster entries became C++23 module partition units in
# dda3fa458 ("cluster: convert to a C++23 named module") — each one now
# opens with `module;` and is compiled exactly once — so the `inline`
# prefix is harmless there but no longer load-bearing. The ODR argument
# above is about src/mako/storage. That is also why the post-pass is not
# idempotent on five of them (sharding_policy.h, sharding_policy_cache.h,
# cluster_config.h, shard.h, shard_manager.h): their committed GEN regions
# carry un-prefixed out-of-line definitions, so running the post-pass over
# the committed file would add `inline`. Neither code path does that — both
# transpile first, and the transpiler rewrites the whole GEN region.
#
# WHICH TRANSPILER (measured 2026-09-17)
# --------------------------------------
# The default is the pinned submodule binary, the same one
# scripts/srpc_dsl_check.sh:25, scripts/extract_srpc_rust.py:30 and
# scripts/check_srpc_crate_mode.py:23 default to, and the only one a
# fresh checkout can produce. It must report the pin:
#
#   $ third-party/rusty-cpp/target/release/rusty-cpp-transpiler --build-info
#   {"git_hash":"a1f8fef85e8d43bb00f85f8ef32e5ecc69408642","git_dirty":false}
#
# The committed GEN regions were NOT produced by it. They were produced
# by a build of rusty-cpp `a4bcff5f` ("codegen: qualify cross-crate
# blanket/orphan rusty_ext calls to the dep crate path"), which is a
# strict ancestor of the pin — same lineage, not a fork — from a binary
# that was never committed. The old default of this script pointed at
# `build_local/rusty-cpp-transpiler-a4bcff5f`; no `build_local/` exists
# in this tree, so that default was unrunnable for anyone but its
# author. It is gone.
#
# Ancestor is not equivalent: the pin's codegen differs, so re-running
# today rewrites the committed output. Measured 2026-09-17 by
# `scripts/regen_storage_dsl.sh --check` across the 15 entries below —
# 11 drift, 1 clean (src/cluster/cluster_config.cc), and 3 fail
# outright with
#
#   inline `cpp_inherit` requires a Cargo manifest so the `rusty`
#   provider can be authenticated
#
# which is exactly the three files carrying a live `#[cpp_inherit]`
# attribute (masstree_ordered_index.hh, mbta_sharded_ordered_index.hh,
# mbta_wrapper.hh — neither the repo root nor src/mako/storage/ carries
# a Cargo.toml). So a regen at the pin is a reviewed change to be built
# and tested, not a formality, and `--check` reports that drift until
# the GEN regions are refreshed at the pin. Pass an explicit transpiler
# path to reproduce the committed output against the older binary.
#
# NOT WIRED INTO CI: nothing under .github/workflows/ or ci/ invokes
# this script. It is a manual pre-commit guard.
#
# DSL background: docs/porting-cpp-to-rust-dsl.md — §6 "Build, Verify,
# Commit — The Operational Loop" (numbering as of 2026-09-17).
#
# Usage: scripts/regen_storage_dsl.sh [--check] [path/to/rusty-cpp-transpiler]
#   --check: regenerate into a temp copy and diff against the committed
#            file (drift guard); non-zero exit on drift.
set -euo pipefail

cd "$(dirname "$0")/.."

CHECK=0
if [[ "${1:-}" == "--check" ]]; then CHECK=1; shift; fi
TRANSPILER="${1:-third-party/rusty-cpp/target/release/rusty-cpp-transpiler}"
if [[ ! -x "$TRANSPILER" ]]; then
  echo "no transpiler at $TRANSPILER" >&2
  # The workspace has no [workspace] default-members, so a bare `cargo build`
  # at its root builds only the root `rusty-cpp` package; name the member.
  echo "build it with: cargo build --release -p rusty-cpp-transpiler \\" >&2
  echo "                  --manifest-path third-party/rusty-cpp/Cargo.toml" >&2
  exit 2
fi

FILES=(
  src/mako/storage/abstract_ordered_index.h
  src/mako/storage/mbta_sharded_ordered_index.hh
  src/mako/storage/masstree_ordered_index.hh
  src/mako/storage/mbta_wrapper.hh
  # Cluster metadata port authored in the DSL (namespaced trait).
  src/cluster/kv_store.h
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
  # ConfigWatcher: struct + Poll + accessors + thread lifecycle all DSL;
  # the stop-then-join dtor is a DSL `impl Drop` — GEN emits a real
  # ~ConfigWatcher() (re-verified 2026-09-17 at pin a1f8fef8: the pin
  # emits the same `~ConfigWatcher() noexcept(false)` declaration and
  # out-of-line body; the old "DSL can't emit a join-on-drop dtor" note
  # is stale). No C++ kernels remain.
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
  # NOTE: control-flow keywords are excluded too. `^\S` is not enough on its
  # own: at pin a1f8fef8 the transpiler emits some lambda bodies unindented,
  # e.g. config_watcher.h gets `while (rusty::detail::rust_not(...)) {` at
  # column 0 — which matches `\w+::~?\w+\s*\(` on `detail::rust_not(` and was
  # being rewritten to the uncompilable `inline while (...)`. Verified
  # 2026-09-17: over all 15 entries below, the old pattern and this one
  # produce byte-identical output, so the exclusion is behaviour-neutral on
  # everything currently committed.
  python3 - "$1" <<'EOF'
import re, sys
p = sys.argv[1]
lines = open(p).read().split('\n')
out, in_gen = [], False
defpat = re.compile(r'^(?!inline\b|class\b|struct\b|template\b|namespace\b|/\*|//|\}|'
                    r'(?:if|else|for|while|do|switch|case|default|return|try|catch|throw|goto)\b)'
                    r'\S.*\w+::~?\w+\s*\(')
for ln in lines:
    if ln.startswith('/*RUSTYCPP:GEN-BEGIN'):
        in_gen = True
    elif ln.startswith('/*RUSTYCPP:GEN-END'):
        in_gen = False
    elif in_gen and defpat.match(ln):
        ln = 'inline ' + ln
    out.append(ln)
open(p, 'w').write('\n'.join(out))
EOF
}

status=0
for f in "${FILES[@]}"; do
  if [[ $CHECK -eq 1 ]]; then
    tmp="$(mktemp --suffix=".${f##*.}")"
    cp "$f" "$tmp"
    # Report and keep going rather than aborting the sweep on the first
    # failure: a drift guard that stops at file 2 under-reports (same
    # reasoning as scripts/srpc_dsl_check.sh).
    if ! "$TRANSPILER" inline-rust --rewrite --files "$tmp" >/dev/null; then
      echo "TRANSPILE-FAIL: $f (see the transpiler message above)" >&2
      status=1
      rm -f "$tmp"
      continue
    fi
    post_pass "$tmp"
    if ! diff -q "$f" "$tmp" >/dev/null; then
      echo "DRIFT: $f (Rust block and committed GEN region disagree)" >&2
      status=1
    fi
    rm -f "$tmp"
  else
    if ! "$TRANSPILER" inline-rust --rewrite --files "$f" >/dev/null; then
      echo "TRANSPILE-FAIL: $f (left unchanged)" >&2
      status=1
      continue
    fi
    post_pass "$f"
    echo "regenerated $f"
  fi
done
exit $status
