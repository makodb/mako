#!/usr/bin/env bash

set -euo pipefail

# Miri follows the nightly compiler and can change semantics independently of
# the project's stable MSRV. Pin the ownership gate so local and CI results are
# reproducible. Update this date deliberately, together with a green gate run.
readonly MAKO_LOCAL_MIRI_TOOLCHAIN="nightly-2026-08-12"

if [ "${1:-}" = "--print-toolchain" ]; then
    printf '%s\n' "${MAKO_LOCAL_MIRI_TOOLCHAIN}"
    exit 0
fi
if [ "$#" -ne 0 ]; then
    echo "usage: $0 [--print-toolchain]" >&2
    exit 2
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
CARGO_BIN=${CARGO:-cargo}

if ! command -v "${CARGO_BIN}" >/dev/null 2>&1; then
    echo "mako-local Miri gate: cargo is unavailable: ${CARGO_BIN}" >&2
    exit 1
fi

if ! "${CARGO_BIN}" "+${MAKO_LOCAL_MIRI_TOOLCHAIN}" miri --version \
    >/dev/null 2>&1; then
    cat >&2 <<EOF
mako-local Miri gate requires ${MAKO_LOCAL_MIRI_TOOLCHAIN} with Miri.
Install it with:
  rustup toolchain install ${MAKO_LOCAL_MIRI_TOOLCHAIN} --profile minimal --component miri --component rustfmt --component rust-src
The gate never falls back to an unpinned nightly or an ordinary cargo test.
EOF
    exit 1
fi

# mako-local-sys generates its declarations with bindgen. Bindgen invokes
# rustfmt and can otherwise leave an empty/invalid generated surface after only
# an internal formatting diagnostic, obscuring the actual missing component.
if ! "${CARGO_BIN}" "+${MAKO_LOCAL_MIRI_TOOLCHAIN}" fmt --version \
    >/dev/null 2>&1; then
    cat >&2 <<EOF
mako-local Miri gate requires rustfmt for ${MAKO_LOCAL_MIRI_TOOLCHAIN}.
Install it with:
  rustup component add --toolchain ${MAKO_LOCAL_MIRI_TOOLCHAIN} rustfmt
EOF
    exit 1
fi

export MAKO_LOCAL_FAKE_ABI=1
export MAKO_LOCAL_REQUIRE_NATIVE=0
export CARGO_TARGET_DIR=${CARGO_TARGET_DIR:-"${REPOSITORY_ROOT}/build_mako_local_miri"}

exec "${CARGO_BIN}" "+${MAKO_LOCAL_MIRI_TOOLCHAIN}" miri test \
    --locked \
    --manifest-path "${REPOSITORY_ROOT}/crates/Cargo.toml" \
    -p mako-local \
    --lib
