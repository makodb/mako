#!/usr/bin/env bash
set -euo pipefail
variant=${1:?variant name}
case "$variant" in holder|rcu) ;; *) exit 2;; esac
unset CARGO_BUILD_TARGET CARGO_ENCODED_RUSTFLAGS
export RUSTUP_TOOLCHAIN=1.95.0
export CARGO_TARGET_DIR="/var/tmp/mako-residual-gap.IvBRsm/target-$variant"
export CARGO_BUILD_JOBS=2 CARGO_PROFILE_RELEASE_LTO=thin
export MAKO_BUILD_DIR=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22
export MAKO_LOCAL_REQUIRE_NATIVE=1 MAKO_LOCAL_FAKE_ABI=0
export MAKO_MTREE_NATIVE_INTEGRATION=1
export MAKO_MTREE_NATIVE_LIB_DIRS="$MAKO_BUILD_DIR:$MAKO_BUILD_DIR/src/masstree:/home/users/shuai/.linuxbrew/opt/llvm@22/lib"
export MAKO_MTREE_NATIVE_LIBS=static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread
export LIBCXX_DIR=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export LIBCLANG_PATH=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export PYTHON=/usr/bin/python3
export ROCKSDB_LIB_DIR=/var/tmp/mako-engine-perf-20260912.trsFuD/rocks
export RUSTFLAGS='-C target-cpu=znver1 -C force-frame-pointers=yes -C debuginfo=1 -C linker=/var/tmp/mako-engine-perf-20260912.trsFuD/linker.sh'
cargo build --locked --release --manifest-path "/var/tmp/mako-residual-gap.IvBRsm/$variant-source/crates/Cargo.toml" \
    -p mako-cache-bench --no-default-features --features engine-sto --bin engine_compare
