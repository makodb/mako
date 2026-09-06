#!/usr/bin/env bash

set -euo pipefail

# Miri follows nightly Rust. Pin the date so local and CI runs execute the same
# interpreter and standard library. Update this together with a clean full run.
readonly RUST_STO_MIRI_TOOLCHAIN="nightly-2026-08-12"

if [ "${1:-}" = "--print-toolchain" ]; then
    printf '%s\n' "${RUST_STO_MIRI_TOOLCHAIN}"
    exit 0
fi
if [ "$#" -ne 0 ]; then
    echo "usage: $0 [--print-toolchain]" >&2
    exit 2
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
CARGO_BIN=${CARGO:-cargo}
MANIFEST="${REPOSITORY_ROOT}/crates/Cargo.toml"
QUARANTINE_ALLOWLIST="${REPOSITORY_ROOT}/scripts/ci/rust_sto_quarantine_tests.txt"

if [[ -n "${MIRIFLAGS:-}" ]]; then
    echo "ambient MIRIFLAGS must be unset for the Rust STO Miri evidence gate" >&2
    exit 2
fi
if [[ ! -r "${QUARANTINE_ALLOWLIST}" ]]; then
    echo "Rust STO quarantine allowlist is missing: ${QUARANTINE_ALLOWLIST}" >&2
    exit 2
