#!/usr/bin/env bash
# Drift guard for the inline-Rust DSL in src/rrr.
#
# Each DSL block carries a `rust_sha256` of the Rust it was generated
# from. If the Rust is edited without regenerating, the block and the
# C++ the compiler actually sees disagree — and NOTHING detects that
# today: scripts/regen_storage_dsl.sh has zero src/rrr entries. Two
# blocks (client.1, server.1) drifted exactly this way.
#
# That matters most during the migration: converting hand-written C++
# into DSL means editing these blocks constantly, and a silent
# divergence means the Rust we later extract is a draft nothing ever
# compiled.
#
# ONE FILE PER INVOCATION, deliberately: `inline-rust --check` stops at
# the first mismatch in a file, so batching under-reports.
#
# Usage: scripts/rrr_dsl_check.sh [path/to/rusty-cpp-transpiler]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPOSITORY_ROOT}" || exit 2

TRANSPILER="${1:-${REPOSITORY_ROOT}/third-party/rusty-cpp/target/release/rusty-cpp-transpiler}"
if [[ ! -x "$TRANSPILER" ]]; then
  echo "no transpiler at $TRANSPILER" >&2
  exit 2
fi

mapfile -t FILES < <(grep -rl '#if RUSTYCPP_RUST' src/rrr --include='*.cpp' \
                       --include='*.cc' --include='*.h' --include='*.hpp' | sort)

fail=0
for f in "${FILES[@]}"; do
  if ! out=$("$TRANSPILER" inline-rust --check --files "$f" 2>&1); then
    echo "DRIFT $f"
    echo "$out" | sed 's/^/    /' | head -4
    fail=$((fail + 1))
  fi
done

echo
echo "checked ${#FILES[@]} files, $fail with drift"
exit $(( fail > 0 ? 1 : 0 ))
