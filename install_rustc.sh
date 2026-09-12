#!/bin/bash
set -euo pipefail

cd "$HOME"

rust_version="1.95.0"
case "$(uname -m)" in
  x86_64|amd64)
    rust_target="x86_64-unknown-linux-gnu"
    rust_archive_sha256="a47ac940abd12399d59ad15c877e7113fa35f2b9ec7e6a8a045d4fd8b9741dea"
    clippy_archive_sha256="5230c92fb0ae1346ee30a1b4e01413cc3e1ead8ded20391fb2826619775f1ccb"
    rustfmt_archive_sha256="6d3e64adc505ad4bef6935f6e6f5e4c6956d9782607fb8394df3a5d2b30d2733"
    ;;
  aarch64|arm64)
    rust_target="aarch64-unknown-linux-gnu"
    rust_archive_sha256="3b9385d3144ac57616befa0ccbac524f857ba1b4ab074226e73a24d43568a98e"
    clippy_archive_sha256="84480abfbfed89616b1e0b35acb9db84766f0353b671766638084f4e9b1d1143"
    rustfmt_archive_sha256="ee169a16fb2a415aef71fba78fb279b7e0bb875dfecd56cdb80bdb95ba29a559"
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
rust_src_archive_sha256="98548815569318eb60afe7189ace6bca4ba6e4ae59a54f111d276ab78d6ddd10"
install_prefix="$HOME/.local-rust"

download_and_verify() {
  local archive="$1"
  local expected_sha256="$2"
  curl --proto '=https' --tlsv1.2 -fL --retry 3 \
    --output "$archive" "https://static.rust-lang.org/dist/${archive}"
  printf '%s  %s\n' "$expected_sha256" "$archive" \
    | sha256sum --check --strict
}

download_and_verify "$rust_archive" "$rust_archive_sha256"
download_and_verify "$rust_src_archive" "$rust_src_archive_sha256"
download_and_verify "$clippy_archive" "$clippy_archive_sha256"
download_and_verify "$rustfmt_archive" "$rustfmt_archive_sha256"

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
