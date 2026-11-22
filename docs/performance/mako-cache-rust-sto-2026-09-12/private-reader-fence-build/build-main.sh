#!/usr/bin/env bash
set -euo pipefail
variant=${1:?expected base or reader}
arm=${2:?expected c-abi or sto}
case "$variant" in base|reader) ;; *) exit 2;; esac
case "$arm" in c-abi|sto) ;; *) exit 2;; esac
export RUSTUP_TOOLCHAIN=1.95.0
export CARGO_TARGET_DIR=/var/tmp/mako-engine-perf-20260912.trsFuD/target-$arm-main
export CARGO_BUILD_JOBS=2
export CARGO_PROFILE_RELEASE_LTO=false
export MAKO_BUILD_DIR=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22
export MAKO_LOCAL_REQUIRE_NATIVE=1 MAKO_LOCAL_FAKE_ABI=0
export MAKO_MTREE_NATIVE_INTEGRATION=1
export MAKO_MTREE_NATIVE_LIB_DIRS=$MAKO_BUILD_DIR:$MAKO_BUILD_DIR/src/masstree:/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export MAKO_MTREE_NATIVE_LIBS=static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread
export LIBCXX_DIR=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export LIBCLANG_PATH=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export PYTHON=/usr/bin/python3
export ROCKSDB_LIB_DIR=/var/tmp/mako-engine-perf-20260912.trsFuD/rocks
export RUSTFLAGS='-C target-cpu=znver1 -C force-frame-pointers=yes -C debuginfo=1 -C linker=/var/tmp/mako-engine-perf-20260912.trsFuD/linker.sh'
cd /home/users/shuai/mako/.claude/worktrees/masstree-rocks/.mako-read-fence-$variant-crates
cargo build --locked --release -p mako-cache-bench --no-default-features --features engine-$arm --bin mako-cache-bench
cp "$CARGO_TARGET_DIR/release/mako-cache-bench" /var/tmp/mako-read-fence.rNqyuM/main-$variant-$arm
