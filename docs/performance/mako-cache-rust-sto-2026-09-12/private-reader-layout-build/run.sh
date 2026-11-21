#!/usr/bin/env bash
set -euo pipefail
kind=${1:?expected probe or build}
variant=${2:?expected base reader or matched}
arm=${3:?expected c-abi or sto}
case "$arm" in c-abi|sto) ;; *) exit 2;; esac
case "$kind:$variant" in
  probe:base) source=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/.mako-reader-layout-base-probe.bwOApZ ;;
  probe:reader) source=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/.mako-reader-layout-reader-probe.vRyHuR ;;
  probe:matched) source=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/.mako-reader-layout-matched-probe.jkuNVg ;;
  build:matched) source=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/.mako-reader-layout-matched-crates.lFTsaC ;;
  *) exit 2 ;;
esac
unset CARGO_BUILD_TARGET CARGO_ENCODED_RUSTFLAGS MIRIFLAGS
export RUSTUP_TOOLCHAIN=1.95.0
export CARGO_TARGET_DIR=/var/tmp/mako-reader-layout.XBkX12/target-$kind-$variant-$arm
export CARGO_BUILD_JOBS=2
export CARGO_PROFILE_RELEASE_LTO=false
export MAKO_BUILD_DIR=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22
export MAKO_LOCAL_REQUIRE_NATIVE=1 MAKO_LOCAL_FAKE_ABI=0
export MAKO_MTREE_NATIVE_INTEGRATION=1
export MAKO_MTREE_NATIVE_LIB_DIRS=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22:/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/src/masstree:/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export MAKO_MTREE_NATIVE_LIBS=static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread
export LIBCXX_DIR=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export LIBCLANG_PATH=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export PYTHON=/usr/bin/python3
export ROCKSDB_LIB_DIR=/var/tmp/mako-engine-perf-20260912.trsFuD/rocks
export LD_LIBRARY_PATH=/var/tmp/mako-engine-perf-20260912.trsFuD/rocks:/home/users/shuai/.linuxbrew/opt/llvm@22/lib:/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/third-party/yaml-cpp
export RUSTFLAGS='-C target-cpu=znver1 -C force-frame-pointers=yes -C debuginfo=1 -C linker=/var/tmp/mako-engine-perf-20260912.trsFuD/linker.sh'
rustc --version --verbose
cd "$source"
if [[ "$kind" == probe ]]; then
  cargo test --locked --release -p mako-cache --no-default-features --features engine-$arm --lib private_layout_probe::fence_and_cache_offsets -- --exact --nocapture --test-threads=1
else
  cargo build --locked --release -p mako-cache-bench --no-default-features --features engine-$arm --bin mako-cache-bench
  cp "$CARGO_TARGET_DIR/release/mako-cache-bench" /var/tmp/mako-reader-layout.XBkX12/main-matched-$arm
  sha256sum /var/tmp/mako-reader-layout.XBkX12/main-matched-$arm
fi
