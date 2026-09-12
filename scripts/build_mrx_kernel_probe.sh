#!/usr/bin/env bash
# Build the focused MRX syscall/context-switch probe through its canonical
# CMake target. This deliberately contains no compiler paths or copied flags:
# the selected build tree owns the toolchain, generated modules, STO profile,
# and native/Rust dependency closure.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_root=$(cd -- "${script_dir}/.." && pwd)
build_dir=${1:-${MAKO_BUILD_DIR:-"${source_root}/build"}}

if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
    printf 'error: %s is not a configured Mako CMake build directory\n' \
        "${build_dir}" >&2
    printf 'configure one first, or pass its path as the first argument\n' >&2
    exit 2
fi

# CMake reports a clear target-not-found error when Cargo/mrxffi was absent at
# configure time, which is preferable to silently producing a reduced probe.
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')}
cmake --build "${build_dir}" --target mrx_kernel_probe --parallel "${jobs}"

probe="${build_dir}/mrx_kernel_probe"
if [[ ! -x "${probe}" ]]; then
    printf 'error: CMake completed but %s is not executable\n' "${probe}" >&2
    exit 1
fi

printf 'built %s\n' "${probe}"
