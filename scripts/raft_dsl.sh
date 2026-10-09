#!/usr/bin/env bash
# Regenerate or verify the Raft inline-Rust DSL carriers.
#
# Raft carriers now receive the same ODR post-pass scripts/regen_storage_dsl.sh
# applies: within a GEN region, column-0 out-of-line definitions are prefixed
# with `inline `. Check mode validates the source hash (against copies with
# that pass undone) and a fresh rewrite (against copies with it applied), so
# edits to either side of a DSL/GEN pair are still detected.
#
# NOTE (Stage 2): the Rust is compiled AS A CRATE, not per carrier, and through
# Cargo rather than bare rustc. The old per-carrier `rustc` stage was removed --
# see the comment where it used to be, in the check loop below.
#
# The two limits that used to confine conversions to ODR-exempt shapes (pub
# trait, struct with no impl, const fn) are gone: the ODR post-pass above, and
# the manifest-less `rustc` invocation that could not resolve a single foreign
# type name. What still stands is the orphan-impl rule -- an `impl` on a
# hand-written C++ type is stubbed out, so the unit of conversion is a whole
# type -- and implementation inheritance, which has no Rust spelling. See
# the constraints list in the Raft migration plan and docs/stage2_raft.txt
# (both removed; see git history; docs/stage2_open_questions.md Q1b lived on
# as docs/stage2_raft.txt:353).
#
# Usage:
#   bash scripts/raft_dsl.sh --check [--transpiler PATH] [FILE ...]
#   bash scripts/raft_dsl.sh --rewrite [--transpiler PATH] [FILE ...]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPOSITORY_ROOT}" || exit 2

MODE="check"
TRANSPILER="${REPOSITORY_ROOT}/third-party/rusty-cpp/target/release/rusty-cpp-transpiler"
RUSTC_BIN="${RUSTC:-rustc}"
REQUIRED_RUSTY_CPP_COMMIT="1689f4380c25d13455cbe1f9eb8e5ff94e49861c"
FILES=()
EXPECTED_BLOCKS=(
  "src/deptran/raft/channel_transport.hpp|raft_channel_transport.scalar_decisions"
  "src/deptran/raft/commo.h|raft_commo.scalar_decisions"
  "src/deptran/raft/frame.cc|raft_frame.lab_decisions"
  "src/deptran/raft/log_storage.hpp|raft_log_entry.scalar_decisions"
  "src/deptran/raft/memory_log_storage.hpp|raft_memory_log.scalar_decisions"
  "src/deptran/raft/memory_snapshot_manager.hpp|raft_memory_snapshot.stream_math"
  "src/deptran/raft/messages.hpp|raft_messages.append_entries_reply"
  "src/deptran/raft/messages.hpp|raft_messages.heartbeat"
  "src/deptran/raft/messages.hpp|raft_messages.install_snapshot_reply"
  "src/deptran/raft/messages.hpp|raft_messages.vote"
  "src/deptran/raft/quorum.hpp|raft_quorum.scalar_decisions"
  "src/deptran/raft/raft_worker.cc|raft_worker.scalar_decisions"
  "src/deptran/raft/rocksdb_log_storage.hpp|raft_rocksdb_log.scalar_decisions"
  "src/deptran/raft/snapshot_manager.hpp|raft_snapshot.metadata_decisions"
  "src/deptran/raft/snapshot_format.hpp|raft_snapshot.crc32_scalar_step"
  "src/deptran/raft/snapshot_format.hpp|raft_snapshot.crc32_update_loop"
  "src/deptran/raft/snapshot_format.hpp|raft_snapshot.format_decisions"
  "src/deptran/raft/snapshot_format.hpp|raft_snapshot.format_enums"
  "src/deptran/raft_main_helper.cc|raft_main.argument_casefold"
  "src/deptran/raft_main_helper.cc|raft_main.group_mode"
  "src/deptran/raft_main_helper.cc|raft_main.group_mode_argument_predicate"
  "src/deptran/communicator.h|deptran_communicator.peer_registry"
  "src/deptran/scheduler.h|deptran_scheduler.tx_log_server"
)
# Distinct carrier paths named by EXPECTED_BLOCKS -- used to decide whether a
# run covers the whole graph (and therefore whether to verify the crate).
mapfile -t EXPECTED_INVENTORY_FILES < <(
  printf '%s\n' "${EXPECTED_BLOCKS[@]}" | cut -d'|' -f1 | LC_ALL=C sort -u)


