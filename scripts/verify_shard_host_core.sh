#!/usr/bin/env bash
# The proof gate of crates/shard-host-core, the verified core of the sharding
# storage host on the Rust engine (shell: crates/sto-tpcc-ffi/src/shard_host.rs).
#
# Run it inside the build container, from anywhere in the checkout:
#     ./docker_build.sh enter
#     ./scripts/verify_shard_host_core.sh
#
# It needs the repository, the container's Rust 1.97.1 and, the first time,
# the network: the pinned Verus release (checked by sha256, cached under
# $BUILD_DIR/proof-tools, shared with scripts/verify_native_sharding.sh) and
# the crates crates/Cargo.lock names.
#
# What passes here:
#   - the source audit, the pin check, the proof with --no-cheating and the
#     negative controls of scripts/shard_host_core_proof.py;
#   - a plain cargo build of the core and of its shell. The shell holds
#     compile-time assertions that the status numbers the proof speaks about
#     are the engine's, so the proof is not accepted without that build.
#
# What does not: the shell itself. It is not verified. That it calls the core
# at the right moment, hands it the spans the caller sent and acts on the
# answer as the answer says is read, not proved. The functions this is true
# of are listed in scripts/shard_host_core_trusted.txt.
set -euo pipefail
if [[ ! -f /.dockerenv && ! -f /run/.containerenv ]]; then
    echo 'Run inside the build container (./docker_build.sh enter).' >&2
    exit 1
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
# The release and its checksum are the ones of scripts/verify_native_sharding.sh.
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
# The image installs the official Rust tarball without rustup; see
# scripts/verify_native_sharding.sh.
rust_version="$(rustc --version)"
if [[ "$rust_version" != "rustc 1.97.1 "* ]]; then
    echo "Pinned Verus requires official Rust 1.97.1; found: $rust_version" >&2
    exit 1
fi
export VERUS_USE_RUSTUP=0
export VERUS_SYSROOT="$(rustc --print sysroot)"
export LD_LIBRARY_PATH="$VERUS_SYSROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$VERUS_PATH" --version
python3 scripts/shard_host_core_proof.py
echo "shard-host-core: proof gate passed"
