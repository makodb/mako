#!/usr/bin/env bash
# Build the worktree's patched engine_compare (bin engine_compare_items) for both arms.
set -uo pipefail
TC=/home/users/shuai/.rustup/toolchains/1.95.0-x86_64-unknown-linux-gnu/bin
WT=/home/users/shuai/mako/.claude/worktrees/masstree-rocks
export CARGO_HOME=/home/users/shuai/.cargo RUSTUP_HOME=/home/users/shuai/.rustup
export RUSTUP_TOOLCHAIN=1.95.0 CARGO_NET_OFFLINE=true
export PATH="$TC:/home/users/shuai/.cargo/bin:/home/users/shuai/.linuxbrew/opt/llvm@22/bin:$PATH"
export RUSTC="$TC/rustc"
export LIBCXX_DIR=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export LIBCLANG_PATH=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export BINDGEN_EXTRA_CLANG_ARGS=-I/home/users/shuai/.linuxbrew/opt/llvm@22/lib/clang/22/include
export PYTHON=/usr/bin/python3
export MAKO_BUILD_DIR="$WT/build_c22"
export MAKO_LOCAL_REQUIRE_NATIVE=1 MAKO_LOCAL_FAKE_ABI=0
export MAKO_MTREE_NATIVE_INTEGRATION=1
export MAKO_MTREE_NATIVE_LIB_DIRS="$MAKO_BUILD_DIR:$MAKO_BUILD_DIR/src/masstree:/home/users/shuai/.linuxbrew/opt/llvm@22/lib"
export MAKO_MTREE_NATIVE_LIBS=static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread
export RUSTFLAGS='-C target-cpu=znver1 -C force-frame-pointers=yes -C debuginfo=1'
cd /var/tmp/probe-mine/probe-items
mkdir -p /var/tmp/probe-mine/bin
for arm in sto c-abi; do
  export CARGO_TARGET_DIR="/var/tmp/probe-mine/target-items-$arm"
  CARGO_PROFILE_RELEASE_LTO=thin CARGO_BUILD_JOBS=8 \
    "$TC/cargo" build --offline --release --no-default-features --features "engine-$arm" \
    --bin engine_compare_items || { echo "ARM $arm FAILED"; continue; }
  cp "$CARGO_TARGET_DIR/release/engine_compare_items" "/var/tmp/probe-mine/bin/engine-compare-items-$arm"
  echo "built engine-compare-items-$arm"
done
chown -R shuai:shuai /var/tmp/probe-mine 2>/dev/null
ls -la /var/tmp/probe-mine/bin/
