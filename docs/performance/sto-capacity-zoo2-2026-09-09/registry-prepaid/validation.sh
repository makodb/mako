#!/usr/bin/env bash
set -euxo pipefail
cd /home/users/shuai/mako/.claude/worktrees/sto-rust
date -u
sha256sum crates/sto-masstree/src/lib.rs crates/sto-masstree/src/registry_growth_tests.rs
rustc -Vv
cargo fmt --manifest-path crates/Cargo.toml -p sto-masstree -- --check
env -u MAKO_MTREE_NATIVE_INTEGRATION -u MAKO_MTREE_NATIVE_LIB_DIRS -u MAKO_MTREE_NATIVE_LIBS \
  TMPDIR=/dev/shm CARGO_TARGET_DIR=/dev/shm/mako-capacity-prepaid-target \
  cargo test --manifest-path crates/Cargo.toml -p sto-masstree --all-features --locked --offline
env -u MAKO_MTREE_NATIVE_INTEGRATION -u MAKO_MTREE_NATIVE_LIB_DIRS -u MAKO_MTREE_NATIVE_LIBS \
  TMPDIR=/dev/shm CARGO_TARGET_DIR=/dev/shm/mako-capacity-prepaid-target \
  cargo clippy --manifest-path crates/Cargo.toml -p sto-masstree --all-targets --all-features --locked --offline -- -D warnings