# ---------------------------------------------------------------------------
# ODR post-pass (gate G2).
#
# Ported verbatim from scripts/regen_storage_dsl.sh:72-100, which has run in
# production on src/mako/storage/mbta_wrapper.hh (23 inline-prefixed methods)
# and src/cluster/config_manager.h (~33) since those headers were converted.
#
# Within a RUSTYCPP:GEN region, prefix `inline ` onto column-0 out-of-line
# definitions -- `Ret Owner::name(...)`, `Owner Owner::new_(...)`, and
# `Owner::~Owner()` from `impl Drop`. Without it a header-resident type with
# an inherent impl produces `ld: multiple definition of ...` across two TUs,
# which is why raft conversions have so far been limited to free const fns
# over scalars. Class bodies and virtuals are indented, and class / template /
# namespace / comment lines are excluded, so none of them are touched.
#
# NOTE: the `\w+::` has NO leading `\b` on purpose -- a `\b` before it makes the
# pattern fail to match destructors (`Owner::~Owner()`), which then emit a
# non-inline out-of-line dtor and blow up with a multiple-definition link
# error in any header included by more than one TU.
#
# `^\S` alone is NOT enough to anchor these to definitions. The claim this
# comment used to make -- "statements inside bodies are indented and skipped"
# -- is false for one emitted shape: a Rust block EXPRESSION lowers to an
# immediately-invoked lambda whose body the emitter renders at column 0. Its
# statements then matched, and `inline uint64_t size_before = rusty::len(...)`
# and `inline if (rusty::detail::rust_not(...))` are syntax errors the DSL
# gate cannot see -- it checks the Rust, not the emitted C++ -- so they
# surface much later as a compile failure in something unrelated-looking.
#
# Hence the two exclusions below, neither of which can hold for the FIRST
# LINE of an out-of-line definition:
#   - the line contains `;`   -- a definition opens a body rather than ending
#     a statement, and a wrapped signature's first line ends in `,` or `(`;
#   - the line starts with a control keyword.
# Both only ever REMOVE matches, so nothing that used to be inlined stops
# being inlined.
#
# On the tree as of this commit the pass changes ZERO lines across all 17
# carriers: every existing block is a free const fn that already emits as
# constexpr or as an indented class-body member. It is installed now so that
# the first conversion that needs it does not also have to introduce it.
odr_post_pass() {
  "${PYTHON_BIN:-python3}" - "$1" <<'PYEOF'
import re, sys
p = sys.argv[1]
lines = open(p).read().split('\n')
out, in_gen = [], False
defpat = re.compile(r'^(?!inline\b|class\b|struct\b|template\b|namespace\b|/\*|//|\})\S.*\w+::~?\w+\s*\(')
stmtpat = re.compile(r'^(if|for|while|do|switch|case|default|return|else|throw|delete|break|continue|goto)\b')
def is_definition(ln):
    return defpat.match(ln) is not None and ';' not in ln and not stmtpat.match(ln)
for ln in lines:
    if ln.startswith('/*RUSTYCPP:GEN-BEGIN'):
        in_gen = True
    elif ln.startswith('/*RUSTYCPP:GEN-END'):
        in_gen = False
    elif in_gen and is_definition(ln):
        ln = 'inline ' + ln
    out.append(ln)
open(p, 'w').write('\n'.join(out))
PYEOF
}

# Inverse of odr_post_pass, used ONLY to build the copies handed to
# `inline-rust --check`.
#
# That check compares a committed GEN region against what the emitter renders,
# and the emitter does not render the `inline ` this script adds. Feeding it
# the committed file directly would therefore report drift for every
# post-passed definition. Stripping first keeps both gates: the emitter's own
# source-hash/render check AND the full-file diff below.
#
# SHARP EDGE, so it is findable if it ever fires: if the emitter itself ever
# starts rendering `inline Ret Owner::method(` at column 0, this strips a
# prefix odr_post_pass did not add, and `inline-rust --check` reports a drift
# that is not real. The fix then is to drop this strip, not to weaken the
# post-pass -- the full-file diff below already subsumes what --check proves.
odr_strip_pass() {
  "${PYTHON_BIN:-python3}" - "$1" <<'PYEOF'
import re, sys
p = sys.argv[1]
lines = open(p).read().split('\n')
out, in_gen = [], False
defpat = re.compile(r'^(?!inline\b|class\b|struct\b|template\b|namespace\b|/\*|//|\})\S.*\w+::~?\w+\s*\(')
stmtpat = re.compile(r'^(if|for|while|do|switch|case|default|return|else|throw|delete|break|continue|goto)\b')
def is_definition(ln):
    return defpat.match(ln) is not None and ';' not in ln and not stmtpat.match(ln)
for ln in lines:
    if ln.startswith('/*RUSTYCPP:GEN-BEGIN'):
        in_gen = True
    elif ln.startswith('/*RUSTYCPP:GEN-END'):
        in_gen = False
    elif in_gen and ln.startswith('inline ') and is_definition(ln[len('inline '):]):
        ln = ln[len('inline '):]
    out.append(ln)
open(p, 'w').write('\n'.join(out))
PYEOF
}

