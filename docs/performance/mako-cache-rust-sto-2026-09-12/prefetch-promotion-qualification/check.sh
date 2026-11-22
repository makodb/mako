#!/usr/bin/env bash
set -euo pipefail
unset CARGO_BUILD_TARGET CARGO_ENCODED_RUSTFLAGS MIRIFLAGS
export CARGO_BUILD_JOBS=4
export TMPDIR=/var/tmp/mako-prefetch-promotion.3eMW85/tmp
manifest=/var/tmp/mako-prefetch-promotion.3eMW85/crates/Cargo.toml
case "${1:?expected miri or native}" in
  miri)
    unset RUSTFLAGS MAKO_MTREE_NATIVE_INTEGRATION MAKO_MTREE_NATIVE_LIB_DIRS MAKO_MTREE_NATIVE_LIBS
    export RUSTUP_TOOLCHAIN=nightly-2026-08-12
    export CARGO_TARGET_DIR=/var/tmp/mako-prefetch-promotion.3eMW85/target-miri
    cargo +nightly-2026-08-12 miri test --locked --manifest-path "$manifest" -p mako-engine-sto --lib -- --test-threads=1
    ;;
  native)
    export RUSTUP_TOOLCHAIN=1.95.0
    export CARGO_TARGET_DIR=/var/tmp/mako-prefetch-promotion.3eMW85/target-native
    export MAKO_MTREE_NATIVE_INTEGRATION=1
    export MAKO_MTREE_NATIVE_LIB_DIRS=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22:/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/src/masstree:/home/users/shuai/.linuxbrew/opt/llvm@22/lib
    export MAKO_MTREE_NATIVE_LIBS=static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread
    export LD_LIBRARY_PATH=/home/users/shuai/.linuxbrew/opt/llvm@22/lib:/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/third-party/yaml-cpp
    export RUSTFLAGS='-C target-cpu=znver1 -C force-frame-pointers=yes -C debuginfo=1 -C linker=/var/tmp/mako-engine-perf-20260912.trsFuD/linker.sh'
    rustc --version --verbose
    cargo test --locked --manifest-path "$manifest" -p mako-engine-sto --all-features --lib prefetch::tests::hint_does_not_allocate_a_stamp_or_change_the_gate -- --exact --test-threads=1
    ;;
  *) exit 2 ;;
esac