fi
QUARANTINE_CASES=()
declare -A QUARANTINE_IDENTITIES_SEEN=()
declare -A QUARANTINE_NAMES_SEEN=()
while IFS= read -r QUARANTINE_CASE || [[ -n "${QUARANTINE_CASE}" ]]; do
    [[ -z "${QUARANTINE_CASE}" || "${QUARANTINE_CASE}" == \#* ]] && continue
    if [[ ! "${QUARANTINE_CASE}" =~ ^[A-Za-z0-9_.-]+\|[A-Za-z0-9_.-]+\|[A-Za-z0-9_.:-]+$ ]]; then
        echo "invalid Rust STO quarantine allowlist entry: ${QUARANTINE_CASE}" >&2
        exit 2
    fi
    IFS='|' read -r QUARANTINE_PACKAGE QUARANTINE_SUITE QUARANTINE_NAME \
        <<<"${QUARANTINE_CASE}"
    if [[ -n "${QUARANTINE_IDENTITIES_SEEN[${QUARANTINE_CASE}]:-}" ]]; then
        echo "duplicate Rust STO quarantine identity: ${QUARANTINE_CASE}" >&2
        exit 2
    fi
    if [[ -n "${QUARANTINE_NAMES_SEEN[${QUARANTINE_NAME}]:-}" ]]; then
        echo "quarantine test name is not globally unique: ${QUARANTINE_NAME}" >&2
        exit 2
    fi
    QUARANTINE_IDENTITIES_SEEN["${QUARANTINE_CASE}"]=1
    QUARANTINE_NAMES_SEEN["${QUARANTINE_NAME}"]=1
    QUARANTINE_CASES+=("${QUARANTINE_CASE}")
done <"${QUARANTINE_ALLOWLIST}"
if [[ "${#QUARANTINE_CASES[@]}" -eq 0 ]]; then
    echo "Rust STO quarantine allowlist is empty: ${QUARANTINE_ALLOWLIST}" >&2
    exit 2
fi
readonly -a QUARANTINE_CASES

for REQUIRED_COMMAND in "${CARGO_BIN}" python3; do
    if ! command -v "${REQUIRED_COMMAND}" >/dev/null 2>&1; then
        echo "Rust STO Miri gate: required command is unavailable: ${REQUIRED_COMMAND}" >&2
        exit 1
    fi
done

if ! "${CARGO_BIN}" "+${RUST_STO_MIRI_TOOLCHAIN}" miri --version \
    >/dev/null 2>&1; then
    cat >&2 <<EOF
Rust STO Miri gate requires ${RUST_STO_MIRI_TOOLCHAIN} with Miri.
Install it with:
  rustup toolchain install ${RUST_STO_MIRI_TOOLCHAIN} --profile minimal --component miri --component rust-src
EOF
    exit 1
fi

export CARGO_TARGET_DIR=${CARGO_TARGET_DIR:-"${REPOSITORY_ROOT}/build-rust-sto-miri"}

run_miri() {
    "${CARGO_BIN}" "+${RUST_STO_MIRI_TOOLCHAIN}" miri test \
        --locked --manifest-path "${MANIFEST}" "$@"
}

run_miri_with_quarantine_leaks() {
    MIRIFLAGS="-Zmiri-ignore-leaks" \
        "${CARGO_BIN}" "+${RUST_STO_MIRI_TOOLCHAIN}" miri test \
        --locked --manifest-path "${MANIFEST}" "$@"
}

audit_miri_suite_quarantine() {
    local package="$1"
    local suite="$2"
    shift 2
    local list_dir="${CARGO_TARGET_DIR}/quarantine-audit"
    local list_log="${list_dir}/${package}-${suite}.log"

    mkdir -p "${list_dir}"
    run_miri -p "${package}" --test "${suite}" -- \
        --list --format terse 2>&1 | tee "${list_log}"
    python3 - "${list_log}" "$@" <<'PY'
import pathlib
import sys

listed = [
    line.removesuffix(": test")
    for line in pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
    if line.endswith(": test")
]
if not listed:
    raise SystemExit("Miri suite listing returned no tests")
for identity in sys.argv[2:]:
    package, suite, expected = identity.split("|", 2)
    matches = [name for name in listed if expected in name]
    if matches != [expected]:
        raise SystemExit(
            f"Miri --skip {expected!r} from {package}|{suite} matched "
            f"{matches!r}, expected exactly [{expected!r}]"
        )
print(f"verified {len(sys.argv) - 2} Miri quarantine skip/rerun targets")
PY
}

# These failure-injection cases deliberately retain state after ownership or
# publication becomes uncertain. Audit the test harness's substring matching,
# run every other case with leak checking, then qualify only the exact cases.
declare -A QUARANTINE_SUITES_SEEN=()
QUARANTINE_SUITE_KEYS=()
for QUARANTINE_CASE in "${QUARANTINE_CASES[@]}"; do
    IFS='|' read -r QUARANTINE_PACKAGE QUARANTINE_SUITE QUARANTINE_NAME \
        <<<"${QUARANTINE_CASE}"
    QUARANTINE_SUITE_KEY="${QUARANTINE_PACKAGE}|${QUARANTINE_SUITE}"
    if [[ -n "${QUARANTINE_SUITES_SEEN[${QUARANTINE_SUITE_KEY}]:-}" ]]; then
        continue
    fi
    QUARANTINE_SUITES_SEEN["${QUARANTINE_SUITE_KEY}"]=1
    QUARANTINE_SUITE_KEYS+=("${QUARANTINE_SUITE_KEY}")
    SUITE_CASES=()
    for SUITE_CASE in "${QUARANTINE_CASES[@]}"; do
        if [[ "${SUITE_CASE}" == "${QUARANTINE_SUITE_KEY}|"* ]]; then
            SUITE_CASES+=("${SUITE_CASE}")
        fi
    done
    audit_miri_suite_quarantine "${QUARANTINE_PACKAGE}" \
        "${QUARANTINE_SUITE}" "${SUITE_CASES[@]}"
done

run_miri -p masstree --lib
run_miri -p sto-core --lib

for QUARANTINE_SUITE_KEY in "${QUARANTINE_SUITE_KEYS[@]}"; do
    IFS='|' read -r QUARANTINE_PACKAGE QUARANTINE_SUITE \
        <<<"${QUARANTINE_SUITE_KEY}"
    SUITE_CASES=()
    SUITE_SKIP_ARGS=()
    for SUITE_CASE in "${QUARANTINE_CASES[@]}"; do
        if [[ "${SUITE_CASE}" == "${QUARANTINE_SUITE_KEY}|"* ]]; then
            SUITE_CASES+=("${SUITE_CASE}")
            IFS='|' read -r _ _ SUITE_TEST <<<"${SUITE_CASE}"
            SUITE_SKIP_ARGS+=(--skip "${SUITE_TEST}")
        fi
    done
    run_miri -p "${QUARANTINE_PACKAGE}" --test "${QUARANTINE_SUITE}" -- \
        "${SUITE_SKIP_ARGS[@]}"
    for SUITE_CASE in "${SUITE_CASES[@]}"; do
        IFS='|' read -r _ _ SUITE_TEST <<<"${SUITE_CASE}"
        run_miri_with_quarantine_leaks -p "${QUARANTINE_PACKAGE}" \
            --test "${QUARANTINE_SUITE}" "${SUITE_TEST}" -- \
                --exact --include-ignored
    done
done

# With native integration disabled, these unit tests exercise codecs, cache
# ownership, panic containment, and every hostile raw-range validator without
# asking Miri to execute the external Masstree library. The skipped statistical
# hash-distribution loop has no unsafe access and remains in the native gate.
run_miri -p sto-tpcc-ffi --lib -- \
    --skip resolved_cache_hash_distributes_big_endian_customer_ids

# The 128-seed history sweep and three high-iteration concurrency stress tests
# belong in the native/TSan gate and are prohibitively slow under the
# interpreter. The remaining 185 tests retain the smaller concurrency cases
# plus registry, publication, record-address, scan, and ownership coverage.
run_miri -p sto-masstree --all-features --lib -- \
    --skip history_tests:: \
    --skip tests::concurrent_inline_and_shared_publications_never_expose_torn_bytes \
    --skip tests::concurrent_bounded_publications_never_expose_torn_bytes \
    --skip tests::concurrent_ready_resolution_survives_the_publication_transition_in_place
