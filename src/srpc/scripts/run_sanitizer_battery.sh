#!/usr/bin/env bash
# Tier 1.4 of docs/testing-plan.md: run the runtime battery under a sanitizer.
#
# The -DSRPC_SANITIZER= configs have always existed but nothing exercised them,
# so leak/race/use-after-free coverage was latent. This wires them into a
# runnable pass. It is a SEPARATE build tree (sanitizers are a whole-config
# switch, per CLAUDE.md) so it never disturbs the normal build/.
#
# It builds the runtime-battery target and its normal source-check dependencies.
# It excludes the Goal-0 dual-compile target, whose exact strong-symbol census
# and ASan/TSan/UBSan instrumentation injects its own runtime symbols
# (__asan_*, interceptors), which perturbs that census -- so the ABI oracle is
# both meaningless and failing under a sanitizer. Runtime memory/race checking
# is what a sanitizer is for, and that lives in the battery binaries.
#
# Usage:
#   scripts/run_sanitizer_battery.sh [address|thread|undefined]   (default: address)
#
# What it catches per mode:
#   address    memory leaks (LSan) + heap/stack/use-after-free
#   thread     data races across the reactor's poll threads and fibers
#   undefined  UB in the framer/varint/pointer code
#
# Optional-but-recommended pre-commit pass for changes touching the reactor,
# fibers, channels, or the C seam. NOT wired into ctest (a sanitizer build is
# minutes and a whole extra tree); run it deliberately.
set -euo pipefail

MODE="${1:-address}"
case "$MODE" in
  address|thread|undefined) ;;
  *) echo "usage: $0 [address|thread|undefined]" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-san-$MODE"

echo "=== configuring $BUILD (SRPC_SANITIZER=$MODE) ==="
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSRPC_SANITIZER="$MODE"

echo "=== building ONLY the battery under $MODE (skipping the ABI gate) ==="
cmake --build "$BUILD" --parallel 4 --target srpc_runtime_battery

echo "=== running the runtime battery under $MODE ==="
# LSan on by default under address; make a leak fail the run.  Suppress the
# reactor's by-design fiber retention (see scripts/lsan_suppressions.txt) so
# the leak dimension is usable instead of drowned in known retention; the
# AddressSanitizer memory-error checks are unaffected and are the main value.
export ASAN_OPTIONS="detect_leaks=1:${ASAN_OPTIONS:-}"
export LSAN_OPTIONS="suppressions=$ROOT/scripts/lsan_suppressions.txt:${LSAN_OPTIONS:-}"
ctest --test-dir "$BUILD" -L runtime_battery --output-on-failure

echo "=== $MODE sanitizer battery passed ==="
