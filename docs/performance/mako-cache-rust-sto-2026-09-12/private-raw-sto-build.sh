#!/usr/bin/env bash
set -euo pipefail
unset CARGO_BUILD_TARGET CARGO_ENCODED_RUSTFLAGS
export RUSTUP_TOOLCHAIN=1.95.0
export CARGO_TARGET_DIR=/var/tmp/mako-raw-sto-attribution.XKZsYS/target-rust195
export CARGO_BUILD_JOBS=4 CARGO_PROFILE_RELEASE_LTO=false
export TMPDIR=/var/tmp/mako-raw-sto-attribution.XKZsYS/tmp
export MAKO_BUILD_DIR=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22
export MAKO_LOCAL_REQUIRE_NATIVE=1 MAKO_LOCAL_FAKE_ABI=0
export MAKO_MTREE_NATIVE_INTEGRATION=1
export MAKO_MTREE_NATIVE_LIB_DIRS=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22:/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/src/masstree:/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export MAKO_MTREE_NATIVE_LIBS=static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread
export LIBCXX_DIR=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export LIBCLANG_PATH=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export ROCKSDB_LIB_DIR=/var/tmp/mako-engine-perf-20260912.trsFuD/rocks
export LD_LIBRARY_PATH=/var/tmp/mako-engine-perf-20260912.trsFuD/rocks:/home/users/shuai/.linuxbrew/opt/llvm@22/lib:/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/third-party/yaml-cpp
export PYTHON=/usr/bin/python3
export RUSTFLAGS='-C target-cpu=znver1 -C force-frame-pointers=yes -C debuginfo=1 -C linker=/var/tmp/mako-engine-perf-20260912.trsFuD/linker.sh'
manifest=/var/tmp/mako-raw-sto-attribution.XKZsYS/crates/Cargo.toml
check_native() {
    local observed
    observed=$(sha256sum /home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/libmako.a)
    [[ "$observed" == '53e9a34a1c1e963aaa809adcb26ac5af70d27d072c351122f5911b8f7a273402  '* ]]
    observed=$(sha256sum /home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/src/masstree/libmasstree.a)
    [[ "$observed" == 'd0470b6005b62b741198b1143ba82807bdcd6c9e2cadb274a6dad5d11b5ed613  '* ]]
}
check_native
rustc --version --verbose
case "${1:-build}" in
    build) cargo build --locked --release --manifest-path "$manifest" -p mako-cache-bench --no-default-features --features engine-sto --bin engine_compare ;;
    test) cargo test --locked --manifest-path "$manifest" -p mako-cache-bench --no-default-features --features engine-sto --bin engine_compare -- --test-threads=1 ;;
    clippy) cargo clippy --no-deps --locked --manifest-path "$manifest" -p mako-cache-bench --no-default-features --features engine-sto --bin engine_compare -- -D warnings ;;
    *) exit 2 ;;
esac
check_native
