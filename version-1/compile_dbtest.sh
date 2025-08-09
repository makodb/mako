#!/bin/bash

# This script compiles dbtest.cc by linking it against the pre-built libmako.a
# and other required dependencies.

# Ensure the script exits if any command fails
set -e

# --- Configuration ---
# Assumes the mako library and its dependencies have been built in the 'build' directory.
# Assumes the 'eth' environment and 'perf' mode were used for the library build.
# ---

# Get dependency flags from pkg-config
PKG_CONFIG_CFLAGS=$(pkg-config --cflags libevent gflags protobuf jemalloc)
PKG_CONFIG_LIBS=$(pkg-config --libs libevent gflags protobuf jemalloc)

# Project-specific include paths
INCLUDE_PATHS="-Isrc -Isrc/mako -Isrc/mako/masstree -Ithird-party/lz4 -Ithird-party/erpc/src -Ithird-party/erpc/third_party/asio/include"

# Project-specific compiler definitions
# These MUST match the settings used to compile libmako.a
# Use absolute path for CONFIG_H to avoid include issues.
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
DEFINES="-DCONFIG_H=\"${SCRIPT_DIR}/src/mako/config/config-perf.h\" -DUSE_JEMALLOC -DERPC_FAKE=true -DGFLAGS_IS_A_DLL=0 -DSHARDS=1"

# Paths to our pre-built libraries
LINK_PATHS="-Lbuild -Lbuild/third-party/erpc -Lthird-party/lz4 -Lrust-lib/target/release"

# The libraries to link
LIBS="\
  ${SCRIPT_DIR}/build/libmako.a \
  ${SCRIPT_DIR}/build/third-party/erpc/liberpc.a \
  ${SCRIPT_DIR}/rust-lib/target/release/librust_redis.a \
  /home/weihai/mako/build/libtxlog.so \
  /home/weihai/mako/third-party/lz4/liblz4.so \
  -lyaml-cpp -lboost_fiber -lboost_context -lboost_system -lboost_thread \
  -lpthread -lnuma -lrt -lz -lcrypt -laio -ldl -lssl -lcrypt -levent_pthreads"

# The final g++ command
g++ -std=c++17 -O2 \
  -include src/mako/masstree/config.h \
  $INCLUDE_PATHS \
  $PKG_CONFIG_CFLAGS \
  $DEFINES \
  src/mako/benchmarks/dbtest.cc \
  $LINK_PATHS \
  $LIBS \
  $PKG_CONFIG_LIBS \
  -o build/dbtest

echo "Successfully compiled 'dbtest' executable in 'build/dbtest'"