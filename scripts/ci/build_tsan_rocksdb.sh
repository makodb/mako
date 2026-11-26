#!/usr/bin/env bash
# Pinned upstream RocksDB with compiler instrumentation for the native TSan lane.
set -euo pipefail
out="${1:?usage: build_tsan_rocksdb.sh OUTPUT_DIRECTORY [JOBS]}"
jobs="${2:-4}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'jobs must be positive' >&2; exit 2; }
[[ ! -e "$out" ]] || { echo "expected a fresh output directory: $out" >&2; exit 2; }
mkdir -p "$out"
out="$(cd "$out" && pwd -P)"
archive="$out/rocksdb-11.8.1.tar.gz"
curl --proto '=https' --tlsv1.2 -fL --retry 3 \
    https://codeload.github.com/facebook/rocksdb/tar.gz/refs/tags/v11.8.1 -o "$archive"
printf '%s  %s\n' 618d9726a7cb1cf4ce034f4cdca49de98aa64867dda06b91371a791ae8921aff "$archive" \
    | sha256sum --check --strict
tar -xzf "$archive" -C "$out"
cmake -S "$out/rocksdb-11.8.1" -B "$out/build" -G Ninja \
    -DCMAKE_C_COMPILER="${CC:-clang-22}" -DCMAKE_CXX_COMPILER="${CXX:-clang++-22}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g -DNDEBUG -fno-omit-frame-pointer' \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DWITH_TSAN=ON -DROCKSDB_BUILD_SHARED=ON \
    -DPORTABLE=ON -DFAIL_ON_WARNINGS=OFF -DWITH_TESTS=OFF -DWITH_TOOLS=OFF \
    -DWITH_BENCHMARK_TOOLS=OFF -DWITH_CORE_TOOLS=OFF -DWITH_TRACE_TOOLS=OFF \
    -DWITH_GFLAGS=OFF -DWITH_JNI=OFF -DWITH_LIBURING=OFF -DWITH_JEMALLOC=OFF \
    -DWITH_SNAPPY=ON -DWITH_LZ4=ON -DWITH_ZLIB=ON -DWITH_ZSTD=ON -DWITH_BZ2=ON
cmake --build "$out/build" --target rocksdb-shared --parallel "$jobs"
python3 - "$out" <<'PY'
import json
from pathlib import Path
import shlex
import subprocess
import sys
out = Path(sys.argv[1])
commands = json.loads((out/'build/compile_commands.json').read_text())
selected = [c for c in commands if 'rocksdb-shared.dir/' in c['command']]
if not selected or any('-fsanitize=thread' not in shlex.split(c['command']) for c in selected):
    raise SystemExit('RocksDB shared objects lack TSan instrumentation')
library = (out/'build/librocksdb.so').resolve(strict=True)
symbols = subprocess.check_output(['nm', '-D', '--undefined-only', library], text=True)
if '__tsan_read' not in symbols or '__tsan_write' not in symbols:
    raise SystemExit('RocksDB lacks compiler TSan hooks')
(out/'instrumentation.json').write_text(json.dumps({
    'version': '11.8.1', 'instrumented_sources': len(selected), 'library': str(library),
    'compiler_commands': str(out/'build/compile_commands.json')}, indent=2)+'\n')
print(f'RocksDB TSan: {len(selected)} instrumented translation units')
PY
sha256sum "$out/build/librocksdb.so" >"$out/library.sha256"
