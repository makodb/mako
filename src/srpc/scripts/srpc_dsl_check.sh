#!/usr/bin/env bash
# Canonical Rust is the only source for generated C++ module behavior.
# The optional legacy transpiler argument is accepted for CMake compatibility.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPOSITORY_ROOT}"

for directory in base misc reactor rpc; do
  if [[ ! -d "$directory" ]]; then
    echo "missing canonical source directory: $directory" >&2
    exit 2
  fi
done

if matches=$(rg -n '#[[:space:]]*if[[:space:]]+RUSTYCPP_RUST|RUSTYCPP:GEN-BEGIN' \
    base misc reactor rpc -g '*.rs' -g '*.cpp' -g '*.cc' -g '*.h' -g '*.hpp'); then
  echo "inline-Rust carrier census mismatch: expected zero carriers" >&2
  echo "$matches" >&2
  exit 1
else
  status=$?
  if [[ $status -ne 1 ]]; then
    echo "canonical source scan failed" >&2
    exit "$status"
  fi
fi

echo "checked canonical sources: zero inline-Rust carriers"
