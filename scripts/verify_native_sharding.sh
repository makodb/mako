#!/usr/bin/env bash
# Docker entry: ./docker_build.sh ci nativeShardingProof
# Whole-crate positives; only negative controls use module/function filters.
# No trusted-handler exemptions or resource overrides.
set -euo pipefail
if [[ ! -f /.dockerenv && ! -f /run/.containerenv ]]; then
    echo 'Run ./docker_build.sh ci nativeShardingProof (Docker is required).' >&2
    exit 1
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
version=0.2026.08.02.b677dd5
sha256=4c769256e888ee84bde85aae44d95c46bccbb8cf70e1d09f537b0d05fe965dee
cache="${BUILD_DIR:-build_docker}/proof-tools"
mkdir -p "$cache"
archive="$cache/verus-$version-x86-linux.zip"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --proto '=https' --tlsv1.2 \
        "https://github.com/verus-lang/verus/releases/download/release/$version/verus-$version-x86-linux.zip" \
        --output "$archive.download"
    mv "$archive.download" "$archive"
fi
printf '%s  %s\n' "$sha256" "$archive" | sha256sum --check --status
# Reextract the attested archive, rather than trusting a mutable cached binary.
tools="$(mktemp -d "$cache/verified.XXXXXXXX")"
trap 'rm -rf "$tools"' EXIT
python3 -m zipfile -e "$archive" "$tools"
chmod +x "$tools/verus-x86-linux/verus" "$tools/verus-x86-linux/rust_verify" "$tools/verus-x86-linux/z3"
export VERUS_PATH="$(cd "$tools/verus-x86-linux" && pwd)/verus"
# This release's real launcher supports VERUS_USE_RUSTUP=0. The development
# image installs the official Rust tarball in /opt/rust, intentionally without
# rustup. Require the exact compiler, expose its real driver library, and pass
# its sysroot explicitly to every production/model/control verification.
rust_version="$(rustc --version)"
if [[ "$rust_version" != "rustc 1.97.1 "* ]]; then
    echo "Pinned Verus requires official Rust 1.97.1; found: $rust_version" >&2
    echo 'Rebuild Dockerfile.ubuntu24 with RUST_VERSION=1.97.1.' >&2
    exit 1
fi
export VERUS_USE_RUSTUP=0
export VERUS_SYSROOT="$(rustc --print sysroot)"
export LD_LIBRARY_PATH="$VERUS_SYSROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$VERUS_PATH" --version
python3 scripts/native_sharding_proof_sources.py
# The same lib.rs and executable modules feed cargo and Verus. This is the
# production verification; the independent specification below is additional.
python3 tla/mako/scripts/verify_controls.py --native
# This runner first verifies the complete unchanged independent model, then
# demands semantic (not compile/timeout/resource) failure of its model mutants.
python3 tla/mako/scripts/verify_controls.py
cargo test --locked --manifest-path src/cluster/Cargo.toml \
    --target-dir "${BUILD_DIR:-build_docker}/native-sharding"
