#!/usr/bin/env bash
set -euo pipefail
candidate=${1:?candidate snapshot name}
lto=${2:-false}
arm=${3:-sto}
case "$candidate" in baseline|stage1|stage2|stage3|combined) ;; *) exit 2;; esac
case "$arm" in sto|c-abi) ;; *) exit 2;; esac
case "$lto" in false|thin|fat) ;; *) exit 2;; esac
scratch=/var/tmp/mako-w1-implementation.VtilJG
unset CARGO_BUILD_TARGET CARGO_ENCODED_RUSTFLAGS
export RUSTUP_TOOLCHAIN=1.95.0
export CARGO_TARGET_DIR="$scratch/target-$candidate-$lto${arm#sto}"
if [[ "$arm" == c-abi ]]; then
    export CARGO_TARGET_DIR="$scratch/target-$candidate-$lto-c-abi-fresh"
fi
export CARGO_BUILD_JOBS=2 CARGO_PROFILE_RELEASE_LTO="$lto"
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
check_native() {
    local observed
    observed=$(sha256sum "$MAKO_BUILD_DIR/libmako.a")
    [[ "$observed" == '53e9a34a1c1e963aaa809adcb26ac5af70d27d072c351122f5911b8f7a273402  '* ]]
    observed=$(sha256sum "$MAKO_BUILD_DIR/src/masstree/libmasstree.a")
    [[ "$observed" == 'd0470b6005b62b741198b1143ba82807bdcd6c9e2cadb274a6dad5d11b5ed613  '* ]]
}
check_native
rustc --version --verbose
manifest="$scratch/$candidate/crates/Cargo.toml"
if [[ "$arm" == c-abi ]]; then
    # The verifier checks the complete native source closure against the real
    # CMake tree. Its Rust dependency closure is unchanged by either candidate.
    manifest=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/crates/Cargo.toml
fi
cargo build --locked --release --manifest-path "$manifest" \
    -p mako-cache-bench --no-default-features --features "engine-$arm" \
    --bin engine_compare --bin mako-cache-bench
check_native
