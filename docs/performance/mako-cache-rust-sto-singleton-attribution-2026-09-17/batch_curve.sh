#!/usr/bin/env bash
# Build the --rcu-batch diagnostic (sto arm) and run the K curve.
set -uo pipefail
TC=/home/users/shuai/.rustup/toolchains/1.95.0-x86_64-unknown-linux-gnu/bin
WT=/home/users/shuai/mako/.claude/worktrees/masstree-rocks
export CARGO_HOME=/home/users/shuai/.cargo RUSTUP_HOME=/home/users/shuai/.rustup
export RUSTUP_TOOLCHAIN=1.95.0 CARGO_NET_OFFLINE=true
export PATH="$TC:/home/users/shuai/.cargo/bin:$PATH"
export RUSTC="$TC/rustc"
export LIBCXX_DIR=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export LIBCLANG_PATH=/home/users/shuai/.linuxbrew/opt/llvm@22/lib
export MAKO_BUILD_DIR="$WT/build_c22"
export MAKO_LOCAL_REQUIRE_NATIVE=1 MAKO_LOCAL_FAKE_ABI=0
export MAKO_MTREE_NATIVE_INTEGRATION=1
export MAKO_MTREE_NATIVE_LIB_DIRS="$MAKO_BUILD_DIR:$MAKO_BUILD_DIR/src/masstree:/home/users/shuai/.linuxbrew/opt/llvm@22/lib"
export MAKO_MTREE_NATIVE_LIBS=static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread
export RUSTFLAGS='-C target-cpu=znver1 -C force-frame-pointers=yes -C debuginfo=1'

echo "=== build batch-sto ==="
cd /var/tmp/probe-mine/batch-source/crates
export CARGO_TARGET_DIR=/var/tmp/probe-mine/target-batch
CARGO_PROFILE_RELEASE_LTO=thin CARGO_BUILD_JOBS=8 \
  "$TC/cargo" build --offline --release -p mako-cache-bench --no-default-features \
  --features engine-sto --bin engine_compare || { echo BUILD_FAILED; exit 1; }
mkdir -p /var/tmp/probe-mine/bin
cp "$CARGO_TARGET_DIR/release/engine_compare" /var/tmp/probe-mine/bin/batch-sto
echo built

echo "=== K curve ==="
W=/var/tmp/mako-engine-perf-20260912.iz63eX
export LD_LIBRARY_PATH="$W:/home/users/shuai/.linuxbrew/opt/llvm@22/lib"
export LD_PRELOAD="$W/libjemalloc.so.2"
out=/var/tmp/mako-batch-20260918; mkdir -p "$out"
run() { # tag bin extra-args
  local tag=$1 bin=$2; shift 2
  taskset -c 33-63 "$bin" engine --workers 1 --iterations 262144 --warmup 65536 --perf 1 "$@" \
    2>"$out/$tag.err" | python3 -c "
import json,sys
try:
    r=json.loads(sys.stdin.read())
except Exception as e:
    print('$tag parse-failed'); raise SystemExit
print(f\"{'$tag':22s} tx/s {r['transactions_per_second']:>11,.0f}  cyc/tx {r['cycles_per_commit']:>8.1f}  ins/tx {r['instructions_per_commit']:>7.0f}\")"
}
for r in 1 2 3; do
  run "frozen-baseline-r$r" /var/tmp/mako-w1-fixes.ShjYqz/stage3-thin
  for K in 0 1 8 16 64; do
    run "batch-K$K-r$r" /var/tmp/probe-mine/bin/batch-sto --rcu-batch "$K"
  done
done
grep -h "private diagnostic" "$out"/*.err | sort -u
chown -R shuai:shuai /var/tmp/probe-mine "$out" 2>/dev/null
echo done
