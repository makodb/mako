#!/bin/bash
set -euo pipefail
export RUSTUP_TOOLCHAIN=1.95.0
export MAKO_BUILD_DIR=/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22
export MAKO_LOCAL_FAKE_ABI=0
export MAKO_LOCAL_REQUIRE_NATIVE=1
export MAKO_MTREE_NATIVE_INTEGRATION=1
export MAKO_MTREE_NATIVE_LIB_DIRS="${MAKO_BUILD_DIR}:${MAKO_BUILD_DIR}/src/masstree:/home/users/shuai/.linuxbrew/opt/llvm@22/lib"
export MAKO_MTREE_NATIVE_LIBS=static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread
export LD_LIBRARY_PATH="/home/users/shuai/.linuxbrew/opt/llvm@22/lib:${MAKO_BUILD_DIR}/third-party/yaml-cpp"
export MAKO_LOCAL_REQUIRE_DIFFERENTIAL=1
export MAKO_LOCAL_DIFFERENTIAL_DRIVER="${MAKO_BUILD_DIR}/mako_local_differential_driver"
export PYTHON=/home/users/shuai/.linuxbrew/bin/python3
export LIBCLANG_PATH=/home/users/shuai/.linuxbrew/opt/llvm@21/lib
export CARGO_TARGET_DIR=/var/tmp/mako-sto-integration.UdvOQE/target
export TMPDIR=/var/tmp/mako-sto-integration.UdvOQE
export CARGO_BUILD_JOBS=4
if [[ "${1:-}" == "--run" ]]; then
    shift
    exec "$@"
fi
exec cargo "$@"
