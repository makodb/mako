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
  src/cluster/kv_store.h
  # Sharding-policy value types (copyable aggregates + inherent-impl
  # methods; KeyExtractor stays hand-C++ for its `type` keyword field).
  src/cluster/sharding_policy.h
  # ConfigManager: struct + ~25 methods inline (kv calls via the
  # (*(*self).kv).m() deref form); cm_* kernels for no-throw parse etc.
  src/cluster/config_manager.h
)

post_pass() {
  # Within RUSTYCPP:GEN regions, prefix `inline ` onto column-0
  # out-of-line definitions (`Ret Owner::name(...)`, `Owner Owner::new_`,
  # free fns emitted by the DSL). Class bodies/virtuals are indented and
  # class/template/namespace/comment lines are excluded, so they are
  # untouched.
  python3 - "$1" <<'EOF'
import re, sys
p = sys.argv[1]
lines = open(p).read().split('\n')
out, in_gen = [], False
defpat = re.compile(r'^(?!inline\b|class\b|struct\b|template\b|namespace\b|/\*|//|\})\S.*\b\w+::~?\w+\s*\(')
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