usage() {
  echo "Usage: bash scripts/raft_dsl.sh --check [--transpiler PATH] [FILE ...]" >&2
  echo "       bash scripts/raft_dsl.sh --rewrite [--transpiler PATH] [FILE ...]" >&2
}

while (($#)); do
  case "$1" in
    --check)
      MODE="check"
      shift
      ;;
    --rewrite)
      MODE="rewrite"
      shift
      ;;
    --transpiler)
      if (($# < 2)); then
        echo "--transpiler requires a path" >&2
        usage
        exit 2
      fi
      TRANSPILER="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --)
      shift
      FILES+=("$@")
      break
      ;;
    -*)
      echo "unknown option: $1" >&2
      usage
      exit 2
      ;;
    *)
      FILES+=("$1")
      shift
      ;;
  esac
done

if [[ ! -x "${TRANSPILER}" ]]; then
  echo "no executable transpiler at ${TRANSPILER}" >&2
  exit 2
fi

RUSTYCPP_DIR="${REPOSITORY_ROOT}/third-party/rusty-cpp"
# git for the pin attestation, vouching for exactly the directory it reads.
# In a CI container the checkout belongs to another uid, and bare git then
# refuses it ("detected dubious ownership"); actions/checkout's own
# safe.directory entry lives under a temporary HOME and never names the
# submodule. Same approach as ownership_exception() in
# scripts/extract_srpc_rust.py: only git's ownership heuristic is suppressed,
# and every pin comparison below still reads real git output.
owned_git() {
  local dir="$1"
  shift
  git -c "safe.directory=${dir}" -c "safe.directory=$(cd "${dir}" && pwd -P)" \
    -C "${dir}" "$@"
}
if ! GITLINK_ENTRY=$(owned_git "${REPOSITORY_ROOT}" ls-files --stage -- \
    third-party/rusty-cpp 2>/dev/null) ||
    ! read -r GITLINK_MODE GITLINK_HASH _ <<<"${GITLINK_ENTRY}"; then
  echo "cannot inspect the rusty-cpp gitlink" >&2
  exit 2
fi
if [[ "${GITLINK_MODE}" != "160000" ||
      "${GITLINK_HASH}" != "${REQUIRED_RUSTY_CPP_COMMIT}" ]]; then
  echo "rusty-cpp gitlink mismatch: expected ${REQUIRED_RUSTY_CPP_COMMIT}, got ${GITLINK_HASH:-missing}" >&2
  exit 2
fi
if ! CHECKED_OUT_EMITTER_HASH=$(owned_git "${RUSTYCPP_DIR}" rev-parse HEAD 2>/dev/null); then
  echo "cannot inspect the rusty-cpp checkout" >&2
  exit 2
fi
if [[ "${CHECKED_OUT_EMITTER_HASH}" != "${REQUIRED_RUSTY_CPP_COMMIT}" ]]; then
  echo "rusty-cpp checkout mismatch: expected ${REQUIRED_RUSTY_CPP_COMMIT}, got ${CHECKED_OUT_EMITTER_HASH}" >&2
  exit 2
fi
if [[ -n "$(owned_git "${RUSTYCPP_DIR}" status --porcelain --untracked-files=no)" ]]; then
  echo "rusty-cpp has tracked local changes; refusing an unpinned emitter" >&2
  exit 2
fi
if ! EMITTER_BUILD_INFO=$("${TRANSPILER}" --build-info 2>/dev/null); then
  echo "cannot read transpiler build provenance" >&2
  exit 2
fi
EXPECTED_BUILD_INFO="{\"git_hash\":\"${REQUIRED_RUSTY_CPP_COMMIT}\",\"git_dirty\":false}"
if [[ "${EMITTER_BUILD_INFO}" != "${EXPECTED_BUILD_INFO}" ]]; then
  echo "transpiler provenance mismatch: expected clean ${REQUIRED_RUSTY_CPP_COMMIT}, got ${EMITTER_BUILD_INFO}" >&2
  exit 2
