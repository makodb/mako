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
# The surviving src/cluster carriers are C++23 module partition units,
# compiled exactly once. The inline post-pass remains harmless there;
# its load-bearing ODR purpose is the included src/mako/storage headers.
#
# Use the pinned compiler; the regenerated storage/configuration carriers below
# are checked with the same binary used by SRPC's production source gate:
#
#   $ third-party/rusty-cpp/target/release/rusty-cpp-transpiler --build-info
#   {"git_hash":"7e0c201f1b0d548f0166dc9ee700f24bc18066a4","git_dirty":false}
#
# The backend carriers use the compiler-owned inert spelling
# `#[cfg_attr(any(), cpp_inherit)]`. Unlike a live proc-macro attribute, this is
# authenticated by its permanently false predicate and needs no Cargo facade.
# Keep each Rust block adjacent to its GEN region. Module exports wrap the
# containing namespace, not the gap between source and generated output.
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
  # a column-zero `while (rusty::detail::rust_not(...)) {` matches the
  # function pattern on `detail::rust_not(` and must not become `inline while`.
  # The exclusion was measured behavior-neutral in the pre-retirement
  # 2026-09-17 carrier census.
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
    # Preserve the carrier's actual Cargo/sysroot context and relative includes;
    # check mode must use the same source context as rewrite mode.
    tmp="$(mktemp "$(dirname "$f")/.regen-XXXXXX.${f##*.}")"
    cp "$f" "$tmp"
    # Report and keep going rather than aborting the sweep on the first
    # failure: a drift guard that stops at file 2 under-reports (same
    # reasoning as scripts/rrr_dsl_check.sh).
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
