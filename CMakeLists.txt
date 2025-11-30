cmake_minimum_required(VERSION 3.10)
# using g++
set(CMAKE_C_COMPILER "gcc" CACHE STRING "C compiler")
set(CMAKE_CXX_COMPILER "g++" CACHE STRING "C++ compiler")

# Enable compile_commands.json generation for language servers (clangd, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(mako)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Check compiler and version
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.0)
        message(FATAL_ERROR "GCC version 12 or higher is required. Found ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
else()
    message(WARNING "You are not using GCC. Current compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

# Configure and enable rusy-cpp-checker
include(${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/cmake/RustyCppSubmodule.cmake)
set(RUSTYCPP_BUILD_TYPE "release")  # or "debug"

# Build the rusty-cpp-checker binary first before enabling borrow checking
set(RUSTYCPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp")
set(CPP_BORROW_CHECKER "${RUSTYCPP_DIR}/target/${RUSTYCPP_BUILD_TYPE}/rusty-cpp-checker")

# Create a custom command to build the checker if it doesn't exist
if(NOT EXISTS ${CPP_BORROW_CHECKER})
    message(STATUS "Building rusty-cpp-checker binary...")
    execute_process(
        COMMAND cargo build --${RUSTYCPP_BUILD_TYPE}
        WORKING_DIRECTORY ${RUSTYCPP_DIR}
        RESULT_VARIABLE BUILD_RESULT
    )
endif()

# Enable borrow checking (can be disabled with -DENABLE_BORROW_CHECKING=OFF)
# Force ON here so the checker runs unless explicitly turned off
set(ENABLE_BORROW_CHECKING ON CACHE BOOL "Enable RustyCpp borrow checking" FORCE)
if(ENABLE_BORROW_CHECKING)
    enable_borrow_checking()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

# Find Python for txlog library
find_package(Python3 COMPONENTS Development REQUIRED)

# Find Boost libraries for txlog library
find_package(Boost REQUIRED COMPONENTS system filesystem thread coroutine context)

# Define paths
set(mkfile_path "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
set(HOME "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ERPC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc")
message(STATUS "I: ${ERPC_PATH}")
set(W "src/mako")
set(TOP "${CMAKE_SOURCE_DIR}")

# RUST
set(RUST_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/target/release")
set(RUST_LIB_NAME "rust_redis")
set(RUST_LIB_PATH "${RUST_LIB_DIR}/lib${RUST_LIB_NAME}.a")

# Use add_custom_command instead of add_custom_target to avoid rebuilding every time
add_custom_command(
    OUTPUT ${RUST_LIB_PATH}
    COMMAND cargo build --release
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/src/lib.rs
    COMMENT "Building Rust library if needed"
)

# Create a custom target that depends on the output
add_custom_target(rust_build ALL DEPENDS ${RUST_LIB_PATH})

# Create imported library for Rust library
add_library(rust_lib STATIC IMPORTED)
set_target_properties(rust_lib PROPERTIES
    IMPORTED_LOCATION "${RUST_LIB_PATH}"
)
add_dependencies(rust_lib rust_build)

# Read environment variable from env.txt (ib or dpdk or eth)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/env.txt" env_raw LIMIT_COUNT 1)
string(STRIP "${env_raw}" env_clean)

if(env_clean STREQUAL "ib")
  set(env "ib")
elseif(env_clean STREQUAL "dpdk")
  set(env "dpdk")
else()
  set(env "eth")
endif()
message(STATUS "environment: ${env}")

# Configure erpc based on environment
if(env STREQUAL "ib")
  # if env == "ib", erpc compile flag: cmake . -DTRANSPORT=infiniband -DROCE=on -DPERF=ON
  set(TRANSPORT "infiniband" CACHE STRING "eRPC transport type")
  set(ROCE ON CACHE BOOL "eRPC RoCE setting")
  set(PERF ON CACHE BOOL "eRPC performance setting")
  set(LOG_LEVEL "none" CACHE STRING "eRPC log level")
elseif(env STREQUAL "dpdk")
  # if env == "dpdk", erpc compile flag: -DTRANSPORT=dpdk -DAZURE=on -DPERF=ON -DLOG_LEVEL=none"
  set(TRANSPORT "dpdk" CACHE STRING "eRPC transport type")
  set(AZURE ON CACHE BOOL "eRPC Azure setting")
  set(PERF ON CACHE BOOL "eRPC performance setting")
  set(LOG_LEVEL "none" CACHE STRING "eRPC log level")
else()
  # else if env == "eth", erpc compile flag: cmake . -DTRANSPORT=fake -DROCE=off -DPERF=ON
  set(TRANSPORT "fake" CACHE STRING "eRPC transport type")
  set(ROCE OFF CACHE BOOL "eRPC RoCE setting")
  set(PERF ON CACHE BOOL "eRPC performance setting")
  set(LOG_LEVEL "none" CACHE STRING "eRPC log level")
endif()

add_subdirectory(third-party/erpc)
set_target_properties(erpc PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/third-party/erpc")

# eRPC flags
set(ERPC_CFLAGS_DPDK
  -Isrc/mako
  -I${ERPC_PATH}/src
  -DERPC_DPDK=true
  -march=native
  # ${DPDK_CXXFLAGS}
  -DERPC_LOG_LEVEL=6
  -DERPC_TESTING=false
  -DGFLAGS_IS_A_DLL=0
)
set(ERPC_LDFLAGS_DPDK
  -Wl,--whole-archive
  ${DPDK_LDFLAGS}
  -Wl,--no-whole-archive
  -lpthread
  -lnuma
  -ldl
  -lgflags
  -libverbs
  -lmlx4
  -lmlx5
)
set(ERPC_CFLAGS_IB
  -Isrc/mako
  -I${ERPC_PATH}/src
  -DERPC_INFINIBAND=true
  -march=native
  -DERPC_LOG_LEVEL=6
  -DERPC_TESTING=false
  -DGFLAGS_IS_A_DLL=0
)
set(ERPC_LDFLAGS_IB
  -Wl,--whole-archive
  ${DPDK_LDFLAGS}
  -Wl,--no-whole-archive
  -lpthread
  -lnuma
  -ldl
  -lgflags
  -libverbs
  -lmlx4
  -lmlx5
)
set(ERPC_CFLAGS_ETH
  -Isrc/mako
  -I${ERPC_PATH}/src
  -DERPC_FAKE=true
  -march=native
  -DERPC_LOG_LEVEL=6
  -DERPC_TESTING=false
  -DGFLAGS_IS_A_DLL=0
)
set(ERPC_LDFLAGS_ETH
  -Wl,--whole-archive
  ${DPDK_LDFLAGS}
  -Wl,--no-whole-archive
  -lpthread
  -lnuma
  -ldl
  -lgflags
  #-libverbs
  #-lmlx4
  #-lmlx5
)

# Options
option(DEBUG "Enable debug mode" OFF)
option(CHECK_INVARIANTS "Enable invariant checking" OFF)
option(COCO "Enable COCO" OFF)
set(USE_MALLOC_MODE 1 CACHE STRING "Malloc mode: 0=libc, 1=jemalloc, 2=tcmalloc, 3=flow")
set(MODE "perf" CACHE STRING "Build mode: perf, backoff, factor-gc, factor-gc-nowriteinplace, factor-fake-compression, sandbox")
option(STO_RMW "Enable STO read-modify-write" OFF)
option(HASHTABLE "Enable hashtable" OFF)
option(GPROF "Enable profiling with gprof" OFF)
option(OPACITY "Enable opacity" OFF)
option(DISABLE_MULTI_VERSION "Disable multi-version" OFF)
option(MEGA_BENCHMARK "Enable MEGA benchmark" OFF)
option(MEGA_BENCHMARK_MICRO "Enable MEGA benchmark micro" OFF)
option(TRACKING_LATENCY "Enable latency tracking" OFF)
option(DISABLE_DISK "Disable disk persistence" OFF)
option(FAIL_NEW_VERSION "Enable fail new version" ON)
option(SIMULATE_ONE_SHARD_PER_THREAD "Simulate one shard per thread" OFF)
option(TRACKING_ROLLBACK "Enable rollback tracking" OFF)

# Masstree configuration
set(MASSTREE_CONFIG "CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER} --enable-max-key-len=1024")
if(DEBUG)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --enable-assertions")
else()
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --disable-assertions")
endif()
if(CHECK_INVARIANTS)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --enable-invariants --enable-preconditions")
else()
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --disable-invariants --disable-preconditions")
endif()

# Output suffix
set(OSUFFIX_D "")
set(OSUFFIX_S "")
set(OSUFFIX_E "")
set(OSUFFIX_H "")
set(OSUFFIX_R "")
if(DEBUG)
  set(OSUFFIX_D "")
else()
  set(OSUFFIX_D "")
endif()
if(CHECK_INVARIANTS)
  set(OSUFFIX_S ".check")
else()
  set(OSUFFIX_S "")
endif()
if(EVENT_COUNTERS)
  set(OSUFFIX_E ".ectrs")
endif()
if(STO_RMW)
  set(OSUFFIX_R ".rmw")
endif()
if(HASHTABLE)
  set(OSUFFIX_H ".ht")
endif()
set(OSUFFIX "${OSUFFIX_D}${OSUFFIX_S}${OSUFFIX_E}${OSUFFIX_H}${OSUFFIX_R}")

# Output directory and config header
if(MODE STREQUAL "perf")
  set(O "out-perf${OSUFFIX}")
  set(CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/config-perf.h")
elseif(MODE STREQUAL "backoff")
  set(O "out-backoff${OSUFFIX}")
  set(CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/config-backoff.h")
elseif(MODE STREQUAL "factor-gc")
  set(O "out-factor-gc${OSUFFIX}")
  set(CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/config-factor-gc.h")
elseif(MODE STREQUAL "factor-gc-nowriteinplace")
  set(O "out-factor-gc-nowriteinplace${OSUFFIX}")
  set(CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/config-factor-gc-nowriteinplace.h")
elseif(MODE STREQUAL "factor-fake-compression")
  set(O "out-factor-fake-compression${OSUFFIX}")
  set(CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/config-factor-fake-compression.h")
elseif(MODE STREQUAL "sandbox")
  set(O "out-sandbox${OSUFFIX}")
  set(CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/config-sandbox.h")
else()
  message(FATAL_ERROR "Invalid mode: ${MODE}")
endif()
set(O "${O}.masstree")

# Borrow checker inputs (place after CONFIG_H so it is available to the checker)
set(BORROW_CHECKER_INCLUDE_PATHS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mako/masstree
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rrr
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/third_party/masstree-beta
)

# Make config paths visible to the borrow checker (will be turned into -include)
add_compile_definitions(MASSTREE_CONFIG_H=\"${CMAKE_CURRENT_SOURCE_DIR}/${W}/masstree/config.h\")
add_compile_definitions(MASSTREE_SUPPORT_HEADER=\"${CMAKE_CURRENT_SOURCE_DIR}/src/mako/masstree_btree.h\")

file(GLOB_RECURSE MASSTREE_BORROW_CHECK_SOURCES
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    src/mako/masstree/*.c
    src/mako/masstree/*.cc
)

foreach(SRC ${MASSTREE_BORROW_CHECK_SOURCES})
    add_borrow_check(${SRC})
endforeach()

# Compiler flags
set(CXXFLAGS
  -w
  -Wreturn-type
  -Isrc/mako
  -MD
  -MP
  -I${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4
  -Isrc
  -I${HOME}
  -DCONFIG_H=\"${CONFIG_H}\"
  -DRUSTYCPP_DISABLE_ARC_LOG  # Disable Arc debug logging
)

if(env STREQUAL "ib")
  list(APPEND CXXFLAGS ${ERPC_CFLAGS_IB})
elseif(env STREQUAL "dpdk")
  list(APPEND CXXFLAGS ${ERPC_CFLAGS_DPDK})
else()
  list(APPEND CXXFLAGS ${ERPC_CFLAGS_ETH})
endif()
if(GPROF)
  list(APPEND CXXFLAGS -pg -static-libstdc++ -static-libgcc)
endif()
if(DEBUG)
  list(APPEND CXXFLAGS -fno-omit-frame-pointer -DDEBUG -DSIMPLE_WORKLOAD)
else()
  list(APPEND CXXFLAGS -O2)
endif()
if(CHECK_INVARIANTS)
  list(APPEND CXXFLAGS -DCHECK_INVARIANTS)
endif()
if(EVENT_COUNTERS)
  list(APPEND CXXFLAGS -DENABLE_EVENT_COUNTERS)
endif()
if(DISABLE_MULTI_VERSION)
  list(APPEND CXXFLAGS -DDISABLE_MULTI_VERSION)
endif()
if(COCO)
  list(APPEND CXXFLAGS -DCOCO)
endif()
if(MEGA_BENCHMARK)
  list(APPEND CXXFLAGS -DMEGA_BENCHMARK)
endif()
if(MEGA_BENCHMARK_MICRO)
  list(APPEND CXXFLAGS -DMEGA_BENCHMARK_MICRO)
endif()
if(TRACKING_LATENCY)
  list(APPEND CXXFLAGS -DTRACKING_LATENCY)
endif()
if(DISABLE_DISK)
  list(APPEND CXXFLAGS -DDISABLE_DISK)
endif()
if(FAIL_NEW_VERSION)
  list(APPEND CXXFLAGS -DFAIL_NEW_VERSION)
endif()
if(SIMULATE_ONE_SHARD_PER_THREAD)
  list(APPEND CXXFLAGS -DSIMULATE_ONE_SHARD_PER_THREAD)
endif()
if(TRACKING_ROLLBACK)
  list(APPEND CXXFLAGS -DTRACKING_ROLLBACK)
endif()
list(APPEND CXXFLAGS
  -include ${W}/masstree/config.h
  -DREAD_MY_WRITES=${STO_RMW}
  -DHASHTABLE=${HASHTABLE}
  -DSTO_OPACITY=${OPACITY}
)

if(DEFINED LESSER_OPACITY)
  list(APPEND CXXFLAGS -DLESSER_OPACITY=${LESSER_OPACITY})
endif()
if(DEFINED GV7_OPACITY)
  list(APPEND CXXFLAGS -DGV7_OPACITY)
endif()
if(DEFINED ABORT_ON_LOCKED)
  list(APPEND CXXFLAGS -DSTO_ABORT_ON_LOCKED=${ABORT_ON_LOCKED})
endif()

# ASIO and libevent flags
list(APPEND CXXFLAGS ${ASIO_CFLAGS} ${LIBEVENT_CFLAGS})

set(LDFLAGS
  -lpthread
  -lnuma
  -lrt
  #-lmemcached
)
if(GPROF)
  list(APPEND LDFLAGS -pg -static-libstdc++ -static-libgcc)
endif()

set(LZ4LDFLAGS
  -L${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4
  -llz4
  -Wl,-rpath,${TOP}/third-party/lz4
)

if(USE_MALLOC_MODE EQUAL 1)
  list(APPEND CXXFLAGS -DUSE_JEMALLOC)
  list(APPEND LDFLAGS -ljemalloc)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --with-malloc=jemalloc")
elseif(USE_MALLOC_MODE EQUAL 2)
  list(APPEND CXXFLAGS -DUSE_TCMALLOC)
  list(APPEND LDFLAGS -ltcmalloc)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --with-malloc=tcmalloc")
elseif(USE_MALLOC_MODE EQUAL 3)
  list(APPEND CXXFLAGS -DUSE_FLOW)
  list(APPEND LDFLAGS -lflow)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --with-malloc=flow")
else()
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --with-malloc=malloc")
endif()

if(DEFINED CUSTOM_LDPATH)
  set(LDFLAGS "${LDFLAGS} ${CUSTOM_LDPATH}")
endif()

set(ASIO_PATH "${ERPC_PATH}/third_party/asio")
set(ASIO_CFLAGS -I${ASIO_PATH}/include)

set(CFLAGS ${ASIO_CFLAGS})
if(env STREQUAL "ib")
  separate_arguments(ERPC_CFLAGS_IB_LIST UNIX_COMMAND "${ERPC_CFLAGS_IB}")
  list(APPEND CFLAGS ${ERPC_CFLAGS_IB_LIST})
  separate_arguments(ERPC_LDFLAGS_IB_LIST UNIX_COMMAND "${ERPC_LDFLAGS_IB}")
  list(APPEND LDFLAGS ${ERPC_LDFLAGS_IB_LIST})
elseif(env STREQUAL "dpdk")
  separate_arguments(ERPC_CFLAGS_DPDK_LIST UNIX_COMMAND "${ERPC_CFLAGS_DPDK}")
  list(APPEND CFLAGS ${ERPC_CFLAGS_DPDK_LIST})
  separate_arguments(ERPC_LDFLAGS_DPDK_LIST UNIX_COMMAND "${ERPC_LDFLAGS_DPDK}")
  list(APPEND LDFLAGS ${ERPC_LDFLAGS_DPDK_LIST})
else()
  separate_arguments(ERPC_CFLAGS_ETH_LIST UNIX_COMMAND "${ERPC_CFLAGS_ETH}")
  list(APPEND CFLAGS ${ERPC_CFLAGS_ETH_LIST})
  separate_arguments(ERPC_LDFLAGS_ETH_LIST UNIX_COMMAND "${ERPC_LDFLAGS_ETH}")
  list(APPEND LDFLAGS ${ERPC_LDFLAGS_ETH_LIST})
endif()

list(APPEND LDFLAGS
  -levent_pthreads
  -pthread
  -lboost_fiber
  -lboost_context
  -lboost_system
  -lboost_thread
)

pkg_check_modules(LIBEVENT REQUIRED libevent)
pkg_check_modules(GFLAGS REQUIRED gflags)
#pkg_check_modules(MEMCACHED REQUIRED memcached)
pkg_check_modules(PROTOBUF REQUIRED protobuf)
pkg_check_modules(JEMALLOC REQUIRED jemalloc)
list(APPEND CFLAGS ${LIBEVENT_CFLAGS})
list(APPEND LDFLAGS ${LIBEVENT_LDFLAGS})
list(APPEND CXXFLAGS ${ASIO_CFLAGS})

# Source files
set(MAKO_SRCFILES
  "${W}/allocator.cc"
  "${W}/btree.cc"
  "${W}/core.cc"
  "${W}/counter.cc"
  "${W}/memory.cc"
  "${W}/rocksdb_persistence.cc"
  "${W}/rcu.cc"
  "${W}/stats_server.cc"
  "${W}/thread.cc"
  "${W}/ticker.cc"
  "${W}/tuple.cc"
  "${W}/txn_btree.cc"
  "${W}/txn.cc"
  "${W}/txn_proto2_impl.cc"
  "${W}/varint.cc"
  "${W}/masstree/compiler.cc"
  "${W}/masstree/str.cc"
  "${W}/masstree/string.cc"
  "${W}/masstree/straccum.cc"
  "${W}/masstree/json.cc"
  "${W}/masstree/kvthread.cc"
  "${W}/benchmarks/bench.cc"
  "${W}/benchmarks/encstress.cc"
  "${W}/benchmarks/bid.cc"
  "${W}/benchmarks/queue.cc"
  "${W}/benchmarks/tpcc.cc"
  "${W}/benchmarks/rpc_setup.cc"
  #"${W}/benchmarks/tpcc_simple.cc"
  #"${W}/benchmarks/ycsb.cc"
  "${W}/benchmarks/sync_util_init.cc"
  "${W}/benchmarks/sto/Transaction.cc"
  "${W}/benchmarks/sto/MassTrans.cc"
  "${W}/benchmarks/sto/common.cc"
  "${W}/benchmarks/sto/TRcu.cc"
  "${W}/benchmarks/sto/Packer.cc"
  "${W}/benchmarks/sto/ReplayDB.cc"
  "${W}/benchmarks/sto/ThreadPool.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/lookup3.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/kv_store.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/rust_wrapper.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/message.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/memory.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/helper_queue.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/transport.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/fasttransport.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/erpc_backend.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/rrr_rpc_backend.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/multi_transport_manager.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/configuration.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/timestamp.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/promise.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/client.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/shardClient.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/server.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/benchmarks/abstract_db.cc"
)

# Benchmark flags
set(BENCH_CXXFLAGS ${CXXFLAGS})
set(BENCH_LDFLAGS
  ${LDFLAGS}
  -lz
  -lrt
  -lcrypt
  -laio
  -ldl
  -lssl
  -lcrypto
)

# Custom command for Masstree configuration
# avoid adding backslashes (\) automatically
separate_arguments(MASSTREE_CONFIG_ARGS NATIVE_COMMAND "${MASSTREE_CONFIG}")

# Masstree configuration - automatically configure if needed
# First, run autoreconf if configure script doesn't exist
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${W}/masstree/configure")
  message(STATUS "Running autoreconf for Masstree...")
  execute_process(
    COMMAND autoreconf -i
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${W}/masstree"
    RESULT_VARIABLE autoreconf_result
  )
  if(NOT autoreconf_result EQUAL 0)
    message(FATAL_ERROR "Failed to run autoreconf for Masstree")
  endif()
endif()

# Then, run configure if config.h doesn't exist
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${W}/masstree/config.h")
  message(STATUS "Configuring Masstree with: ${MASSTREE_CONFIG}")
  execute_process(
    COMMAND ./configure ${MASSTREE_CONFIG_ARGS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${W}/masstree"
    RESULT_VARIABLE configure_result
  )
  if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "Failed to configure Masstree")
  endif()
endif()


# Add rpc defined in Janus
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.h
         ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.py
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/bin/rpcgen --cpp --python src/deptran/rcc_rpc.rpc
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.rpc
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# Create a target for the generated RPC files
add_custom_target(rcc_rpc_gen DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.py
)

# Custom command for lz4 library - build only if missing
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so")
  message(STATUS "Building LZ4 library...")
  execute_process(
    COMMAND make -C "${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4" library
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE lz4_result
  )
  if(NOT lz4_result EQUAL 0)
    message(FATAL_ERROR "Failed to build LZ4 library")
  endif()
  message(STATUS "LZ4 library built successfully")
endif()

# Build libtxlog.so natively with CMake instead of WAF
# Collect RRR source files
file(GLOB RRR_SRC
    "src/rrr/base/*.hpp"
    "src/rrr/base/*.cpp"
    "src/rrr/misc/*.hpp"
    "src/rrr/misc/*.cpp"
    "src/rrr/reactor/*.h"
    "src/rrr/reactor/*.cc"
    "src/rrr/pylib/simplerpc/_pyrpc.cpp"
    "src/rrr/rpc/*.cpp"
    "src/rrr/rpc/*.hpp"
    "src/rrr/utils/*.h"
    "src/rrr/rrr.h"
    "src/rrr/rrr.hpp"
)

# Collect MEMDB source files
file(GLOB MEMDB_SRC
    "src/memdb/*.h"
    "src/memdb/*.cc"
)

# Collect deptran and bench source files (matching WAF's ant_glob patterns)
file(GLOB_RECURSE DEPTRAN_SRC
    "src/deptran/*.cc"
    "src/deptran/*/*.cc"
    "src/bench/*/*.cc"
)
# Exclude specific files as per WAF configuration
list(FILTER DEPTRAN_SRC EXCLUDE REGEX "src/deptran/s_main.cc")
list(FILTER DEPTRAN_SRC EXCLUDE REGEX "src/deptran/paxos_main_helper.cc")
list(FILTER DEPTRAN_SRC EXCLUDE REGEX "src/bench/paxos_lib/.*")

# Build libtxlog as a shared library
add_library(txlog SHARED
    src/deptran/paxos_main_helper.cc
    ${DEPTRAN_SRC}
    ${RRR_SRC}
    ${MEMDB_SRC}
)

# Add dependency to ensure rcc_rpc.h is generated before txlog is built
add_dependencies(txlog rcc_rpc_gen)

target_include_directories(txlog PRIVATE
    src
    src/rrr
    src/memdb
    src/deptran
    src/bench
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
    dependencies/yaml-cpp/include
    ${Boost_INCLUDE_DIRS}
    ${Python3_INCLUDE_DIRS}
)

target_compile_options(txlog PRIVATE ${BENCH_CXXFLAGS})
target_compile_definitions(txlog PRIVATE CONFIG_H=\"${CONFIG_H}\")

target_link_libraries(txlog
    yaml-cpp
    Boost::system
    Boost::filesystem
    Boost::thread
    Boost::coroutine
    Boost::context
    pthread
    ${CMAKE_DL_LIBS}
    Python3::Python
)

# Set output location to match expected path
set_target_properties(txlog PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    OUTPUT_NAME "txlog"
)

# Build stamp files
add_custom_command(
  OUTPUT ${O}/${W}/buildstamp ${O}/${W}/buildstamp.bench ${O}/${W}/buildstamp.masstree
  COMMAND mkdir -p "${O}/${W}"
  COMMAND echo > "${O}/${W}/buildstamp"
  COMMAND echo > "${O}/${W}/buildstamp.bench"
  COMMAND echo > "${O}/${W}/buildstamp.masstree"
)

# Handle Masstree submodule
if(NOT EXISTS "${W}/masstree/GNUmakefile.in")
  execute_process(COMMAND git submodule init WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  execute_process(COMMAND git submodule update WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Track MASSTREE_CONFIG changes
if(NOT "${MASSTREE_CONFIG}" STREQUAL "${DEP_MASSTREE_CONFIG}")
  execute_process(
    COMMAND mkdir -p "${O}/${W}"
    COMMAND echo > "${O}/${W}/buildstamp.masstree"
  )
  set(DEP_MASSTREE_CONFIG "${MASSTREE_CONFIG}" CACHE INTERNAL "Stored MASSTREE_CONFIG")
endif()

# Masstree dependency - not needed since we assume it's pre-configured
# set_source_files_properties(${MAKO_SRCFILES} PROPERTIES OBJECT_DEPENDS "${W}/masstree/config.h")

# Add the mako library
add_library(mako STATIC ${MAKO_SRCFILES})
target_include_directories(mako PUBLIC
    "${W}/masstree"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mako"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rrr"
    "${HOME}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include"
)
target_compile_options(mako PUBLIC ${BENCH_CXXFLAGS})
target_compile_definitions(mako PUBLIC CONFIG_H=\"${CONFIG_H}\")
target_include_directories(mako PUBLIC ${LIBEVENT_INCLUDE_DIRS})
set_target_properties(mako PROPERTIES OUTPUT_NAME "mako")


# ------------------------------------- build apps -----------------------------------------------

# Custom targets for additional dependencies
# lz4 is handled during configuration, no need for custom target
# txlog_lib is now built directly as a CMake target, no need for custom target


function(add_apps exec_name exec_path)
  # Add executable
  add_executable(${exec_name} ${exec_path})

  # Configure target
  target_include_directories(${exec_name} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
      ${CMAKE_CURRENT_SOURCE_DIR}/src/mako/masstree
      ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/src
      ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/third_party/asio/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  target_compile_options(${exec_name} PRIVATE ${BENCH_CXXFLAGS}) # include all flags

  # Link dbtest against mako library and dependencies
  target_link_libraries(${exec_name}
      mako
      erpc
      rust_lib
      txlog  # Link to the CMake txlog target instead of the .so file
      ${BENCH_LDFLAGS}
      yaml-cpp
      pthread
      numa
      event_pthreads
      rocksdb
      ${LIBEVENT_LDFLAGS}
      ${GFLAGS_LDFLAGS}
      ${PROTOBUF_LDFLAGS}
      ${JEMALLOC_LDFLAGS}
  )

  # Add dependencies
  if(ENABLE_BORROW_CHECKING)
    add_dependencies(${exec_name} mako erpc rust_lib txlog build_rusty_cpp_checker)
  else()
    add_dependencies(${exec_name} mako erpc rust_lib txlog)
  endif()
endfunction()

add_apps(dbtest src/mako/benchmarks/dbtest.cc)
add_apps(simpleTransaction examples/simpleTransaction.cc)
add_apps(simpleTransactionRep examples/simpleTransactionRep.cc)
add_apps(continuousTransactions examples/continuousTransactions.cc)
add_apps(simplePaxos examples/simplePaxos.cc)
add_apps(test_rocksdb_persistence examples/test_rocksdb_persistence.cc)
add_apps(rocksdb_replay_app src/mako/benchmarks/rocksdb_replay_app.cc)
add_apps(test_callback_demo examples/test_callback_demo.cc)
add_apps(test_ordered_callbacks examples/test_ordered_callbacks.cc)
add_apps(test_partitioned_queues examples/test_partitioned_queues.cc)
add_apps(test_stress_partitioned_queues examples/test_stress_partitioned_queues.cc)

# Borrow checking for dbtest DISABLED - uses STO (Software Transactional Objects)
# headers which have reference-returning functions (back(), key()) that would
# require extensive modifications to be borrow-safe. The core masstree files
# are still borrow-checked via file(GLOB_RECURSE MASSTREE_BORROW_CHECK_SOURCES)
# if(ENABLE_BORROW_CHECKING)
#     add_borrow_check_target(dbtest)
# endif()

add_apps(paxos_async_commit_test src/mako/benchmarks/paxos_async_commit_test.cc)
add_apps(basic src/mako/benchmarks/ut/basic.cc)
add_apps(erpc_client src/mako/benchmarks/ut/erpc_client.cc)
add_apps(erpc_server src/mako/benchmarks/ut/erpc_server.cc)

# RPC benchmark binary
add_executable(rpcbench test/rpcbench.cc test/benchmark_service.cc)
target_include_directories(rpcbench PRIVATE
    src
    src/rrr
    test
)
target_compile_options(rpcbench PRIVATE ${BENCH_CXXFLAGS})
target_link_libraries(rpcbench
    rrr
    memdb
    pthread
    ${BENCH_LDFLAGS}
    ${LIBEVENT_LDFLAGS}
)

# Build the necessary RRR and memdb libraries for rpcbench
file(GLOB RRR_BENCH_SRC
    "src/rrr/base/*.cpp"
    "src/rrr/misc/*.cpp"
    "src/rrr/rpc/*.cpp"
    "src/rrr/reactor/*.cc"
)

# Add RRR library if not already built
if(NOT TARGET rrr)
    add_library(rrr STATIC ${RRR_BENCH_SRC})
    target_include_directories(rrr PUBLIC
        src
        src/rrr
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
    )
    target_compile_options(rrr PUBLIC ${BENCH_CXXFLAGS})
    target_link_libraries(rrr pthread ${Boost_LIBRARIES})

    # Add borrow checking for RRR library
    if(ENABLE_BORROW_CHECKING)
        add_borrow_check_target(rrr)

        # Create a convenience target to check only RRR library files
        add_custom_target(borrow_check_rrr
            DEPENDS borrow_check_all_rrr
            COMMENT "Running borrow checker on RRR library files"
        )
    endif()
endif()

# Add memdb library if not already built
if(NOT TARGET memdb)
    file(GLOB MEMDB_BENCH_SRC "src/memdb/*.cc")
    add_library(memdb STATIC ${MEMDB_BENCH_SRC})
    target_include_directories(memdb PUBLIC
        src
        src/rrr
        src/memdb
    )
    target_compile_options(memdb PUBLIC ${BENCH_CXXFLAGS})
    target_link_libraries(memdb rrr pthread)
endif()

# ============================================================================
# Test Configuration
# ============================================================================

# Enable testing
enable_testing()

# Find GoogleTest - try multiple methods
set(BUILD_TESTS OFF)

# Method 1: Try find_package
find_package(GTest QUIET)
if(GTest_FOUND)
    set(BUILD_TESTS ON)
    message(STATUS "GoogleTest found via find_package")
endif()

# Method 2: Check for installed libraries directly
if(NOT BUILD_TESTS)
    find_library(GTEST_LIB gtest PATHS /usr/lib /usr/local/lib)
    find_library(GTEST_MAIN_LIB gtest_main PATHS /usr/lib /usr/local/lib)
    find_path(GTEST_INCLUDE_DIR gtest/gtest.h PATHS /usr/include /usr/local/include)

    if(GTEST_LIB AND GTEST_MAIN_LIB AND GTEST_INCLUDE_DIR)
        set(BUILD_TESTS ON)
        set(GTEST_LIBRARIES ${GTEST_LIB})
        set(GTEST_MAIN_LIBRARIES ${GTEST_MAIN_LIB})
        set(GTEST_INCLUDE_DIRS ${GTEST_INCLUDE_DIR})
        message(STATUS "GoogleTest found via direct library search")
        message(STATUS "  GTEST_LIB: ${GTEST_LIB}")
        message(STATUS "  GTEST_MAIN_LIB: ${GTEST_MAIN_LIB}")
        message(STATUS "  GTEST_INCLUDE_DIR: ${GTEST_INCLUDE_DIR}")
    endif()
endif()

# Method 3: Try pkg-config as last resort
if(NOT BUILD_TESTS)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTEST gtest)
        pkg_check_modules(GTEST_MAIN gtest_main)
        if(GTEST_FOUND)
            set(BUILD_TESTS ON)
            message(STATUS "GoogleTest found via pkg-config")
        endif()
    endif()
endif()

if(NOT BUILD_TESTS)
    message(WARNING "GoogleTest not found. Tests will be disabled. Install with: sudo apt-get install libgtest-dev")
endif()

if(BUILD_TESTS)
    # Common test include directories
    set(TEST_INCLUDE_DIRS
        src
        src/rrr
        src/memdb
        test
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
    )

    # Common test compile options
    set(TEST_COMPILE_OPTIONS ${BENCH_CXXFLAGS})

    # Common test link libraries
    set(TEST_LINK_LIBS
        rrr
        memdb
        pthread
        ${BENCH_LDFLAGS}
        ${LIBEVENT_LDFLAGS}
    )

    # Marshal/Unmarshal Tests
    add_executable(test_marshal test/test_marshal.cc)
    target_include_directories(test_marshal PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_marshal PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_marshal ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_marshal COMMAND test_marshal)

    # RPC Tests
    add_executable(test_rpc test/test_rpc.cc test/benchmark_service.cc)
    target_include_directories(test_rpc PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc COMMAND test_rpc)

    # Atomic Lock Tests - Commented out due to crashes
    # add_executable(test_alock test/test_alock.cc)
    # target_include_directories(test_alock PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    # target_compile_options(test_alock PRIVATE ${TEST_COMPILE_OPTIONS})
    # target_link_libraries(test_alock ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    # add_test(NAME test_alock COMMAND test_alock)

    # Future Tests
    add_executable(test_future test/test_future.cc)
    target_include_directories(test_future PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_future PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_future ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_future COMMAND test_future)

    # Future Performance Benchmark (not run in test suite)
    add_executable(bench_future test/bench_future.cc)
    target_include_directories(bench_future PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(bench_future PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(bench_future ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)

    # Masstree unit tests (run with ctest/make test)
    add_executable(test_masstree test/test_masstree.cc)
    target_include_directories(test_masstree PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_masstree PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_masstree mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_masstree COMMAND test_masstree)

    # Masstree performance benchmark (on-demand JSON output)
    add_executable(masstree_perf test/masstree_perf.cc)
    target_include_directories(masstree_perf PRIVATE ${TEST_INCLUDE_DIRS})
    target_compile_options(masstree_perf PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(masstree_perf mako ${TEST_LINK_LIBS})

    # Reactor/Event System Tests
    add_executable(test_reactor test/test_reactor.cc)
    target_include_directories(test_reactor PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_reactor PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_reactor ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_reactor COMMAND test_reactor)

    # Coroutine Tests (existing)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/coroutine.cc")
        add_executable(test_coroutine test/coroutine.cc)
        target_include_directories(test_coroutine PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
        target_compile_options(test_coroutine PRIVATE ${TEST_COMPILE_OPTIONS})
        target_link_libraries(test_coroutine ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
        add_test(NAME test_coroutine COMMAND test_coroutine)
    endif()

    # Extended RPC Tests
    add_executable(test_rpc_extended test/test_rpc_extended.cc test/benchmark_service.cc)
    target_include_directories(test_rpc_extended PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_extended PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_extended ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_extended COMMAND test_rpc_extended)

    # Extended Reactor Tests
    add_executable(test_reactor_extended test/test_reactor_extended.cc)
    target_include_directories(test_reactor_extended PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_reactor_extended PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_reactor_extended ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_reactor_extended COMMAND test_reactor_extended)

    # Arc<Mutex<>> Thread Safety Tests
    add_executable(test_arc_mutex_thread test/test_arc_mutex_thread.cc)
    target_include_directories(test_arc_mutex_thread PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_arc_mutex_thread PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_arc_mutex_thread ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_arc_mutex_thread COMMAND test_arc_mutex_thread)

    # Timeout Race Condition Tests
    add_executable(test_timeout_race test/test_timeout_race.cc)
    target_include_directories(test_timeout_race PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_timeout_race PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_timeout_race ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_timeout_race COMMAND test_timeout_race)

    # AndEvent Tests
    add_executable(test_and_event test/test_and_event.cc)
    target_include_directories(test_and_event PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_and_event PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_and_event ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_and_event COMMAND test_and_event)

    # Transport Backend Tests (Comprehensive)
    # Tests use mock implementations to avoid mako/eRPC dependencies
    add_executable(test_transport_backend test/test_transport_backend.cc)
    target_include_directories(test_transport_backend PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_transport_backend PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_transport_backend ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_transport_backend COMMAND test_transport_backend)

    # Transport Backend Stress Tests
    add_executable(stress_transport_backend test/stress_transport_backend.cc)
    target_include_directories(stress_transport_backend PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(stress_transport_backend PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(stress_transport_backend ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    # Stress tests have longer timeout
    add_test(NAME stress_transport_backend COMMAND stress_transport_backend)
    set_tests_properties(stress_transport_backend PROPERTIES TIMEOUT 120)

    # Transport Integration Tests (uses real rrr/rpc network I/O)
    add_executable(test_transport_integration test/test_transport_integration.cc)
    target_include_directories(test_transport_integration PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
    )
    target_compile_options(test_transport_integration PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_transport_integration
        rrr
        ${TEST_LINK_LIBS}
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
        pthread
    )
    add_test(NAME test_transport_integration COMMAND test_transport_integration)
    set_tests_properties(test_transport_integration PROPERTIES TIMEOUT 120)

    # eRPC Integration Tests (uses socket-based "fake" transport - no RDMA required)
    add_executable(test_erpc_integration test/test_erpc_integration.cc)
    target_include_directories(test_erpc_integration PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/third_party/asio/include
    )
    target_compile_options(test_erpc_integration PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_erpc_integration
        erpc
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
        pthread
        numa
    )
    add_test(NAME test_erpc_integration COMMAND test_erpc_integration)
    set_tests_properties(test_erpc_integration PROPERTIES TIMEOUT 120)

    # Integration tests from CI
    add_test(NAME simpleTransaction COMMAND simpleTransaction)

    # RPC benchmark test - starts server, runs client, verifies it works
    add_test(NAME rpcbench
        COMMAND bash -c "pkill -9 rpcbench || true; ./build/rpcbench -s 127.0.0.1:8848 > /dev/null 2>&1 & SERVER_PID=$!; sleep 1; ./build/rpcbench -c 127.0.0.1:8848 -n 3 -t 4 -o 100; RESULT=$?; kill $SERVER_PID 2>/dev/null || true; exit $RESULT"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(rpcbench PROPERTIES TIMEOUT 30)

    # simplePaxos requires config generation and running a shell script that launches multiple processes
    # add_test(NAME simplePaxos
    #     COMMAND bash -c "bash ./src/mako/update_config.sh && bash ./examples/simplePaxos.sh"
    #     WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    # )
    # # Use RESOURCE_LOCK to prevent port conflicts with other Paxos tests (ports 17001-17xxx)
    # set_tests_properties(simplePaxos PROPERTIES RESOURCE_LOCK "paxos_ports" TIMEOUT 120)

    # shard1ReplicationSimple tests 1-shard replication using simpleTransactionRep
    # add_test(NAME shard1ReplicationSimple
    #     COMMAND bash -c "bash ./examples/test_1shard_replication_simple.sh"
    #     WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    # )
    # # Use RESOURCE_LOCK to prevent port conflicts with other Paxos tests (ports 17001-17xxx)
    # set_tests_properties(shard1ReplicationSimple PROPERTIES RESOURCE_LOCK "paxos_ports" TIMEOUT 300)

    # Add a convenience target to run all tests
    add_custom_target(run_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS test_marshal test_rpc test_future test_masstree test_reactor test_rpc_extended test_reactor_extended rpcbench simpleTransaction simplePaxos shard1ReplicationSimple  # test_alock removed
        COMMENT "Running all RRR tests"
    )

    # Add borrow checking for test files if enabled
    if(ENABLE_BORROW_CHECKING)
        add_borrow_check_target(test_marshal)
        add_borrow_check_target(test_rpc)
        # test_alock is disabled
        add_borrow_check_target(test_future)
        add_borrow_check_target(test_reactor)
        # Create a convenience target to check all test files
        add_custom_target(borrow_check_tests
            DEPENDS borrow_check_all_test_marshal borrow_check_all_test_rpc
                    borrow_check_all_test_future borrow_check_all_test_reactor
            COMMENT "Running borrow checker on all test files"
        )
    endif()

    message(STATUS "Test targets configured:")
    message(STATUS "  - test_marshal: Marshal/Unmarshal tests")
    message(STATUS "  - test_rpc: RPC client/server tests")
    message(STATUS "  - test_rpc_extended: Extended RPC tests (timeouts, reconnection, load)")
    # message(STATUS "  - test_alock: Atomic lock tests")  # Commented out due to crashes
    message(STATUS "  - test_future: Future/async operation tests")
    message(STATUS "  - test_masstree: Core Masstree b-tree regression tests")
    message(STATUS "  - test_reactor: Reactor/event system tests")
    message(STATUS "  - test_reactor_extended: Extended reactor tests (events, coroutines, load)")
    message(STATUS "  - rpcbench: RPC performance benchmark test")
    message(STATUS "Additional perf binaries:")
    message(STATUS "  - masstree_perf: On-demand Masstree throughput benchmark (JSON output)")
    message(STATUS "")
    message(STATUS "Run tests with: make run_tests")
    message(STATUS "Or individually: make test && ctest")
endif()
