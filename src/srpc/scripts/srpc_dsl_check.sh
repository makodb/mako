#!/usr/bin/env bash
# Drift guard for the inline-Rust DSL in this SRPC repository.
#
# Each DSL block carries a `rust_sha256` of the Rust it was generated
# from. If the Rust is edited without regenerating, the block and the
# C++ the compiler actually sees disagree — and NOTHING detects that
# today: no independent regeneration script covers these SRPC carriers. Two
# blocks (client.1, server.1) historically drifted exactly this way.
#
# That matters most during the migration: converting hand-written C++
# into DSL means editing these blocks constantly, and a silent
# divergence means the Rust we later extract is a draft nothing ever
# compiled.
#
# ONE FILE PER INVOCATION, deliberately: `inline-rust --check` stops at
# the first mismatch in a file, so batching under-reports.
#
# Usage: scripts/srpc_dsl_check.sh [path/to/rusty-cpp-transpiler]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPOSITORY_ROOT}" || exit 2

TRANSPILER="${1:-${REPOSITORY_ROOT}/third-party/rusty-cpp/target/release/rusty-cpp-transpiler}"
if [[ ! -x "$TRANSPILER" ]]; then
  echo "no transpiler at $TRANSPILER" >&2
  exit 2
fi

EXPECTED_FILES=(
  reactor/epoll_platform_linux.cc
)
EXPECTED_FILE_COUNT=1
EXPECTED_BLOCK_COUNT=5

# --include='*.rs' is load-bearing even though the census below expects exactly
# one C++ carrier. The 37 canonical sources are now .rs; without that pattern
# this scan would silently cover 1 file instead of 38, and a DSL block growing
# in a canonical source would stop being drift-checked with no signal at all.
mapfile -t FILES < <(grep -rl '#if RUSTYCPP_RUST' base misc reactor rpc \
                       --include='*.rs' --include='*.cpp' --include='*.cc' \
                       --include='*.h' --include='*.hpp' | sort)

if [[ ${#FILES[@]} -ne $EXPECTED_FILE_COUNT ]] ||
   [[ "${FILES[*]}" != "${EXPECTED_FILES[*]}" ]]; then
  echo "inline-Rust carrier census mismatch" >&2
  echo "expected (${#EXPECTED_FILES[@]}): ${EXPECTED_FILES[*]}" >&2
  echo "actual (${#FILES[@]}): ${FILES[*]}" >&2
  exit 1
fi

block_count=0
for f in "${FILES[@]}"; do
  count=$(grep -c '^#if RUSTYCPP_RUST' "$f")
  block_count=$((block_count + count))
done
if [[ $block_count -ne $EXPECTED_BLOCK_COUNT ]]; then
  echo "inline-Rust block census mismatch: expected $EXPECTED_BLOCK_COUNT, got $block_count" >&2
  exit 1
fi

fail=0
for f in "${FILES[@]}"; do
  if ! out=$("$TRANSPILER" inline-rust --check --files "$f" 2>&1); then
    echo "DRIFT $f"
    echo "$out" | sed 's/^/    /' | head -4
    fail=$((fail + 1))
  fi
done

echo
echo "checked ${#FILES[@]} files / $block_count blocks, $fail with drift"
exit $(( fail > 0 ? 1 : 0 ))
