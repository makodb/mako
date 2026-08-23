#!/bin/bash
# Compiles and links mrx_kernel_probe with the EXACT flags CMake uses for
# masstree_rocks_bench (taken from compile_commands.json and build.ninja),
# but outside the build system: a sibling session is building in
# build_c22 concurrently and editing CMakeLists.txt would race with it.
set -eu
W=/home/users/shuai/mako/.claude/worktrees/masstree-rocks
B=$W/build_c22
CXX=/home/users/shuai/.linuxbrew/opt/llvm@22/bin/clang++
SRC=$W/src/mako/benchmarks/mrx_kernel_probe.cc
OBJ=$B/mrx_kernel_probe.o
OUT=$B/mrx_kernel_probe

cd "$B"

# Keep template-affecting STO definitions identical to the library in this
# cached build. A fresh configure emits numeric 0/1 values; an older cache may
# still contain ON/OFF, which must not be mixed with differently compiled
# MassTrans templates at link time.
STO_DEFINES=$(awk '
  /^  FLAGS = / && /-DREAD_MY_WRITES=/ {
    for (i = 1; i <= NF; ++i)
      if ($i ~ /^-D(READ_MY_WRITES|HASHTABLE|STO_OPACITY)=/)
        printf "%s ", $i
    exit
  }
' build.ninja)
test -n "$STO_DEFINES"

$CXX \
 -DCONFIG_H=\"$W/src/mako/config/config-perf.h\" \
 -DMASSTREE_CONFIG_H=\"$B/generated/masstree/config.h\" \
 -DMASSTREE_SUPPORT_HEADER=\"$W/src/mako/masstree_btree.h\" \
 -DRAFT_DEFAULT_SINGLE_GROUP=1 -DRPC_TEST_HOOKS -DRUSTY_PORTABLE_INTRINSICS=1 \
 -D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION \
 -D_LIBCPP_ENABLE_CXX20_REMOVED_BINDER_TYPEDEFS \
 -D_LIBCPP_ENABLE_CXX20_REMOVED_TYPE_TRAITS -D_LIBCPP_NO_ABI_TAG \
 -I$W/src -I$W/src/rrr -I$W/third-party/rusty-cpp/include -I$W/src/mako \
 -I$W/src/masstree -I$W/third-party/erpc/src -I$W/src/deptran -I$W \
 -I$W/third-party/lz4 -I$W/third-party/yaml-cpp/include -I$W/src/memdb \
 -I$W/src/bench -I$W/src/mako/benchmarks -I$W/src/compat -I$W/src/masstree/.. \
 -I$B/generated/masstree -isystem $W/crates/mrx-ffi/include \
 -isystem /home/users/shuai/.linuxbrew/opt/python@3.14/include/python3.14 \
 -stdlib=libc++ -O3 -DNDEBUG -std=gnu++23 -w -Wreturn-type \
 -I$B/generated/masstree -I$B/generated -Isrc/mako \
 -I$W/third-party/lz4 -Isrc -I$W \
 -DRUSTYCPP_DISABLE_ARC_LOG -DREUSE_FIBER -DERPC_FAKE=true \
 -DERPC_LOG_LEVEL=6 -DERPC_TESTING=false -DGFLAGS_IS_A_DLL=0 \
 -march=native -O2 -g -DFAIL_NEW_VERSION $STO_DEFINES \
 -I/usr/include -DUSE_JEMALLOC \
 -fno-omit-frame-pointer \
 -include $B/generated/masstree/config.h \
 @CMakeFiles/masstree_rocks_bench.dir/src/mako/benchmarks/masstree_rocks_bench.cc.o.modmap \
 -o "$OBJ" -c "$SRC"

LINK_FLAGS="-stdlib=libc++ -L/home/users/shuai/.linuxbrew/opt/llvm@22/lib -Wl,-rpath,/home/users/shuai/.linuxbrew/opt/llvm@22/lib -stdlib=libc++ -lc++abi -Xlinker -u -Xlinker janus_register_bench_factories"
LINK_LIBRARIES=$(python3 - <<'PY'
import re
for line in open("build.ninja"):
    pass
PY
)
# The library list is long and order-sensitive; take it verbatim from the
# generated build.ninja rather than retyping it.
LIBS=$(awk '/^build masstree_rocks_bench: CXX_EXECUTABLE_LINKER/{f=1} f&&/^  LINK_LIBRARIES = /{sub(/^  LINK_LIBRARIES = /,""); print; exit}' build.ninja)

$CXX -stdlib=libc++ -O3 -DNDEBUG $LINK_FLAGS "$OBJ" -o "$OUT" $LIBS
echo "built $OUT"
ls -l --time-style=+%H:%M:%S "$OUT"
