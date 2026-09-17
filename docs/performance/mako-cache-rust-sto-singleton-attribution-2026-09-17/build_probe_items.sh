#!/usr/bin/env bash
# Build the --items diagnostic harness (probe crate) for both engine arms.
set -euo pipefail
cd /var/tmp/probe-mine/probe-items
unset CARGO_BUILD_TARGET CARGO_ENCODED_RUSTFLAGS
TC=/home/users/shuai/.rustup/toolchains/1.95.0-x86_64-unknown-linux-gnu/bin
export CARGO_HOME=/home/users/shuai/.cargo RUSTUP_HOME=/home/users/shuai/.rustup
export RUSTC="$TC/rustc" PATH="$TC:$PATH"
export MAKO_BUILD_DIR=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22
export MAKO_LOCAL_REQUIRE_NATIVE=1 MAKO_LOCAL_FAKE_ABI=0
export LIBCXX_DIR=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export LIBCLANG_PATH=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export PATH=/home/users/shuai/.linuxbrew/opt/llvm@22/bin:$PATH
export BINDGEN_EXTRA_CLANG_ARGS=-I/home/users/shuai/.linuxbrew/opt/llvm@22/lib/clang/22/include
export PYTHON=/usr/bin/python3
export RUSTFLAGS='-C target-cpu=znver1 -C force-frame-pointers=yes -C debuginfo=1'
mkdir -p /var/tmp/probe-mine/bin
for arm in c-abi sto; do
  export CARGO_TARGET_DIR="/var/tmp/probe-mine/target-items-$arm"
  CARGO_PROFILE_RELEASE_LTO=thin CARGO_BUILD_JOBS=8 \
    "$TC/cargo" build --offline --release --no-default-features --features "engine-$arm"
  cp "$CARGO_TARGET_DIR/release/engine_compare" "/var/tmp/probe-mine/bin/probe-items-$arm"
done
ls -la /var/tmp/probe-mine/bin/
