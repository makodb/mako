#!/usr/bin/env bash
set -euxo pipefail
cd /home/users/shuai/mako/.claude/worktrees/sto-rust
date -u
git rev-parse HEAD
sha256sum crates/Cargo.lock crates/sto-masstree/src/lib.rs crates/sto-masstree/src/registry_growth_tests.rs crates/sto-masstree/src/fixed_u64.rs
rustup run nightly-2026-08-12 rustc -Vv
rustup run nightly-2026-08-12 cargo -V
/home/users/shuai/.linuxbrew/opt/llvm@22/bin/clang --version
# Reuse the earlier full-std TSan target, rebuilding the changed registry crate.
env -u RUSTC -u RUSTC_WRAPPER -u RUSTC_WORKSPACE_WRAPPER \
  -u CARGO_ENCODED_RUSTFLAGS -u CARGO_BUILD_RUSTFLAGS \
  -u CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_RUSTFLAGS \
  -u MAKO_MTREE_NATIVE_INTEGRATION -u MAKO_MTREE_NATIVE_LIB_DIRS -u MAKO_MTREE_NATIVE_LIBS \
  RUSTUP_TOOLCHAIN=nightly-2026-08-12 \
  CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=/home/users/shuai/.linuxbrew/opt/llvm@22/bin/clang \
  RUSTFLAGS='-Zsanitizer=thread -Zexternal-clangrt -C force-frame-pointers=yes -C link-arg=-fsanitize=thread' \
  RUSTDOCFLAGS='-Zsanitizer=thread -Zexternal-clangrt -C force-frame-pointers=yes -C link-arg=-fsanitize=thread' \
  TSAN_OPTIONS='halt_on_error=1:print_suppressions=1:second_deadlock_stack=1' \
  TMPDIR=/dev/shm \
  CARGO_TARGET_DIR=/dev/shm/mako-registry-tsan-20260909.BUDV4D \
  LD_LIBRARY_PATH=/home/users/shuai/.linuxbrew/opt/llvm@22/lib \
  cargo test --manifest-path crates/Cargo.toml --offline --locked \
    --target x86_64-unknown-linux-gnu -Zbuild-std \
    -p sto-masstree --lib --all-features registry_growth_tests \
    -- --test-threads=1 --nocapture
