#!/bin/bash
set -euo pipefail

cd "$HOME"

# 1.97.1 is the release mainline's native sharding requires exactly
# (src/cluster/Cargo.toml rust-version, scripts/verify_native_sharding.sh); the
# Rust STO workspace under crates/ only needs >= 1.95. Keep this in step with
# rust-toolchain.toml and Dockerfile.ubuntu24.
rust_version="1.97.1"
case "$(uname -m)" in
  x86_64|amd64)
    rust_target="x86_64-unknown-linux-gnu"
    ;;
  aarch64|arm64)
    rust_target="aarch64-unknown-linux-gnu"
    ;;
  *)
    echo "unsupported Rust host architecture: $(uname -m)" >&2
    exit 2
    ;;
esac
rust_archive="rust-${rust_version}-${rust_target}.tar.gz"
rust_src_archive="rust-src-${rust_version}.tar.gz"
clippy_archive="clippy-${rust_version}-${rust_target}.tar.gz"
rustfmt_archive="rustfmt-${rust_version}-${rust_target}.tar.gz"
install_prefix="$HOME/.local-rust"

# Each archive is verified against the checksum the release publishes beside
# it, as scripts/ci/install_rust_toolchain.sh does. The 1.95.0 digests this
# script used to embed do not apply to 1.97.1; re-embed fixed digests here once
# they have been recorded from a trusted fetch.
download_and_verify() {
  local archive="$1"
  curl --proto '=https' --tlsv1.2 -fL --retry 3 \
    --output "$archive" "https://static.rust-lang.org/dist/${archive}"
  curl --proto '=https' --tlsv1.2 -fL --retry 3 \
    --output "${archive}.sha256" \
    "https://static.rust-lang.org/dist/${archive}.sha256"
  sha256sum --check --strict "${archive}.sha256"
}

download_and_verify "$rust_archive"
download_and_verify "$rust_src_archive"
download_and_verify "$clippy_archive"
download_and_verify "$rustfmt_archive"

tar xzf "$rust_archive"
tar xzf "$rust_src_archive"
tar xzf "$clippy_archive"
tar xzf "$rustfmt_archive"

mkdir -p "$install_prefix"

"$HOME/rust-${rust_version}-${rust_target}/install.sh" --prefix="$install_prefix"
"$HOME/rust-src-${rust_version}/install.sh" --prefix="$install_prefix"
"$HOME/clippy-${rust_version}-${rust_target}/install.sh" --prefix="$install_prefix"
"$HOME/rustfmt-${rust_version}-${rust_target}/install.sh" --prefix="$install_prefix"

if [ -f "$HOME/.bashrc" ]; then
  if ! grep -Fq 'export PATH="$HOME/.local-rust/bin:$PATH"' "$HOME/.bashrc"; then
    echo '[ -d "$HOME/.local-rust/bin" ] && export PATH="$HOME/.local-rust/bin:$PATH"' >> "$HOME/.bashrc"
  fi
elif [ -f "$HOME/.zshrc" ]; then
  if ! grep -Fq 'export PATH="$HOME/.local-rust/bin:$PATH"' "$HOME/.zshrc"; then
    echo '[ -d "$HOME/.local-rust/bin" ] && export PATH="$HOME/.local-rust/bin:$PATH"' >> "$HOME/.zshrc"
  fi
fi

export PATH="$HOME/.local-rust/bin:$PATH"
"$HOME/.local-rust/bin/rustc" --version
"$HOME/.local-rust/bin/cargo" clippy --version
"$HOME/.local-rust/bin/rustfmt" --version
test -r "$HOME/.local-rust/lib/rustlib/src/rust/library/core/src/marker.rs"
