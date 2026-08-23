#!/usr/bin/env bash
# Regenerate or verify the inline-Rust DSL carriers under src/deptran/raft.
#
# Unlike scripts/regen_storage_dsl.sh, Raft carriers do not receive an ODR or
# textual post-pass: the pinned transpiler's output is committed byte-for-byte.
# Check mode validates both the source hash and a fresh rewrite, so edits to
# either side of a DSL/GEN pair are detected.
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
REQUIRED_RUSTY_CPP_COMMIT="a1f8fef85e8d43bb00f85f8ef32e5ecc69408642"
FILES=()
EXPECTED_BLOCKS=(
  "src/deptran/raft/commo.h|raft_commo.ack_type"
  "src/deptran/raft/commo.h|raft_commo.notify_restart_status"
  "src/deptran/raft/messages.hpp|raft_messages.append_entries_reply"
  "src/deptran/raft/messages.hpp|raft_messages.durable"
  "src/deptran/raft/messages.hpp|raft_messages.heartbeat"
  "src/deptran/raft/messages.hpp|raft_messages.install_snapshot_reply"
  "src/deptran/raft/messages.hpp|raft_messages.notify_restart"
  "src/deptran/raft/messages.hpp|raft_messages.remove_server_req"
  "src/deptran/raft/messages.hpp|raft_messages.timeout_now"
  "src/deptran/raft/messages.hpp|raft_messages.vote"
  "src/deptran/raft/read_raft_disk.cc|raft_disk.data_record"
  "src/deptran/raft/recovery_manager.hpp|raft_recovery.mode"
  "src/deptran/raft/server.h|raft_server.commit_status"
  "src/deptran/raft/server.h|raft_server.step_down_reason"
  "src/deptran/raft/snapshot_format.hpp|raft_snapshot.format_enums"
)

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
if ! GITLINK_ENTRY=$(git -C "${REPOSITORY_ROOT}" ls-files --stage -- \
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
if ! CHECKED_OUT_EMITTER_HASH=$(git -C "${RUSTYCPP_DIR}" rev-parse HEAD 2>/dev/null); then
  echo "cannot inspect the rusty-cpp checkout" >&2
  exit 2
fi
if [[ "${CHECKED_OUT_EMITTER_HASH}" != "${REQUIRED_RUSTY_CPP_COMMIT}" ]]; then
  echo "rusty-cpp checkout mismatch: expected ${REQUIRED_RUSTY_CPP_COMMIT}, got ${CHECKED_OUT_EMITTER_HASH}" >&2
  exit 2
fi
if [[ -n "$(git -C "${RUSTYCPP_DIR}" status --porcelain --untracked-files=no)" ]]; then
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
  if command -v rg >/dev/null 2>&1; then
    mapfile -t FILES < <(
      rg -l '#if RUSTYCPP_RUST' src/deptran/raft \
        -g '*.h' -g '*.hh' -g '*.hpp' -g '*.cc' -g '*.cpp' -g '*.cxx' |
        sort
    )
  else
    mapfile -t FILES < <(
      grep -rl '#if RUSTYCPP_RUST' src/deptran/raft \
        --include='*.h' --include='*.hh' --include='*.hpp' \
        --include='*.cc' --include='*.cpp' --include='*.cxx' |
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
    src/deptran/raft/*) ;;
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
  echo "rewrote ${#FILES[@]} Raft DSL carrier(s)"
  exit 0
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
if ! output=$("${TRANSPILER}" inline-rust --check --files "${FILES[@]}" 2>&1); then
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

for index in "${!FILES[@]}"; do
  file="${FILES[${index}]}"
  regenerated="${REGENERATED[${index}]}"
  if ! cmp -s "${file}" "${regenerated}"; then
    echo "DRIFT ${file} (committed GEN output)" >&2
    diff -u "${file}" "${regenerated}" | head -80 >&2 || true
    failures=$((failures + 1))
  fi
done

echo "checked ${#FILES[@]} Raft DSL carrier(s), ${failures} with drift"
exit $((failures > 0 ? 1 : 0))
