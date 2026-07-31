#!/bin/bash
# Re-verify the 6/6 whole-crate transpile+compile at the current
# verify-stack pin. Phase 1 (transpile) is BMI-independent; phase 2
# compiles the six emitted modules against the mako build tree's
# freshly rebuilt rusty/std BMIs (flag parity with build_clang22).
set -e
WT=/home/users/shuai/mako/.claude/worktrees/srpc-crate
OUT=/var/tmp/mako-srpc/segv/spike_reverify/out
BUILD=/var/tmp/mako-srpc/tree/build_clang22
TRANSPILER=/var/tmp/rusty-cpp-fix/target/release/rusty-cpp-transpiler
CLANG=/var/tmp/mako-srpc/llvm/bin/clang++
RUSTY_INC=/var/tmp/mako-srpc/tree/third-party/rusty-cpp/include

phase="${1:-all}"

if [ "$phase" = "transpile" ] || [ "$phase" = "all" ]; then
  rm -rf "$OUT"; mkdir -p "$OUT"
  "$TRANSPILER" --crate "$WT"/crates/srpc/Cargo.toml \
    --output-dir "$OUT" --auto-namespace
  echo "== transpile OK: $(ls "$OUT"/*.cppm | wc -l) modules =="
fi

if [ "$phase" = "compile" ] || [ "$phase" = "all" ]; then
  cd "$OUT"
  MODMAP="$OUT"/rusty.modmap
  : > "$MODMAP"
  echo "-fmodule-file=std=$BUILD/CMakeFiles/__cmake_cxx_std_23.dir/std.pcm" >> "$MODMAP"
  for p in "$BUILD"/third-party/rusty-cpp/CMakeFiles/*.dir/*.pcm; do
    name=$(basename "$p" .pcm)
    echo "-fmodule-file=${name//-/:}=$p" >> "$MODMAP"
  done
  FLAGS=(-std=gnu++23 -stdlib=libc++ -O3 -DNDEBUG -march=native
         -I"$RUSTY_INC" "@$MODMAP" -Wno-reserved-module-identifier)
  declare -A PCM
  compile() {  # compile <module-name> [dep-module ...]
    local m=$1; shift
    local deps=()
    for d in "$@"; do deps+=("-fmodule-file=$d=${PCM[$d]}"); done
    # CHECK THE EXIT STATUS. This printed "OK" unconditionally for
    # months of use, so counting OK lines measured nothing — a module
    # that failed to compile reported exactly like one that succeeded,
    # and "21 of 24 compile" was derived from those counts.
    if ! "$CLANG" "${FLAGS[@]}" "${deps[@]}" --precompile -x c++-module \
         "$m.cppm" -o "$m.pcm"; then
      echo "  FAIL $m"
      return 1
    fi
    PCM[$m]="$OUT/$m.pcm"
    echo "  OK $m"
  }
  compile srpc.sys
  compile srpc.runtime.epoll  srpc.sys
  compile srpc.runtime.poll_thread srpc.runtime.epoll srpc.sys
  compile srpc.runtime        srpc.runtime.epoll srpc.runtime.poll_thread srpc.sys
  compile srpc.base.time
  compile srpc.base.sync
  compile srpc.base.log     srpc.base.time
  compile srpc.base.rand    srpc.base.time
  compile srpc.base         srpc.base.time srpc.base.sync srpc.base.log srpc.base.rand
  compile srpc.rpc.errors
  compile srpc.rpc.circuit_breaker srpc.base.time srpc.base.rand
  compile srpc.rpc.connection_state
  compile srpc.rpc.request_options srpc.base.rand srpc.base.time
  compile srpc.rpc.heartbeat srpc.base.time
  compile srpc.rpc.load_balancer srpc.base.rand srpc.base.time
  compile srpc.rpc.connection_metrics srpc.rpc.load_balancer srpc.rpc.circuit_breaker srpc.base.rand srpc.base.time
  compile srpc.rpc.reconnect srpc.base.rand srpc.base.time
  compile srpc.rpc          srpc.rpc.errors srpc.rpc.reconnect srpc.rpc.circuit_breaker srpc.rpc.connection_state srpc.rpc.request_options srpc.rpc.heartbeat srpc.rpc.load_balancer srpc.rpc.connection_metrics srpc.base.rand srpc.base.time
  compile srpc.wire.varint
  compile srpc.wire.archive
  compile srpc.wire.frame   srpc.wire.varint srpc.wire.archive
  compile srpc.wire.serde   srpc.wire.varint srpc.wire.archive
  compile srpc.wire         srpc.wire.varint srpc.wire.archive srpc.wire.frame srpc.wire.serde
  # Added 2026-07-31: everything from S3 onward was emitted but NEVER
  # compiled — the gate had drifted to cover only the wire layer and the
  # poll thread while the transport, client, server, executor and fibers
  # accumulated unchecked.
  compile srpc.runtime.fiber srpc.sys
  compile srpc.runtime.fiber.runtime srpc.runtime.fiber srpc.sys
  compile srpc.runtime.tcp srpc.sys srpc.runtime.epoll srpc.runtime.poll_thread srpc.wire.frame srpc.rpc.errors srpc.wire
  compile srpc.rpc.client srpc.runtime.tcp srpc.runtime.poll_thread srpc.runtime.epoll srpc.wire.frame srpc.wire.varint srpc.wire.serde srpc.wire srpc.base.sync srpc.base srpc.rpc.errors srpc.sys
  compile srpc.rpc.server srpc.rpc.client srpc.runtime.tcp srpc.runtime.poll_thread srpc.runtime.epoll srpc.wire.frame srpc.wire.varint srpc.wire srpc.rpc.errors srpc.sys srpc.runtime.fiber srpc.runtime.fiber.runtime srpc.base.sync srpc.base
  compile srpc.rpc.task srpc.rpc.client srpc.runtime.tcp srpc.runtime.poll_thread srpc.wire srpc.wire.frame srpc.rpc.errors srpc.base.sync srpc.base srpc.sys srpc.runtime.epoll
  compile srpc              srpc.wire srpc.wire.varint srpc.wire.archive srpc.wire.frame srpc.wire.serde srpc.base srpc.base.time srpc.base.sync srpc.base.log srpc.base.rand srpc.rpc srpc.rpc.errors srpc.rpc.reconnect srpc.rpc.circuit_breaker srpc.rpc.connection_state srpc.rpc.request_options srpc.rpc.heartbeat srpc.rpc.load_balancer srpc.rpc.connection_metrics srpc.sys srpc.runtime srpc.runtime.epoll srpc.runtime.poll_thread
  echo "== 25/25 COMPILE OK at pin $(git -C "$WT"/third-party/rusty-cpp rev-parse --short HEAD) =="

  # BMIs carry declarations only — emit the objects too, so a consumer
  # can LINK the translated modules (the runtime golden proof).
  for m in srpc.sys srpc.runtime.epoll srpc.runtime.poll_thread srpc.runtime srpc.base.time srpc.base.sync srpc.base.log srpc.base.rand srpc.base \
           srpc.rpc.errors srpc.rpc.reconnect srpc.rpc.circuit_breaker srpc.rpc.connection_state srpc.rpc.request_options srpc.rpc.heartbeat srpc.rpc.load_balancer srpc.rpc.connection_metrics srpc.rpc \
           srpc.wire.varint srpc.wire.archive srpc.wire.frame \
           srpc.wire.serde srpc.wire srpc; do
    deps=()
    for d in srpc.sys srpc.runtime.epoll srpc.runtime.poll_thread srpc.runtime srpc.base.time srpc.base.sync srpc.base.log srpc.base.rand srpc.base \
             srpc.rpc.errors srpc.rpc.reconnect srpc.rpc.circuit_breaker srpc.rpc.connection_state srpc.rpc.request_options srpc.rpc.heartbeat srpc.rpc.load_balancer srpc.rpc.connection_metrics srpc.rpc \
             srpc.wire.varint srpc.wire.archive srpc.wire.frame \
             srpc.wire.serde srpc.wire; do
      [ "$d" = "$m" ] && continue
      deps+=("-fmodule-file=$d=$OUT/$d.pcm")
    done
    "$CLANG" "${FLAGS[@]}" "${deps[@]}" -c "$m.pcm" -o "$m.o"
  done
  echo "== objects emitted: $(ls "$OUT"/*.o | wc -l) =="
fi

# Runtime proof: the C++ golden test importing the TRANSLATED modules
# must reproduce the same corpus bytes as the Rust crate and the
# production C++ encoders.
if [ "$phase" = "golden" ] || [ "$phase" = "all" ]; then
  cd "$OUT"
  MODMAP="$OUT"/rusty.modmap
  "$CLANG" -std=gnu++23 -stdlib=libc++ -O3 -DNDEBUG -march=native \
    -DRUSTY_PORTABLE_INTRINSICS=1 -I"$RUSTY_INC" "@$MODMAP" \
    -Wno-reserved-module-identifier \
    -fmodule-file=srpc.wire.varint="$OUT"/srpc.wire.varint.pcm \
    -fmodule-file=srpc.wire.archive="$OUT"/srpc.wire.archive.pcm \
    -fmodule-file=srpc.wire.frame="$OUT"/srpc.wire.frame.pcm \
    -fmodule-file=srpc.wire.serde="$OUT"/srpc.wire.serde.pcm \
    -fmodule-file=srpc.wire="$OUT"/srpc.wire.pcm \
    -fmodule-file=srpc="$OUT"/srpc.pcm \
    "$WT"/src/rrr/tests/wire_golden_translated_test.cc "$OUT"/*.o \
    "$BUILD"/third-party/rusty-cpp/lib*.a \
    "$BUILD"/third-party/rusty-cpp/lib*.a \
    -o "$OUT"/golden_translated
  "$OUT"/golden_translated "$WT"/crates/srpc/tests/golden/wire_corpus.txt
fi
