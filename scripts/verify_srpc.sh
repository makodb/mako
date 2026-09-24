#!/usr/bin/env bash
#
# Verify srpc's in-place Verus specs with `cargo verus verify`.
#
# Leaf modules carry `#[cfg(verus_only)]`-gated Verus annotations (see
# misc/stat.rs). They are invisible to `cargo build`/`cargo test` and to the
# rusty-cpp C++ transpile, because only `cargo verus verify` sets `verus_only`.
#
# The verify/ package is a separate, workspace-excluded crate whose modules are
# `#[path]` links to the REAL srpc sources -- the same bytes rustc compiles and
# rusty-cpp translates -- so this checks the actual code in place, not a copy.
# verify/ is the only crate that depends on vstd, keeping it out of production.
#
# Requirements:
#   - `cargo-verus` and `verus` on PATH (a prebuilt Verus dist provides both),
#     or point at a dist explicitly:  VERUS_HOME=/path/to/verus-x86-linux
#   - the vstd / verus_* registry crates resolvable (crates.io or a vendor dir)
#
# Usage:  scripts/verify_srpc.sh
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Allow VERUS_HOME to prepend a dist to PATH (it ships both cargo-verus + verus).
if [ -n "${VERUS_HOME:-}" ]; then
    export PATH="$VERUS_HOME:$PATH"
fi

if ! command -v cargo-verus >/dev/null 2>&1; then
    echo "verify_srpc: cargo-verus not found (put a Verus dist on PATH or set VERUS_HOME)" >&2
    exit 2
fi

cd "$HERE/verify" || exit 2
exec cargo verus verify "$@"
