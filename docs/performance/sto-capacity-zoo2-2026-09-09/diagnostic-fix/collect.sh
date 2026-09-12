#!/usr/bin/env bash
set -euo pipefail

evidence_dir="$(cd "$(dirname "$0")" && pwd)"
repo_dir=/home/users/shuai/mako/.claude/worktrees/sto-rust
tiny_build=/dev/shm/sto-output-regression.2V57O6
clang=/home/users/shuai/.linuxbrew/opt/llvm@22/bin/clang++
export TMPDIR=/dev/shm
export PATH=/home/users/shuai/.cargo/bin:/usr/bin:/home/users/shuai/.linuxbrew/opt/llvm@22/bin:/bin
export CARGO_TARGET_DIR=/dev/shm/sto-capacity-asan-20260909-host-cargo
unset ASAN_OPTIONS UBSAN_OPTIONS TSAN_OPTIONS LSAN_OPTIONS
cd "${repo_dir}"

run_logged() {
    local label="$1"
    shift
    {
        printf 'command:'
        printf ' %q' "$@"
        printf '\n'
        "$@"
        printf 'exit_status=0\n'
    } >"${evidence_dir}/${label}.log" 2>&1
}

git status --porcelain=v1 >"${evidence_dir}/git-status-before.txt"
python3 - <<'PY' >"${evidence_dir}/source-provenance.json"
import hashlib
import json
import pathlib
import subprocess
import time

revision = 'a3ad9a110727f5ecc937cbeee23fe40152df719f'
paths = [
    'src/mako/benchmarks/benchmark_output.h',
    'src/mako/benchmarks/bench.h',
    'tests/test_benchmark_output.cc',
    'CMakeLists.txt',
    '.github/workflows/ci.yml',
    'scripts/ci/run_rust_sto_sanitizer.sh',
]
source_hashes = {}
for path in paths:
    committed = subprocess.check_output(['git', 'show', f'{revision}:{path}'])
    assert pathlib.Path(path).read_bytes() == committed, path
    source_hashes[path] = hashlib.sha256(committed).hexdigest()
print(json.dumps({
    'source_revision': revision,
    'observed_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
    'captured_at_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
    'source_hashes': source_hashes,
    'source_hash_method': 'git show revision:path, compared byte-for-byte with compiled working files',
}, indent=2))
PY
run_logged toolchain "${clang}" --version
run_logged cmake-version /usr/bin/cmake --version
run_logged direct-native-build "${clang}" -std=c++23 -Wall -Wextra -Wpedantic \
    -Werror -pthread -Isrc/mako -g -O1 -fno-omit-frame-pointer \
    tests/test_benchmark_output.cc -o "${tiny_build}/test_benchmark_output-native"
run_logged direct-native "${tiny_build}/test_benchmark_output-native"
for sanitizer in address undefined thread; do
    run_logged "direct-${sanitizer}-build" "${clang}" -std=c++23 \
        -Wall -Wextra -Wpedantic -Werror -pthread -Isrc/mako -g -O1 \
        -fno-omit-frame-pointer "-fsanitize=${sanitizer}" \
        tests/test_benchmark_output.cc \
        -o "${tiny_build}/test_benchmark_output-${sanitizer}"
    run_logged "direct-${sanitizer}" "${tiny_build}/test_benchmark_output-${sanitizer}"
done

for profile in release address; do
    asan=OFF
    if [[ "${profile}" == address ]]; then
        asan=ON
    fi
    run_logged "cmake-${profile}-configure" /usr/bin/cmake -S . \
        -B "${tiny_build}/cmake" -DCMAKE_BUILD_TYPE=Release \
        "-DMAKO_ASAN=${asan}" -DMAKO_UBSAN=OFF -DMAKO_TSAN=OFF
    run_logged "cmake-${profile}-build" /usr/bin/cmake --build \
        "${tiny_build}/cmake" --target test_benchmark_output --parallel 2
    run_logged "cmake-${profile}-ctest" /usr/bin/ctest --test-dir \
        "${tiny_build}/cmake" -R '^test_benchmark_output$' \
        --output-on-failure --no-tests=error -V
    /usr/bin/ninja -C "${tiny_build}/cmake" -t commands test_benchmark_output \
        >"${evidence_dir}/cmake-${profile}-commands.txt"
    /usr/bin/ctest --test-dir "${tiny_build}/cmake" --show-only=json-v1 \
        >"${evidence_dir}/cmake-${profile}-inventory.json"
    rg '^(CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER|MAKO_ASAN|MAKO_UBSAN|MAKO_TSAN):' \
        "${tiny_build}/cmake/CMakeCache.txt" \
        >"${evidence_dir}/cmake-${profile}-cache.txt"
done

git status --porcelain=v1 >"${evidence_dir}/git-status-after.txt"
cmp "${evidence_dir}/git-status-before.txt" "${evidence_dir}/git-status-after.txt"
run_logged verify python3 "${evidence_dir}/verify.py" "${repo_dir}"
printf 'Evidence passed: %s\n' "${evidence_dir}"
