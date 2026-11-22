#!/usr/bin/env bash
# Shared by Docker image builds and CI jobs whose published image may be older.
set -euo pipefail
version="${1:?Usage: install_rust_toolchain.sh VERSION [PREFIX]}"
prefix="${2:-/opt/rust}"

if [[ -x "$prefix/bin/rustc" ]] && \
   [[ "$("$prefix/bin/rustc" --version)" == "rustc $version "* ]]; then
    "$prefix/bin/rustc" --version
    "$prefix/bin/cargo" --version
    exit 0
fi

case "$(uname -m)" in
    x86_64) target=x86_64-unknown-linux-gnu ;;
    aarch64) target=aarch64-unknown-linux-gnu ;;
    *) echo "Unsupported Rust host architecture: $(uname -m)" >&2; exit 1 ;;
esac

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
archive="rust-$version-$target.tar.gz"
url="https://static.rust-lang.org/dist/$archive"
curl --fail --location --proto '=https' --tlsv1.2 "$url" --output "$work/$archive"
curl --fail --location --proto '=https' --tlsv1.2 "$url.sha256" --output "$work/$archive.sha256"
(cd "$work" && sha256sum --check "$archive.sha256")
tar -xzf "$work/$archive" -C "$work"
"$work/rust-$version-$target/install.sh" --prefix="$prefix" --without=rust-docs
"$prefix/bin/rustc" --version
"$prefix/bin/cargo" --version
