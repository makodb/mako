#!/usr/bin/env bash
set -euxo pipefail
cd /home/users/shuai/mako/.claude/worktrees/sto-rust
date -u
sha256sum crates/sto-masstree/src/lib.rs crates/sto-masstree/src/registry_growth_tests.rs
rustup run nightly-2026-08-12 rustc -Vv
cargo +nightly-2026-08-12 miri --version
env -u MIRIFLAGS -u MAKO_MTREE_NATIVE_INTEGRATION -u MAKO_MTREE_NATIVE_LIB_DIRS -u MAKO_MTREE_NATIVE_LIBS \
  TMPDIR=/dev/shm CARGO_TARGET_DIR=/dev/shm/mako-capacity-prepaid-miri \
  cargo +nightly-2026-08-12 miri test --manifest-path crates/Cargo.toml \
    -p sto-masstree --lib --all-features --locked --offline registry_growth_tests \
    -- --test-threads=1
