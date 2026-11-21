#!/usr/bin/env bash
set -euo pipefail
proof_workspace=${1:-/var/tmp/mako-resolved-health.lt7kUf/cache}
proof_target=${2:-/var/tmp/mako-native-health-proof.SMAbQK/target-fixed}
unset CARGO_BUILD_TARGET CARGO_ENCODED_RUSTFLAGS
export RUSTUP_TOOLCHAIN=1.95.0
export CARGO_TARGET_DIR="$proof_target"
export CARGO_BUILD_JOBS=4
export TMPDIR=/var/tmp/mako-resolved-health.lt7kUf/tmp
export MAKO_BUILD_DIR=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22
export MAKO_LOCAL_REQUIRE_NATIVE=1 MAKO_LOCAL_FAKE_ABI=0
export MAKO_MTREE_NATIVE_INTEGRATION=1
export MAKO_MTREE_NATIVE_LIB_DIRS=/var/tmp/mako-native-health-proof.SMAbQK:/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22:/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/src/masstree:/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export MAKO_MTREE_NATIVE_LIBS=static=mtree_health_proof,static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread
export LIBCLANG_PATH=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export LD_LIBRARY_PATH=/home/users/shuai/.linuxbrew/opt/llvm@22/lib:/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/third-party/yaml-cpp
export RUSTFLAGS='--cfg mako_native_health_proof -C target-cpu=znver1 -C force-frame-pointers=yes -C debuginfo=1 -C linker=/var/tmp/mako-engine-perf-20260912.trsFuD/linker.sh'
exec cargo test --locked --manifest-path "$proof_workspace/crates/Cargo.toml" \
    -p mako-engine-sto --features test-hooks --lib \
    txn::native_health_proof::global_native_poison_rejects_cross_db_cached_put \
    -- --exact --test-threads=1 --nocapture