fi

FULL_INVENTORY=0
if ((${#FILES[@]} == 0)); then
  FULL_INVENTORY=1
  # Two carriers here are deptran-level rather than Raft-level and are named
  # explicitly, like raft_main_helper.cc, because they are not under
  # src/deptran/raft: scheduler.h holds the TxLogServer interface both engines
  # implement, and communicator.h holds the peer table both communicators are
  # built on. See the note in src/deptran/raft/rust-modules.toml; when a third
  # arrives, that is the point to give deptran a manifest of its own.
  SEARCH_ROOTS=(src/deptran/raft src/deptran/raft_main_helper.cc
                src/deptran/scheduler.h src/deptran/communicator.h)
  if [[ -d src/deptran/fpga_raft ]]; then
    SEARCH_ROOTS=(src/deptran/fpga_raft "${SEARCH_ROOTS[@]}")
  fi
  if command -v rg >/dev/null 2>&1; then
    mapfile -t FILES < <(
      rg -l '#if RUSTYCPP_RUST' "${SEARCH_ROOTS[@]}" \
        -g '*.h' -g '*.hh' -g '*.hpp' -g '*.cc' -g '*.cpp' -g '*.cxx' |
        sort
    )
  else
    mapfile -t FILES < <(
      { grep -rl '#if RUSTYCPP_RUST' "${SEARCH_ROOTS[@]}" \
          --include='*.h' --include='*.hh' --include='*.hpp' \
          --include='*.cc' --include='*.cpp' --include='*.cxx'; } |
        sort
    )
  fi
fi

if ((${#FILES[@]} == 0)); then
  echo "no Raft inline-Rust DSL carriers found" >&2
  exit 2
fi

for index in "${!FILES[@]}"; do
  FILES[${index}]="${FILES[${index}]#./}"
  file="${FILES[${index}]}"
  case "${file}" in
    src/deptran/fpga_raft/*|src/deptran/raft/*|src/deptran/raft_main_helper.cc|src/deptran/scheduler.h|src/deptran/communicator.h) ;;
    *)
      echo "refusing non-Raft carrier: ${file}" >&2
      exit 2
      ;;
  esac
  if [[ ! -f "${file}" ]]; then
    echo "missing carrier: ${file}" >&2
    exit 2
  fi
  if ! grep -q '#if RUSTYCPP_RUST' "${file}"; then
    echo "carrier has no inline-Rust block: ${file}" >&2
    exit 2
  fi
done

EXPECTED_FOR_RUN=()
if ((FULL_INVENTORY)); then
  EXPECTED_FOR_RUN=("${EXPECTED_BLOCKS[@]}")
else
  for expected in "${EXPECTED_BLOCKS[@]}"; do
    expected_carrier="${expected%%|*}"
    for file in "${FILES[@]}"; do
      if [[ "${file}" == "${expected_carrier}" ]]; then
        EXPECTED_FOR_RUN+=("${expected}")
        break
      fi
    done
  done
fi

ACTUAL_BLOCKS=()
for file in "${FILES[@]}"; do
  while IFS= read -r marker; do
    block_id="${marker#*id=}"
    block_id="${block_id%% *}"
    ACTUAL_BLOCKS+=("${file}|${block_id}")
  done < <(grep -F '/*RUSTYCPP:GEN-BEGIN id=' "${file}" || true)
done

EXPECTED_INVENTORY=$(printf '%s\n' "${EXPECTED_FOR_RUN[@]}" | LC_ALL=C sort)
ACTUAL_INVENTORY=$(printf '%s\n' "${ACTUAL_BLOCKS[@]}" | LC_ALL=C sort)
if [[ "${ACTUAL_INVENTORY}" != "${EXPECTED_INVENTORY}" ]]; then
  echo "Raft DSL block inventory mismatch" >&2
  printf '  expected:\n%s\n  actual:\n%s\n' \
    "${EXPECTED_INVENTORY:-<empty>}" "${ACTUAL_INVENTORY:-<empty>}" >&2
  exit 2
fi

if [[ "${MODE}" == "rewrite" ]]; then
  "${TRANSPILER}" inline-rust --rewrite --files "${FILES[@]}"
  for file in "${FILES[@]}"; do
    odr_post_pass "${file}"
  done
  echo "rewrote ${#FILES[@]} Raft DSL carrier(s)"
  # Regenerate the Stage 2 crate from the blocks we just rewrote, so a
  # --rewrite leaves the tree consistent and a following --check passes.
  # Only when this run covered the whole graph; a single-file rewrite leaves
  # the crate to the next full one.
  RAFT_CRATE_MANIFEST="${REPOSITORY_ROOT}/src/deptran/raft/rust-modules.toml"
  if [[ -f "${RAFT_CRATE_MANIFEST}" &&
        ${#FILES[@]} -eq ${#EXPECTED_INVENTORY_FILES[@]} ]]; then
    "${PYTHON_BIN:-python3}" "${SCRIPT_DIR}/raft_crate_extract.py" \
      --mode rewrite --transpiler "${TRANSPILER}" \
      --manifest "${RAFT_CRATE_MANIFEST}" \
      --scratch "$(mktemp -d)" || exit 1
  fi
  exit 0
fi

if ! command -v "${RUSTC_BIN}" >/dev/null 2>&1; then
  echo "no rustc executable found at ${RUSTC_BIN}" >&2
  exit 2
fi

nearest_cargo_manifest() {
  local directory candidate
  directory="$(dirname -- "$1")"
  while true; do
    candidate="${directory}/Cargo.toml"
    if [[ -f "${candidate}" ]]; then
      printf '%s/Cargo.toml\n' "$(cd "${directory}" && pwd -P)"
      return 0
    fi
    if [[ "${directory}" == "." || "${directory}" == "/" ]]; then
      return 1
    fi
    directory="$(dirname -- "${directory}")"
  done
}

RAFT_DSL_TMPDIR="$(mktemp -d)"
cleanup() {
  rm -rf -- "${RAFT_DSL_TMPDIR}"
}
trap cleanup EXIT

# A carrier with no manifest must also have no manifest in its regeneration
# context. Refuse a TMPDIR nested inside an unrelated Cargo workspace only
# when this carrier set needs that manifest-free context.
NEEDS_MANIFESTLESS_CONTEXT=0
for file in "${FILES[@]}"; do
  if ! nearest_cargo_manifest "${file}" >/dev/null; then
    NEEDS_MANIFESTLESS_CONTEXT=1
    break
  fi
done
if ((NEEDS_MANIFESTLESS_CONTEXT)) &&
    TEMP_ANCESTOR_MANIFEST=$(nearest_cargo_manifest \
      "${RAFT_DSL_TMPDIR}/probe"); then
  echo "temporary directory unexpectedly inherits ${TEMP_ANCESTOR_MANIFEST}" >&2
  exit 2
fi

failures=0

# The emitter's own check runs against copies with the ODR post-pass undone,
# so that `inline ` prefixes this script added are not mistaken for drift.
# The copies keep their basename and Cargo context for the same reasons the
# rewrite mirrors below do.
PRISTINE=()
for index in "${!FILES[@]}"; do
  file="${FILES[${index}]}"
  pristine_dir="${RAFT_DSL_TMPDIR}/pristine/${index}"
  mkdir -p -- "${pristine_dir}"
  if manifest=$(nearest_cargo_manifest "${file}"); then
    ln -s -- "${manifest}" "${pristine_dir}/Cargo.toml"
  fi
  pristine="${pristine_dir}/$(basename -- "${file}")"
  cp -- "${file}" "${pristine}"
  odr_strip_pass "${pristine}"
  PRISTINE+=("${pristine}")
done

if ! output=$("${TRANSPILER}" inline-rust --check --files "${PRISTINE[@]}" 2>&1); then
  echo "DRIFT Raft DSL carriers (source hash or render failure)" >&2
  sed 's/^/    /' <<<"${output}" | head -12 >&2
  exit 1
fi

# Give every carrier a private mirror with its original basename. If the real
# carrier has a Cargo context, a manifest symlink canonicalizes back to the
# exact real manifest, preserving workspace and path-dependency resolution.
# Keeping the mirrors outside the checkout also supports read-only sources.
REGENERATED=()
for index in "${!FILES[@]}"; do
  file="${FILES[${index}]}"
  mirror_dir="${RAFT_DSL_TMPDIR}/${index}"
  mkdir -p -- "${mirror_dir}"
  if manifest=$(nearest_cargo_manifest "${file}"); then
    ln -s -- "${manifest}" "${mirror_dir}/Cargo.toml"
  fi
  regenerated="${mirror_dir}/$(basename -- "${file}")"
  cp -- "${file}" "${regenerated}"
  REGENERATED+=("${regenerated}")
done

if ! output=$("${TRANSPILER}" inline-rust --rewrite --files \
    "${REGENERATED[@]}" 2>&1); then
  echo "FAILED Raft DSL carriers (fresh rewrite)" >&2
  sed 's/^/    /' <<<"${output}" | head -12 >&2
  exit 1
fi

# Post-pass the fresh render exactly as --rewrite would, so the comparison
# below is committed-file versus what `--rewrite` actually produces. Applying
# it to one side only would report drift on every post-passed definition.
for regenerated in "${REGENERATED[@]}"; do
  odr_post_pass "${regenerated}"
done

for index in "${!FILES[@]}"; do
  file="${FILES[${index}]}"
  regenerated="${REGENERATED[${index}]}"
  if ! cmp -s "${file}" "${regenerated}"; then
    echo "DRIFT ${file} (committed GEN output)" >&2
    diff -u "${file}" "${regenerated}" | head -80 >&2 || true
    failures=$((failures + 1))
  fi

  # SOURCE-SIDE FOOTGUNS. Two constructs are accepted by the transpiler and
  # then lowered WRONG, with exit 0 and no marker anywhere in the output, so
  # the GEN scan below cannot see them. They have to be caught on the Rust
  # side instead.
  #
  #   #[cfg(...)]  inside a DSL block is DROPPED and the code it guards is
  #                emitted UNCONDITIONALLY. A `#[cfg(test)]` helper would ship
  #                in production. Use a C++ #ifdef around the whole block, or
  #                a runtime predicate. (#[cfg_attr(...)] is a different
  #                attribute and is fine -- every derive here uses it.)
  if bad=$(awk -v src="${file}" '
      /^[[:space:]]*#if RUSTYCPP_RUST/{d=1}
      d && /#\[cfg\(/ {print "  "src":"FNR": "$0}
      /^[[:space:]]*#endif/{d=0}' "${file}") && [ -n "${bad}" ]; then
    echo "FAILED ${file}: #[cfg(...)] inside a DSL block is silently dropped" >&2
    echo "${bad}" >&2
    echo "  the guarded code would be emitted unconditionally; use #ifdef" >&2
    echo "  around the whole block, or a runtime predicate" >&2
    failures=$((failures + 1))
  fi

  #   #[cpp_inherit] WITHOUT `use rusty::cpp_inherit;` in the same block is
  #                the worst of these. The attribute is authenticated through
  #                the marker crate; unauthenticated, the emitter DROPS THE
  #                BASE CLASS and emits adapter classes instead -- exit 0, no
  #                marker, no diagnostic, and the type silently stops
  #                implementing its interface. REPRODUCED against the pinned
  #                transpiler: with the import, `struct X : public Base`;
  #                without it, `struct X {` plus BaseAdapter<X>.
  if bad=$(awk -v src="${file}" '
      /^[[:space:]]*#if RUSTYCPP_RUST/ { d=1; imp=0; line=0; next }
      /^[[:space:]]*#endif/ {
        if (d && line && !imp) {
          print "  " src ":" line ": #[cpp_inherit] without use rusty::cpp_inherit;"
        }
        d=0; line=0; imp=0; next
      }
      d && /use[[:space:]]+rusty::cpp_inherit[[:space:]]*;/ { imp=1 }
      d && /#\[cpp_inherit\]/ { if (line == 0) line=FNR }
    ' "${file}") && [ -n "${bad}" ]; then
    echo "FAILED ${file}: #[cpp_inherit] is not authenticated in this block" >&2
    echo "${bad}" >&2
    echo "  without 'use rusty::cpp_inherit;' in the SAME block the emitter" >&2
    echo "  silently drops the base class and emits adapters instead" >&2
    failures=$((failures + 1))
  fi

  #   #[derive(Default)] is lowered to `static X default_() { return {}; }`,
  #                i.e. C++ value-initialisation. That is correct only when
  #                every field's own Default is all-zero-bytes. A field whose
  #                Rust Default is non-zero is silently zeroed instead. Allow
  #                the derive only on structs whose fields are all primitive
  #                scalars; anything else must write `fn new()` explicitly,
  #                which is this codebase's convention anyway.
  if bad=$(python3 - "${file}" <<'PYCHK'
import re, sys
path = sys.argv[1]
src = open(path).read().split('\n')
state = 'cpp'; block = []; blocks = []; start = 0
for i, l in enumerate(src):
    if state == 'cpp' and re.match(r'\s*#if RUSTYCPP_RUST\s*$', l):
        state = 'rust'; block = []; start = i + 1; continue
    if state == 'rust' and re.match(r'\s*#endif\s*$', l):
        state = 'cpp'; blocks.append((start, block)); continue
    if state == 'rust':
        block.append(l)
SCALAR = re.compile(r'^(u8|u16|u32|u64|usize|i8|i16|i32|i64|isize|f32|f64|bool|char)$')
bad = []
for start, body in blocks:
    for n, l in enumerate(body):
        if not re.search(r'derive\([^)]*\bDefault\b', l):
            continue
        # find the struct that follows and read its field types
        k = n
        while k < len(body) and not re.match(r'\s*pub struct ', body[k]):
            k += 1
        if k >= len(body):
            continue
        name = re.sub(r'.*pub struct (\w+).*', r'\1', body[k])
        k += 1
        while k < len(body) and not body[k].strip().startswith('}'):
            m = re.match(r'\s*(?:pub\s+)?\w+:\s*([\w:<>]+)\s*,', body[k])
            if m and not SCALAR.match(m.group(1)):
                bad.append(f"  {path}:{start+n+1}: derive(Default) on {name}, "
                           f"field type {m.group(1)} is not a primitive scalar")
                break
            k += 1
print('\n'.join(bad))
PYCHK
  ) && [ -n "${bad}" ]; then
    echo "FAILED ${file}: derive(Default) is mis-lowered for non-scalar fields" >&2
    echo "${bad}" >&2
    echo "  it emits 'return {}' (value-init), zeroing any field whose Rust" >&2
    echo "  Default is non-zero. Write an explicit fn new() instead." >&2
    failures=$((failures + 1))
  fi

  # SILENT STATEMENT LOSS. The transpiler does not fail on constructs it
  # cannot lower; it emits a marker and drops the statement, exits 0, and both
  # this gate and the C++ build stay green while the behaviour is simply gone.
  # Two such markers exist and both fire on plausible authoring mistakes:
  #
  #   // TODO: <name>!(...)      an unknown macro. `Log_debug!("x {}", v);`
  #                              produces exactly this and the log call
  #                              disappears -- MEASURED, not hypothetical.
  #   #if 0  // patcher:         an `impl` on a hand-written C++ type, which
  #                              the orphan-impl rule stubs out wholesale.
  #
  # Neither is ever a legitimate thing to commit inside a GEN region, so scan
  # the generated side and fail on either. This is the only check here that
  # catches a body silently doing less than its Rust says.
  # Match `// TODO` in ANY form, not the literal `// TODO:`. The emitter also
  # writes `// TODO orphan impl:` (no colon after TODO) and thirteen
  # `// TODO(interface_traits): ...` variants -- skipped by-value self methods,
  # skipped generic methods, skipped adapters, unsupported associated
  # constants. Every one of those silently drops a method from an adapter, and
  # the narrower pattern caught none of them. A GEN region is entirely
  # generated, so any TODO inside one is an emitter marker by construction.
  if lost=$(awk -v src="${file}" '/RUSTYCPP:GEN-BEGIN/{g=1} g&&/\/\/ *TODO|#if 0  \/\/ patcher:/{print "  "src":"FNR": "$0} /RUSTYCPP:GEN-END/{g=0}' "${regenerated}") && [ -n "${lost}" ]; then
    echo "FAILED ${file}: transpiler dropped a statement inside a GEN region" >&2
    echo "${lost}" >&2
    echo "  an unknown macro or an impl on a C++ type was silently discarded;" >&2
    echo "  rewrite that construct rather than committing the marker" >&2
    failures=$((failures + 1))
  fi

  # The per-carrier `rustc` compile that used to sit here has been REMOVED.
  #
  # It compiled each carrier's extracted Rust as its own standalone crate
  # (--crate-name raft_dsl_carrier_N) with no --extern and no --type-map.
  # That proved each block was self-contained Rust -- and, because a
  # dependency-free fragment cannot name rusty::Mutex, ::janus::Command, a
  # container, or a type from a sibling carrier, it was also the reason every
  # block had to be a free function over scalars. src/srpc never operated
  # under that constraint: scripts/srpc_dsl_check.sh invokes rustc zero times
  # and srpc verifies at crate level.
  #
  # Verification now happens over the whole module graph in the crate stage
  # below: rustc + clippy over src/deptran/raft, plus a drift check that a
  # committed .rs must equal a fresh extraction. That is strictly more code
  # compiled, at coarser granularity -- the trade recorded in
  # docs/stage2_current_progress.txt.
  #
  # WHAT WAS GIVEN UP, explicitly: a block may now compile only because a
  # sibling module or a crate dependency supplies something. Per-carrier
  # self-containment is no longer proven.
  :
done

# ---------------------------------------------------------------------------
# STAGE 2: crate-level verification.
#
# The per-carrier rustc stage above proves each carrier is self-contained
# Rust. This stage proves the same Rust compiles as a MODULE GRAPH, which is
# what src/srpc does and what makes impl/containers/type-mapped C++ types
# expressible at all. Both run for now; the per-carrier stage is removed in a
# following commit once they have agreed.
#
# The .rs files under the crate are GENERATED from the inline blocks. In
# --rewrite mode they are regenerated; in --check mode a mismatch between a
# committed .rs and a fresh extraction is a drift failure, so the crate can
# never silently diverge from the blocks it came from.
# ---------------------------------------------------------------------------
RAFT_CRATE_DIR="${REPOSITORY_ROOT}/src/deptran/raft"
RAFT_CRATE_MANIFEST="${RAFT_CRATE_DIR}/rust-modules.toml"

# The crate is the whole module graph, so only verify it when this run covers
# every carrier. A single-file invocation (raft_dsl.sh --check <one file>)
# still does the per-carrier work and skips this stage.
if [[ -f "${RAFT_CRATE_MANIFEST}" && ${#FILES[@]} -eq ${#EXPECTED_INVENTORY_FILES[@]} ]]; then
  crate_out="${RAFT_DSL_TMPDIR}/crate"
  mkdir -p "${crate_out}"
  if ! "${PYTHON_BIN:-python3}" "${SCRIPT_DIR}/raft_crate_extract.py" \
      --mode "${MODE}" --transpiler "${TRANSPILER}" \
      --manifest "${RAFT_CRATE_MANIFEST}" --scratch "${crate_out}" 2>&1; then
    echo "FAILED Raft crate extraction/drift check" >&2
    failures=$((failures + 1))
  else
    # Compile the whole crate THROUGH CARGO, not through bare rustc.
    #
    # Bare rustc reads no manifest, so it resolves no dependency, so no
    # extracted block could name a foreign type: rusty::Mutex, rusty::Option,
    # rusty::sync::Arc, rusty::ReactorPollThread and ::janus::Command all died
    # at E0433/E0573 here, before the emitter was ever consulted. The emitter
    # lowers every one of those correctly -- this invocation was the gate, and
    # it is a Mako-local script line, not a toolchain limit. src/srpc has never
    # had it: scripts/srpc_dsl_check.sh invokes rustc zero times.
    #
    # RUSTFLAGS rather than a -D on the command line, because cargo passes the
    # flag to every crate it builds from this manifest; the per-carrier stage's
    # `-D warnings` is preserved, not relaxed.
    #
    # See the facade-crate constraint in the Raft migration plan (removed; see git history).
    if ! output=$(cd "${RAFT_CRATE_DIR}" && \
        RUSTFLAGS="-D warnings" CARGO_TARGET_DIR="${RAFT_CRATE_DIR}/target" \
        cargo build --quiet --workspace --lib 2>&1); then
      echo "FAILED Raft crate does not compile" >&2
      sed 's/^/    /' <<<"${output}" | head -30 >&2
      failures=$((failures + 1))
    fi
    # clippy, as src/srpc's gate does. Absent clippy is a hard failure: the
    # crate regime rests on matching srpc's verification, not a subset of it.
    if ! command -v cargo-clippy >/dev/null 2>&1 && ! cargo clippy --version >/dev/null 2>&1; then
      echo "FAILED clippy unavailable (required for crate-level verification)" >&2
      failures=$((failures + 1))
    elif ! output=$(cd "${RAFT_CRATE_DIR}" && cargo clippy --quiet --workspace -- -D warnings 2>&1); then
      echo "FAILED Raft crate clippy" >&2
      sed 's/^/    /' <<<"${output}" | head -30 >&2
      failures=$((failures + 1))
    fi
  fi
fi

echo "checked ${#FILES[@]} Raft DSL carrier(s): pin, inventory, generated C++; ${failures} failure(s)"
exit $((failures > 0 ? 1 : 0))
