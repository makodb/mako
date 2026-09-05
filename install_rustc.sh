#!/bin/bash
set -euo pipefail

cd "$HOME"

rust_version="1.95.0"
rust_target="x86_64-unknown-linux-gnu"
rust_archive="rust-${rust_version}-${rust_target}.tar.gz"
rust_src_archive="rust-src-${rust_version}.tar.gz"
install_prefix="$HOME/.local-rust"

curl -fLO "https://static.rust-lang.org/dist/${rust_archive}"
curl -fLO "https://static.rust-lang.org/dist/${rust_src_archive}"

tar xzf "$rust_archive"
tar xzf "$rust_src_archive"

mkdir -p "$install_prefix"

"$HOME/rust-${rust_version}-${rust_target}/install.sh" --prefix="$install_prefix"
"$HOME/rust-src-${rust_version}/install.sh" --prefix="$install_prefix"

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
test -r "$HOME/.local-rust/lib/rustlib/src/rust/library/core/src/marker.rs"
