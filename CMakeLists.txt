cmake_minimum_required(VERSION 3.10)

# Suppress CMake policy warnings for deprecated modules
# CMP0167: FindBoost module is removed in CMake 3.30+, but we still support older CMake
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)  # Use deprecated FindBoost for compatibility
endif()

# using g++
set(CMAKE_C_COMPILER "gcc" CACHE STRING "C compiler")
set(CMAKE_CXX_COMPILER "g++" CACHE STRING "C++ compiler")

# Enable compile_commands.json generation for language servers (clangd, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(mako)

# Prefer shared libraries when available for third-party dependencies.
set(BUILD_SHARED_LIBS ON)

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

# Auto-detect the latest available libclang for rusty-cpp-checker.
# The checker uses libclang to parse C++ headers; if the system installs newer
# clang headers (e.g., clang 19 with __builtin_nondeterministic_value in AVX
# intrinsics), the checker must be linked against the matching libclang version
# to avoid "Fatal parsing errors" from unknown builtins.
if(NOT DEFINED ENV{LIBCLANG_PATH})
    # Search for the newest libclang, preferring higher versions
    foreach(_CLANG_VER 19 18 17 16 15 14)
        set(_CLANG_LIB_DIR "/usr/lib/llvm-${_CLANG_VER}/lib")
        if(EXISTS "${_CLANG_LIB_DIR}/libclang-${_CLANG_VER}.so" OR
           EXISTS "${_CLANG_LIB_DIR}/libclang.so")
            set(ENV{LIBCLANG_PATH} "${_CLANG_LIB_DIR}")
            message(STATUS "Auto-detected LIBCLANG_PATH=${_CLANG_LIB_DIR} (clang ${_CLANG_VER})")
            break()
        endif()
    endforeach()
endif()

# Configure and enable rusty-cpp-checker
include(${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/cmake/RustyCppSubmodule.cmake)
set(RUSTYCPP_BUILD_TYPE "release")  # or "debug"

# NOTE: RustyCppSubmodule.cmake already computes CPP_BORROW_CHECKER and creates
# build targets. Do not override checker paths here or Docker CARGO_TARGET_DIR
# support is bypassed.

# Enable borrow checking (can be disabled with -DENABLE_BORROW_CHECKING=OFF)
# Don't use FORCE so command-line option takes precedence
set(ENABLE_BORROW_CHECKING ON CACHE BOOL "Enable RustyCpp borrow checking")
if(ENABLE_BORROW_CHECKING)
    enable_borrow_checking()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

# Find Python for txlog_core/txlog_bench build surfaces.
find_package(Python3 COMPONENTS Development REQUIRED)

# Define paths
set(mkfile_path "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
set(HOME "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ERPC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc")
message(STATUS "I: ${ERPC_PATH}")
set(W "src/mako")
set(TOP "${CMAKE_SOURCE_DIR}")

# Build yaml-cpp from vendored source to avoid system ABI/version drift.
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "Disable yaml-cpp tests" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "Disable yaml-cpp tools" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "Disable yaml-cpp install targets" FORCE)
add_subdirectory(third-party/yaml-cpp)

# RUST
# Respect CARGO_TARGET_DIR environment variable if set (for Docker builds)
if(DEFINED ENV{CARGO_TARGET_DIR})
    set(RUST_LIB_TARGET_BASE "$ENV{CARGO_TARGET_DIR}")
    message(STATUS "Using CARGO_TARGET_DIR for rust-lib: ${RUST_LIB_TARGET_BASE}")
else()
    set(RUST_LIB_TARGET_BASE "${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/target")
endif()
set(RUST_LIB_DIR "${RUST_LIB_TARGET_BASE}/release")
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

# eRPC's latency target enables -flto at compile time when PERF=ON. With Clang,
# object files become LLVM bitcode and require matching link-time -flto.
if(TARGET latency AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_link_options(latency PRIVATE -flto)
endif()

# Keep all eRPC artifacts inside the active build tree.
# The eRPC submodule sets output dirs to ${CMAKE_SOURCE_DIR}/build, which
# pollutes host builds when compiling in Docker.
set(ERPC_BINARY_DIR "${CMAKE_BINARY_DIR}/third-party/erpc")
get_property(ERPC_TARGETS DIRECTORY "${ERPC_BINARY_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
foreach(ERPC_TARGET IN LISTS ERPC_TARGETS)
  if(TARGET "${ERPC_TARGET}")
    get_target_property(ERPC_TARGET_TYPE "${ERPC_TARGET}" TYPE)
    if(NOT ERPC_TARGET_TYPE STREQUAL "INTERFACE_LIBRARY")
      set_target_properties("${ERPC_TARGET}" PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${ERPC_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${ERPC_BINARY_DIR}"
      )
    endif()
  endif()
endforeach()

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
option(MAKO_USE_RAFT "Build Mako with the Raft replication helper" OFF)
option(RAFT_TEST "Enable Raft lab testing coroutine" OFF)
option(SINGLE_RAFT_INSTANCE "Single Raft instance for all partitions (OFF=multi-instance, one per partition)" ON)

if(MAKO_USE_RAFT)
  add_compile_definitions(MAKO_USE_RAFT=1)
  message(STATUS "Mako Raft replication enabled")
endif()

if(RAFT_TEST)
  add_compile_definitions(RAFT_TEST_CORO=1)
  message(STATUS "Raft lab testing coroutine enabled (note: REUSE_CORO is always enabled)")
endif()

if(SINGLE_RAFT_INSTANCE)
  add_compile_definitions(SINGLE_RAFT_INSTANCE=1)
  message(STATUS "Single Raft instance mode enabled (all partitions in one Raft group)")
else()
  message(STATUS "Multi Raft instance mode (one Raft group per partition)")
endif()

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
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/third_party/masstree-beta
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/third_party/asio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
)

# Make config paths visible to the borrow checker (will be turned into -include)
add_compile_definitions(MASSTREE_CONFIG_H=\"${CMAKE_CURRENT_SOURCE_DIR}/${W}/masstree/config.h\")
add_compile_definitions(MASSTREE_SUPPORT_HEADER=\"${CMAKE_CURRENT_SOURCE_DIR}/src/mako/masstree_btree.h\")

# Masstree borrow checking - incrementally enabled per file
# Phase 4 of RustyCpp migration enables borrow checking file by file
# See TODO.md "Masstree RustyCpp Safety Migration" for details
#
# Previously disabled due to false positives that have been addressed by:
#   - Adding @unsafe { reason } annotations to all unsafe functions
#   - Converting raw pointers to rusty::Ptr<T>/MutPtr<T> wrappers
#   - Marking safe getters/setters with @safe annotations
#
# Phase 4.1: Enable borrow checking for masstree_context
add_borrow_check(src/mako/masstree/masstree_context.cc)
#
# Phase 4.2: Enable borrow checking for kvthread
add_borrow_check(src/mako/masstree/kvthread.cc)
#
# Phase 4.3: Enable more files incrementally
add_borrow_check(src/mako/masstree/value_versioned_array.cc)
add_borrow_check(src/mako/masstree/query_masstree.cc)

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

# Enable coroutine reuse (required for Raft stability)
list(APPEND CXXFLAGS -DREUSE_CORO)
message(STATUS "Reuse coroutine enabled")

# Add RAFT_TEST_CORO flag if enabled
if(RAFT_TEST)
  message(STATUS "Raft lab testing coroutine enabled")
  list(APPEND CXXFLAGS -DRAFT_TEST_CORO)
endif()

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
  list(APPEND CXXFLAGS -O2 -g)
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
)

pkg_check_modules(LIBEVENT REQUIRED libevent)
pkg_check_modules(GFLAGS REQUIRED gflags)
#pkg_check_modules(MEMCACHED REQUIRED memcached)
pkg_check_modules(PROTOBUF REQUIRED protobuf)
pkg_check_modules(JEMALLOC REQUIRED jemalloc)
list(APPEND CFLAGS ${LIBEVENT_CFLAGS})
list(APPEND LDFLAGS ${LIBEVENT_LDFLAGS})
list(APPEND CXXFLAGS ${ASIO_CFLAGS})
list(APPEND CXXFLAGS -fno-omit-frame-pointer)

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
  "${W}/masstree/masstree_context.cc"
  "${W}/silo_runtime.cc"
  "${W}/benchmarks/bench.cc"
  "${W}/benchmarks/encstress.cc"
  "${W}/benchmarks/bid.cc"
  "${W}/benchmarks/queue.cc"
  "${W}/benchmarks/tpcc.cc"
  "${W}/benchmarks/rpc_setup.cc"
  #"${W}/benchmarks/config_node_init.cc"  # TODO: Enable when rrr/mako header conflicts are resolved
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
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/db.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/client_service.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/client_proxy.cc"
)

set(MAKO_USING_FRAGILEC OFF)
if(CMAKE_CXX_COMPILER MATCHES "fragilec")
  set(MAKO_USING_FRAGILEC ON)
endif()

if(MAKO_USING_FRAGILEC)
  # Strict fragilec benchmark runs only need the core Masstree/library paths.
  # Exclude heavyweight benchmark modules that currently pull in unresolved
  # benchmark-config API surfaces and block `masstree_perf` builds.
  list(FILTER MAKO_SRCFILES EXCLUDE REGEX "/benchmarks/(encstress|bid|queue|tpcc|rpc_setup|sync_util_init)\\.cc$")
  list(FILTER MAKO_SRCFILES EXCLUDE REGEX "/benchmarks/sto/.*\\.cc$")
endif()

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

# Collect deptran core source files
file(GLOB_RECURSE DEPTRAN_CORE_SRC
    "src/deptran/*.cc"
    "src/deptran/*/*.cc"
)
# Exclude specific deptran files (standalone binaries / optional backends)
list(FILTER DEPTRAN_CORE_SRC EXCLUDE REGEX "src/deptran/s_main.cc")
list(FILTER DEPTRAN_CORE_SRC EXCLUDE REGEX "src/deptran/paxos_main_helper.cc")
list(FILTER DEPTRAN_CORE_SRC EXCLUDE REGEX "src/deptran/raft_main_helper.cc")
list(FILTER DEPTRAN_CORE_SRC EXCLUDE REGEX "src/deptran/mongodb/.*")
list(FILTER DEPTRAN_CORE_SRC EXCLUDE REGEX "src/deptran/raft/read_raft_disk.cc")
list(FILTER DEPTRAN_CORE_SRC EXCLUDE REGEX "src/deptran/raft/test_raft_persistence.cc")

# Collect benchmark source files (compiled separately from deptran core)
file(GLOB_RECURSE TXLOG_BENCH_SRC
    "src/bench/*/*.cc"
)
list(FILTER TXLOG_BENCH_SRC EXCLUDE REGEX "src/bench/paxos_lib/.*")

# Runtime replication switching: compile BOTH Paxos and Raft helpers
# The replication_helper.cc dispatcher routes calls based on runtime configuration
set(TXLOG_HELPER_SRC
    src/deptran/replication_helper.cc
    src/deptran/paxos_main_helper.cc
    src/deptran/raft_main_helper.cc
)

# Build txlog in layers:
#   - txlog_core: deptran/rrr/memdb core (no src/bench/* compilation unit)
#   - txlog_bench: benchmark implementations from src/bench/*
add_library(txlog_core_obj OBJECT
    ${TXLOG_HELPER_SRC}
    ${DEPTRAN_CORE_SRC}
    ${RRR_SRC}
    ${MEMDB_SRC}
)
add_dependencies(txlog_core_obj rcc_rpc_gen)
set_target_properties(txlog_core_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(txlog_core_obj PRIVATE
    src
    src/rrr
    src/memdb
    src/deptran
    src/bench
    src/mako
    src/mako/benchmarks
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
    third-party/yaml-cpp/include
    ${Python3_INCLUDE_DIRS}
)
target_compile_options(txlog_core_obj PRIVATE ${BENCH_CXXFLAGS})
target_compile_definitions(txlog_core_obj PRIVATE CONFIG_H=\"${CONFIG_H}\")

add_library(txlog_bench_obj OBJECT ${TXLOG_BENCH_SRC})
add_dependencies(txlog_bench_obj rcc_rpc_gen)
set_target_properties(txlog_bench_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(txlog_bench_obj PRIVATE
    src
    src/rrr
    src/memdb
    src/deptran
    src/bench
    src/mako
    src/mako/benchmarks
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
    third-party/yaml-cpp/include
    ${Python3_INCLUDE_DIRS}
)
target_compile_options(txlog_bench_obj PRIVATE ${BENCH_CXXFLAGS})
target_compile_definitions(txlog_bench_obj PRIVATE CONFIG_H=\"${CONFIG_H}\")

add_library(txlog_core STATIC $<TARGET_OBJECTS:txlog_core_obj>)
target_link_libraries(txlog_core
    PUBLIC
    yaml-cpp
    pthread
    rocksdb
    ${CMAKE_DL_LIBS}
    Python3::Python
)
target_include_directories(txlog_core PUBLIC
    src
    src/rrr
    src/memdb
    src/deptran
)

add_library(txlog_bench STATIC $<TARGET_OBJECTS:txlog_bench_obj>)
target_link_libraries(txlog_bench PUBLIC txlog_core)
target_include_directories(txlog_bench PUBLIC
    src
    src/bench
)

# Canonical link set for targets that previously linked the monolithic txlog.
# Keep txlog_core before/after txlog_bench to satisfy cross-archive refs on ld.
add_library(txlog_full INTERFACE)
target_link_libraries(txlog_full INTERFACE
    txlog_core
    txlog_bench
    txlog_core
)
# Ensure bench registry object is linked from static txlog_bench.
target_link_options(txlog_full INTERFACE
    "LINKER:-u,janus_register_bench_factories"
)
set(TXLOG_LINK_LIBS
    txlog_full
)
set(TXLOG_BUILD_DEPS
    txlog_core
    txlog_bench
)

# Temporary Raft-only target for borrow checking
file(GLOB RAFT_BORROW_SRC
    "src/deptran/raft/*.cc"
    "src/deptran/raft/*.cpp"
)
# Exclude test files from raft borrow checking:
# - testconf.cc: Test infrastructure with class inheritance patterns not compatible with @safe code
# - test.cc: Test harness that depends on testconf.h
# NOTE: raft_main_helper.cc is also excluded because it includes third-party headers
# (YAML, etc.) that generate 1000+ false positive violations from header code.
#   - replicated_db.cc: includes <rocksdb/c.h> third-party header that generates
#     false positive violations from header code.
list(FILTER RAFT_BORROW_SRC EXCLUDE REGEX ".*test(conf)?\\.cc$")
list(FILTER RAFT_BORROW_SRC EXCLUDE REGEX ".*replicated_db\\.cc$")

if(RAFT_BORROW_SRC)
    add_library(raft_borrow OBJECT ${RAFT_BORROW_SRC})
    set_target_properties(raft_borrow PROPERTIES EXCLUDE_FROM_ALL YES)

    target_include_directories(raft_borrow PUBLIC
        src
        src/rrr
        src/deptran
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
    )
    target_compile_options(raft_borrow PUBLIC ${BENCH_CXXFLAGS})
    target_compile_definitions(raft_borrow PUBLIC CONFIG_H="${CONFIG_H}")

    if(ENABLE_BORROW_CHECKING AND CPP_BORROW_CHECKER)
        set(RAFT_BORROW_STAMPS)
        foreach(SOURCE ${RAFT_BORROW_SRC})
            get_filename_component(FILE_NAME ${SOURCE} NAME_WE)
            get_filename_component(SOURCE_ABS ${SOURCE} ABSOLUTE)
            set(STAMP_FILE "${CMAKE_CURRENT_BINARY_DIR}/borrow_check_raft_${FILE_NAME}.stamp")

            add_custom_command(
                OUTPUT ${STAMP_FILE}
                COMMAND ${CPP_BORROW_CHECKER} --compile-commands ${CMAKE_BINARY_DIR}/compile_commands.json ${SOURCE_ABS}
                COMMAND ${CMAKE_COMMAND} -E touch ${STAMP_FILE}
                DEPENDS ${SOURCE_ABS} ${CPP_BORROW_CHECKER}
                COMMENT "Borrow checking ${SOURCE}"
                VERBATIM
            )

            list(APPEND RAFT_BORROW_STAMPS ${STAMP_FILE})
        endforeach()

        add_custom_target(borrow_check_raft
            DEPENDS ${RAFT_BORROW_STAMPS}
            COMMENT "Running borrow checker on Raft sources only"
        )

        if(TARGET build_rusty_cpp_checker)
            add_dependencies(borrow_check_raft build_rusty_cpp_checker)
        endif()
    endif()
endif()

# Deptran worker files for borrow checking (critical files using rusty::Option/Arc)
# These files had use-after-move bugs that weren't caught because borrow checking
# was not enabled for src/deptran/
#
# NOTE: Deptran borrow checking is currently disabled due to false positives from
# recent rusty-cpp updates that enforce stricter uniform borrow rules for pointers
# and references. The following errors are triggered across multiple files:
#   - "Cannot return 'value' because it has been moved"
#   - "Cannot borrow from 'this': variable is not alive in current scope"
#   - "Cannot modify field 'm_pNode' in const method"
#   - "Cannot call method on 'this.epochs_': field is borrowed by ei"
# These appear to be false positives related to how the checker handles method
# calls on this, return value tracking, and const method field access in C++ code.
# Re-enable when these issues are resolved in rusty-cpp.
#
# Previously excluded files (for reference):
#   - raft_main_helper.cc: includes third-party headers (YAML, etc.) that generate
#     1000+ false positive violations from header code.
#   - replication_helper.cc: thin dispatcher that calls non-borrow-checked impls.
#     Uses rusty::Cell for safe interior mutability of global state.
#
# Deptran borrow checking - all files pass after rusty-cpp fixes:
# - Cell/Condvar const propagation fixed (skips @safe callees with internal @unsafe)
# - Unannotated third-party headers skipped (yaml-cpp, STL, etc.)
#
# NOTE: server_worker.cc and client_worker.cc excluded - rusty-cpp flags rrr header functions
#       (RefCell::borrow, Option::unwrap, etc.) as violations when they appear in return
#       statements from headers included by these files. This is a false positive (the files
#       themselves are safe). TODO: Report to rusty-cpp - header functions shouldn't be checked.
set(DEPTRAN_BORROW_SRC
    src/deptran/communicator.cc
    src/deptran/workload.cc
    # src/deptran/server_worker.cc  # Excluded: false positives from rrr header functions
    # src/deptran/client_worker.cc  # Excluded: false positives from rrr header functions
    src/deptran/paxos_worker.cc
    src/deptran/stats_registry.cc
    src/deptran/executor.cc
    src/deptran/empty.cc
    src/deptran/replication_helper.cc
    src/deptran/scheduler.cc
    # Files below excluded due to violations from included headers (not the .cc files themselves):
    # src/deptran/config_client.cc          # 6 violations from RPC proxy headers
    # src/deptran/config_service.cc         # 7 violations from sharding_policy.h, RPC base class
    # src/deptran/config_store.cc           # 12 violations from RocksDB/Marshal headers
    # src/deptran/shard_router.cc           # 51 violations from masstree/sharding headers
    # src/deptran/sharding_policy_cache.cc  # 14 violations from sharding/Mutex headers
    # src/deptran/tpcc_sharding.cc          # 23 violations from sharding_policy_builder.h headers
)

if(DEPTRAN_BORROW_SRC)
    add_library(deptran_borrow OBJECT ${DEPTRAN_BORROW_SRC})
    set_target_properties(deptran_borrow PROPERTIES EXCLUDE_FROM_ALL YES)

    target_include_directories(deptran_borrow PUBLIC
        src
        src/rrr
        src/memdb
        src/deptran
        src/bench
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
        third-party/yaml-cpp/include
    )
    target_compile_options(deptran_borrow PUBLIC ${BENCH_CXXFLAGS})
    target_compile_definitions(deptran_borrow PUBLIC CONFIG_H="${CONFIG_H}")

    # Add borrow checking for deptran files (same style as RRR library)
    if(ENABLE_BORROW_CHECKING)
        add_borrow_check_target(deptran_borrow)

        # Create a convenience target to check only deptran files
        add_custom_target(borrow_check_deptran
            DEPENDS borrow_check_all_deptran_borrow
            COMMENT "Running borrow checker on Deptran worker sources"
        )
    endif()
endif()

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
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/yaml-cpp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rrr"
    "${HOME}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include"
)
target_compile_options(mako PUBLIC ${BENCH_CXXFLAGS})
target_compile_definitions(mako PUBLIC CONFIG_H=\"${CONFIG_H}\")
target_include_directories(mako PUBLIC ${LIBEVENT_INCLUDE_DIRS})
set_target_properties(mako PROPERTIES OUTPUT_NAME "mako")

# ============================================================================
# BORROW CHECKER REGISTRATION SILO/STO
# ============================================================================

if(ENABLE_BORROW_CHECKING)
    message(STATUS "--- Configuring Mako Root & STO Borrow Checks ---")

    # NOTE: Mako root source files (src/mako/*.cc) and STO benchmarks are currently excluded
    # from borrow checking because they use many undeclared functions (atomics, Masstree internals, etc.)
    # Re-enable incrementally as files are properly annotated with @safe/@unsafe
    #
    # file(GLOB MAKO_ROOT_SRCS
    #     "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/*.cc"
    #     "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/*.c"
    # )
    # file(GLOB_RECURSE STO_SRCS
    #     "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/benchmarks/sto/*.cc"
    #     "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/benchmarks/sto/*.c"
    # )
    set(MAKO_ROOT_SRCS "")
    set(STO_SRCS "")

    # Combine lists
    set(TARGET_BORROW_SRCS ${MAKO_ROOT_SRCS} ${STO_SRCS})
    
    # Debug output
    list(LENGTH TARGET_BORROW_SRCS FILE_COUNT)
    message(STATUS "Found ${FILE_COUNT} files to check in src/mako/ and benchmarks/sto/")

    foreach(SRC ${TARGET_BORROW_SRCS})
        get_filename_component(STEM ${SRC} NAME_WE)
        set(CHECK_TARGET "borrow_check_${STEM}")

        if(NOT TARGET ${CHECK_TARGET})
            add_borrow_check(${SRC})
            add_dependencies(mako ${CHECK_TARGET})
        else()
            # If a collision still happens, just link the existing target
            message(STATUS "Linking existing borrow check target: ${CHECK_TARGET}")
            add_dependencies(mako ${CHECK_TARGET})
        endif()
    endforeach()
    
    message(STATUS "--- End Mako Borrow Checks Configuration ---")
endif()

# ------------------------------------- build apps -----------------------------------------------

# Custom targets for additional dependencies
# lz4 is handled during configuration, no need for custom target
# txlog_lib is now built directly as a CMake target, no need for custom target


function(add_apps exec_name exec_path)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs TXLOG_LIBS TXLOG_DEPS)
  cmake_parse_arguments(APP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(APP_TXLOG_LIBS)
    set(APP_TXLOG_LINK_LIBS ${APP_TXLOG_LIBS})
  else()
    set(APP_TXLOG_LINK_LIBS ${TXLOG_LINK_LIBS})
  endif()

  if(APP_TXLOG_DEPS)
    set(APP_TXLOG_BUILD_DEPS ${APP_TXLOG_DEPS})
  else()
    set(APP_TXLOG_BUILD_DEPS ${TXLOG_BUILD_DEPS})
  endif()

  # Add executable
  add_executable(${exec_name} ${exec_path})

  # Configure target
  target_include_directories(${exec_name} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
      ${CMAKE_CURRENT_SOURCE_DIR}/src/mako/masstree
      ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/src
      ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/third_party/asio/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran
      ${CMAKE_CURRENT_SOURCE_DIR}/src/rrr
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  target_compile_options(${exec_name} PRIVATE ${BENCH_CXXFLAGS}) # include all flags

  # Link dbtest against mako library and dependencies
  target_link_libraries(${exec_name}
      mako
      erpc
      rust_lib
      ${APP_TXLOG_LINK_LIBS}
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
    add_dependencies(${exec_name} mako erpc rust_lib ${APP_TXLOG_BUILD_DEPS} build_rusty_cpp_checker)
  else()
    add_dependencies(${exec_name} mako erpc rust_lib ${APP_TXLOG_BUILD_DEPS})
  endif()
endfunction()

add_apps(dbtest src/mako/benchmarks/dbtest.cc)
add_apps(simpleTransaction examples/simpleTransaction.cc
    TXLOG_LIBS txlog_core
    TXLOG_DEPS txlog_core
)
add_apps(simpleTransactionRep examples/simpleTransactionRep.cc)
add_apps(continuousTransactions examples/continuousTransactions.cc)
add_apps(simplePaxos examples/simplePaxos.cc)
add_apps(test_rocksdb_persistence examples/test_rocksdb_persistence.cc)
add_apps(rocksdb_replay_app src/mako/benchmarks/rocksdb_replay_app.cc)
add_apps(test_callback_demo examples/test_callback_demo.cc)
add_apps(test_ordered_callbacks examples/test_ordered_callbacks.cc)
add_apps(test_partitioned_queues examples/test_partitioned_queues.cc)
add_apps(test_stress_partitioned_queues examples/test_stress_partitioned_queues.cc)
add_apps(makoCon examples/makoCon.cc)
add_apps(makoConDBBench examples/makoConDBBench.cc)
add_apps(makoConMultiTrd examples/makoConMultiTrd.cc)
# Note: makoServer functionality has been consolidated into simpleTransactionRep.cc
#       Use: ./simpleTransactionRep --server <args> for server-only mode

# Raft test executables (only when MAKO_USE_RAFT is enabled)
if(MAKO_USE_RAFT)
  add_apps(deptran_server src/deptran/s_main.cc)
  add_apps(simpleRaft examples/mako-raft-tests/simpleRaft.cc)
  add_apps(simpleTransactionRepRaft examples/mako-raft-tests/simpleTransactionRepRaft.cc)
  add_apps(testPreferredReplicaStartup examples/mako-raft-tests/testPreferredReplicaStartup.cc)
  add_apps(testPreferredReplicaLogReplication examples/mako-raft-tests/testPreferredReplicaLogReplication.cc)
  add_apps(testNoOps examples/mako-raft-tests/testNoOps.cc)
  add_apps(read_raft_disk src/deptran/raft/read_raft_disk.cc)
endif()

# Borrow checking for dbtest DISABLED - uses STO (Software Transactional Objects)
# headers which have reference-returning functions (back(), key()) that would
# require extensive modifications to be borrow-safe. The core masstree files
# are still borrow-checked via file(GLOB_RECURSE MASSTREE_BORROW_CHECK_SOURCES)
# if(ENABLE_BORROW_CHECKING)
#     add_borrow_check_target(dbtest)
# endif()

# Enable deptran borrow checking by default for dbtest and related apps
if(ENABLE_BORROW_CHECKING AND TARGET borrow_check_deptran)
    add_dependencies(dbtest borrow_check_deptran)
    if(MAKO_USE_RAFT)
        add_dependencies(deptran_server borrow_check_deptran)
    endif()
endif()

add_apps(paxos_async_commit_test src/mako/benchmarks/paxos_async_commit_test.cc)
add_apps(basic src/mako/benchmarks/ut/basic.cc)
add_apps(erpc_client src/mako/benchmarks/ut/erpc_client.cc)
add_apps(erpc_server src/mako/benchmarks/ut/erpc_server.cc)

# RPC benchmark binary
get_filename_component(MAKO_CXX_COMPILER_BASENAME "${CMAKE_CXX_COMPILER}" NAME)
set(RPCBENCH_SRC test/rpcbench.cc)
if(MAKO_CXX_COMPILER_BASENAME STREQUAL "fragilec")
    # Use a fragile-friendly source variant to keep benchmark logic transpilable.
    set(RPCBENCH_SRC test/rpcbench_fragile.cc)
endif()
add_executable(rpcbench ${RPCBENCH_SRC} test/benchmark_service.cc)
target_include_directories(rpcbench PRIVATE
    src
    src/rrr
    test
)
target_compile_options(rpcbench PRIVATE ${BENCH_CXXFLAGS})
target_link_libraries(rpcbench
    rrr
    pthread
    ${BENCH_LDFLAGS}
    ${LIBEVENT_LDFLAGS}
)

# Build the necessary RRR library sources for rpcbench
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
    target_link_libraries(rrr pthread)

    # RRR files for borrow checking (explicit list to control what gets checked)
    # NOTE: Many RRR files are currently excluded from borrow checking due to
    # false positives from recent rusty-cpp updates that enforce stricter
    # uniform borrow rules for pointers and references. The following errors
    # are being triggered across multiple files:
    #   - "Cannot return 'value' because it has been moved"
    #   - "Cannot borrow from 'this': variable is not alive in current scope"
    # These appear to be false positives related to how the checker handles
    # method calls on this and return value tracking in C++ code.
    # Files should be added back as these issues are resolved in rusty-cpp.
    #
    # Excluded files (add back when issues are resolved):
    #   - src/rrr/misc/rand.cpp (no safety annotations)
    #   - src/rrr/misc/recorder.cpp (no safety annotations)
    #   - src/rrr/base/unittest.cpp (no safety annotations)
    #   - Other *.cpp/*.cc files (need safety annotations or have violations)
    set(RRR_BORROW_SRC
        # Reactor files that pass borrow checking
        src/rrr/reactor/fiber_impl.cc
        src/rrr/reactor/event.cc
        src/rrr/reactor/quorum_event.cc
        src/rrr/reactor/epoll_wrapper.cc
        src/rrr/reactor/reactor.cc  # Uses RefCell for safe interior mutability (refactored from mutable)
        # Base files that pass borrow checking
        src/rrr/base/logging.cpp
        src/rrr/base/misc.cpp
        src/rrr/base/basetypes.cpp
        src/rrr/base/debugging.cpp
        src/rrr/base/threading.cpp
        # Misc files that pass borrow checking
        src/rrr/misc/alock.cpp
        src/rrr/misc/marshal.cpp
        # Additional base files
        src/rrr/base/strop.cpp
        src/rrr/base/unittest.cpp
        src/rrr/misc/rand.cpp
        src/rrr/misc/recorder.cpp
        # RPC files that pass borrow checking
        src/rrr/rpc/utils.cpp
        src/rrr/rpc/client.cpp
        src/rrr/rpc/server.cpp
    )

    # Add borrow checking for RRR library (only if we have files to check)
    if(ENABLE_BORROW_CHECKING AND RRR_BORROW_SRC)
        add_library(rrr_borrow OBJECT ${RRR_BORROW_SRC})
        set_target_properties(rrr_borrow PROPERTIES EXCLUDE_FROM_ALL YES)
        target_include_directories(rrr_borrow PUBLIC
            src
            src/rrr
            ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
        )
        target_compile_options(rrr_borrow PUBLIC ${BENCH_CXXFLAGS})

        add_borrow_check_target(rrr_borrow)

        # Create a convenience target to check only RRR library files
        add_custom_target(borrow_check_rrr
            DEPENDS borrow_check_all_rrr_borrow
            COMMENT "Running borrow checker on RRR library files"
        )
    elseif(ENABLE_BORROW_CHECKING)
        # Create an empty target when no files to check
        add_custom_target(borrow_check_rrr
            COMMAND ${CMAKE_COMMAND} -E echo "No RRR files configured for borrow checking"
            COMMENT "No RRR files configured for borrow checking"
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

# Use vendored GoogleTest only (no system gtest dependency).
set(BUILD_TESTS OFF)
set(FRAGILE_GTEST_FROM_SOURCE OFF)
set(MAKO_GTEST_ROOT "")
set(MAKO_GTEST_MAIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/test/fragile_gtest_main.cc)

# Prefer root-level third-party layout. Keep erpc-nested path as fallback.
foreach(_gtest_candidate
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/googletest/googletest
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/third_party/googletest/googletest
)
    if(EXISTS ${_gtest_candidate}/src/gtest-all.cc AND EXISTS ${_gtest_candidate}/include/gtest/gtest.h)
        set(MAKO_GTEST_ROOT ${_gtest_candidate})
        break()
    endif()
endforeach()

if(NOT EXISTS ${MAKO_GTEST_MAIN_SRC})
    message(WARNING "GoogleTest main stub missing at ${MAKO_GTEST_MAIN_SRC}. Tests will be disabled.")
elseif(MAKO_GTEST_ROOT STREQUAL "")
    message(WARNING "Vendored GoogleTest not found under third-party/googletest or erpc fallback. Tests will be disabled.")
else()
    set(MAKO_GTEST_SRC ${MAKO_GTEST_ROOT}/src/gtest-all.cc)
    if(CMAKE_CXX_COMPILER MATCHES "(/|^)fragilec$")
        set(FRAGILE_GTEST_FROM_SOURCE ON)
        set(MAKO_GTEST_LIB_TARGET fragile_transpiled_gtest)
        set(MAKO_GTEST_MAIN_TARGET fragile_transpiled_gtest_main)
    else()
        set(MAKO_GTEST_LIB_TARGET mako_gtest)
        set(MAKO_GTEST_MAIN_TARGET mako_gtest_main)
    endif()

    if(NOT TARGET ${MAKO_GTEST_LIB_TARGET})
        add_library(${MAKO_GTEST_LIB_TARGET} STATIC ${MAKO_GTEST_SRC})
        target_include_directories(${MAKO_GTEST_LIB_TARGET} PUBLIC
            ${MAKO_GTEST_ROOT}/include
            ${MAKO_GTEST_ROOT}
        )
        if(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
            target_include_directories(${MAKO_GTEST_LIB_TARGET} PRIVATE
                ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}
            )
        endif()
        target_compile_options(${MAKO_GTEST_LIB_TARGET} PRIVATE ${BENCH_CXXFLAGS})
        target_compile_definitions(${MAKO_GTEST_LIB_TARGET} PUBLIC GTEST_HAS_PTHREAD=1)
        target_link_libraries(${MAKO_GTEST_LIB_TARGET} PUBLIC pthread)
    endif()

    if(NOT TARGET ${MAKO_GTEST_MAIN_TARGET})
        add_library(${MAKO_GTEST_MAIN_TARGET} STATIC ${MAKO_GTEST_MAIN_SRC})
        target_include_directories(${MAKO_GTEST_MAIN_TARGET} PUBLIC
            ${MAKO_GTEST_ROOT}/include
            ${MAKO_GTEST_ROOT}
        )
        if(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
            target_include_directories(${MAKO_GTEST_MAIN_TARGET} PRIVATE
                ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}
            )
        endif()
        target_compile_options(${MAKO_GTEST_MAIN_TARGET} PRIVATE ${BENCH_CXXFLAGS})
        target_compile_definitions(${MAKO_GTEST_MAIN_TARGET} PUBLIC GTEST_HAS_PTHREAD=1)
        target_link_libraries(${MAKO_GTEST_MAIN_TARGET} PUBLIC ${MAKO_GTEST_LIB_TARGET} pthread)
    endif()

    set(GTEST_LIBRARIES ${MAKO_GTEST_LIB_TARGET})
    set(GTEST_MAIN_LIBRARIES ${MAKO_GTEST_MAIN_TARGET})
    set(GTEST_INCLUDE_DIRS ${MAKO_GTEST_ROOT}/include ${MAKO_GTEST_ROOT})
    set(BUILD_TESTS ON)
    message(STATUS "GoogleTest configured from vendored source: ${MAKO_GTEST_ROOT}")
endif()

if(BUILD_TESTS)
    # Common test include directories
    set(TEST_INCLUDE_DIRS
        src
        src/rrr
        src/memdb
        src/mako
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
    set(RPC_TEST_LINK_LIBS
        rrr
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

    # MakoClientService Unit Tests
    add_executable(test_client_service test/test_client_service.cc)
    target_include_directories(test_client_service PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_client_service PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_client_service ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_client_service COMMAND test_client_service)

    # Config Schema Serialization Tests
    add_executable(test_config_schema test/config_schema_test.cc)
    target_include_directories(test_config_schema PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_config_schema PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_config_schema ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_config_schema COMMAND test_config_schema)

    # Sharding Policy Tests
    add_executable(test_sharding_policy test/sharding_policy_test.cc)
    target_include_directories(test_sharding_policy PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_sharding_policy PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_sharding_policy ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_sharding_policy COMMAND test_sharding_policy)

    # Sharding Policy Cache Tests (requires txlog bench/core for ConfigClient)
    add_executable(test_sharding_policy_cache test/sharding_policy_cache_test.cc)
    target_include_directories(test_sharding_policy_cache PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/deptran)
    target_compile_options(test_sharding_policy_cache PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_sharding_policy_cache ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_sharding_policy_cache COMMAND test_sharding_policy_cache)

    # Shard Router Integration Tests (requires txlog bench/core for ShardingPolicyCache)
    add_executable(test_shard_router test/shard_router_test.cc)
    target_include_directories(test_shard_router PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/deptran src/mako)
    target_compile_options(test_shard_router PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_shard_router ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_shard_router COMMAND test_shard_router)

    # TPC-C Sharding Integration Tests (requires txlog bench/core for ShardingPolicyCache)
    add_executable(test_tpcc_sharding test/tpcc_sharding_test.cc)
    target_include_directories(test_tpcc_sharding PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/deptran src/mako)
    target_compile_options(test_tpcc_sharding PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_tpcc_sharding ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_tpcc_sharding COMMAND test_tpcc_sharding)

    # Fiber API Tests
    add_executable(test_fiber test/fiber_test.cc)
    target_include_directories(test_fiber PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/rrr)
    target_compile_options(test_fiber PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_fiber rrr ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} pthread)
    add_test(NAME test_fiber COMMAND test_fiber)

    # ConfigStore RocksDB Tests
    add_executable(test_config_store test/config_store_test.cc)
    target_include_directories(test_config_store PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_config_store PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_config_store ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_config_store COMMAND test_config_store)

    # ConfigService RPC Tests
    # Note: Includes src/deptran for rcc_rpc.h dependencies (__dep__.h, rcc/tx.h, etc.)
    add_executable(test_config_service test/config_service_test.cc)
    target_include_directories(test_config_service PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/deptran)
    target_compile_options(test_config_service PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_config_service ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_config_service COMMAND test_config_service)

    # ConfigClient Tests
    # Note: Includes src/deptran for rcc_rpc.h dependencies
    add_executable(test_config_client test/config_client_test.cc)
    target_include_directories(test_config_client PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/deptran)
    target_compile_options(test_config_client PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_config_client ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_config_client COMMAND test_config_client)

    # Config Node Failure Scenario Tests
    add_executable(test_config_failure test/config_failure_test.cc)
    target_include_directories(test_config_failure PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/deptran)
    target_compile_options(test_config_failure PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_config_failure ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_config_failure COMMAND test_config_failure)

    # Sharding Policy Startup Tests
    add_executable(test_sharding_startup test/sharding_startup_test.cc)
    target_include_directories(test_sharding_startup PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/deptran src/mako/benchmarks)
    target_compile_options(test_sharding_startup PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_sharding_startup ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_sharding_startup COMMAND test_sharding_startup)

    # RPC Tests
    add_executable(test_rpc test/test_rpc.cc test/benchmark_service.cc)
    target_include_directories(test_rpc PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc PRIVATE ${TEST_COMPILE_OPTIONS})
    # test_rpc only exercises RRR RPC paths and does not require memdb.
    # Keep linkage minimal to avoid pulling unrelated memdb compile surfaces.
    target_link_libraries(test_rpc
        rrr
        pthread
        ${BENCH_LDFLAGS}
        ${LIBEVENT_LDFLAGS}
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
    )
    add_test(NAME test_rpc COMMAND test_rpc)
    if(ENABLE_BORROW_CHECKING AND TARGET borrow_check_rrr)
        # Ensure regular test builds also gate on SRPC borrow-check status.
        add_dependencies(test_rpc borrow_check_rrr)
    endif()

    # Load Balancer Tests
    add_executable(test_load_balancer test/test_load_balancer.cc test/benchmark_service.cc)
    target_include_directories(test_load_balancer PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_load_balancer PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_load_balancer ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_load_balancer COMMAND test_load_balancer)

    # Idempotency Tests
    add_executable(test_idempotency test/test_idempotency.cc)
    target_include_directories(test_idempotency PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_idempotency PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_idempotency ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_idempotency COMMAND test_idempotency)

    # Completion Tracker Tests
    add_executable(test_completion_tracker test/test_completion_tracker.cc)
    target_include_directories(test_completion_tracker PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_completion_tracker PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_completion_tracker ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_completion_tracker COMMAND test_completion_tracker)

    # Chaos Engineering Tests
    add_executable(rpc_chaos_test test/rpc_chaos_test.cc test/benchmark_service.cc)
    target_include_directories(rpc_chaos_test PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(rpc_chaos_test PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(rpc_chaos_test ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME rpc_chaos_test COMMAND rpc_chaos_test)
    set_tests_properties(rpc_chaos_test PROPERTIES LABELS "chaos")

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

    # Masstree internals tests (threadinfo, globalepoch, RCU)
    add_executable(test_masstree_internals test/test_masstree_internals.cc)
    target_include_directories(test_masstree_internals PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_masstree_internals PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_masstree_internals mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_masstree_internals COMMAND test_masstree_internals)

    # Masstree multi-instance stress tests (MasstreeContext isolation)
    add_executable(test_masstree_multi_instance test/test_masstree_multi_instance.cc)
    target_include_directories(test_masstree_multi_instance PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_masstree_multi_instance PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_masstree_multi_instance mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_masstree_multi_instance COMMAND test_masstree_multi_instance)

    # SiloRuntime tests (per-site runtime isolation)
    add_executable(test_silo_runtime test/test_silo_runtime.cc)
    target_include_directories(test_silo_runtime PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_silo_runtime PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_silo_runtime mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_silo_runtime COMMAND test_silo_runtime)

    # Multi-site Silo stress tests
    add_executable(test_silo_multi_site_stress test/test_silo_multi_site_stress.cc)
    target_include_directories(test_silo_multi_site_stress PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_silo_multi_site_stress PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_silo_multi_site_stress mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_silo_multi_site_stress COMMAND test_silo_multi_site_stress)

    # Borrow checking for Silo tests
    # Note: compare_key and increment functions now have correct @unsafe annotations.
    # Full borrow checking is disabled because other Masstree code (nodeversion.hh,
    # string.hh) has violations that require broader refactoring.
    # if(ENABLE_BORROW_CHECKING)
    #     add_borrow_check_target(test_silo_runtime)
    #     add_borrow_check_target(test_silo_multi_site_stress)
    # endif()

    # Masstree performance benchmark (on-demand throughput output)
    set(MASSTREE_PERF_SRC test/masstree_perf.cc)
    if(MAKO_CXX_COMPILER_BASENAME STREQUAL "fragilec")
        # Use a fragile-friendly source variant to keep benchmark logic transpilable.
        set(MASSTREE_PERF_SRC test/masstree_perf_fragile.cc)
    endif()
    add_executable(masstree_perf ${MASSTREE_PERF_SRC})
    target_include_directories(masstree_perf PRIVATE ${TEST_INCLUDE_DIRS})
    target_compile_options(masstree_perf PRIVATE ${TEST_COMPILE_OPTIONS})
    if(MAKO_CXX_COMPILER_BASENAME STREQUAL "fragilec")
        # Keep strict fragile benchmark builds focused on Masstree-only code paths.
        # Linking the full `mako` library pulls in unrelated RPC translation units
        # that are not required for this benchmark and currently block compilation.
        target_link_libraries(masstree_perf pthread)
    else()
        target_link_libraries(masstree_perf mako ${TEST_LINK_LIBS})
    endif()

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
    target_link_libraries(test_rpc_extended ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
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
        ${RPC_TEST_LINK_LIBS}
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

    # ============================================================================
    # RPC Reliability Unit Tests (Phase 7.1)
    # ============================================================================

    # Connection State Machine Tests
    add_executable(test_rpc_connection_state test/rpc_connection_state_test.cc)
    target_include_directories(test_rpc_connection_state PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_connection_state PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_connection_state ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_connection_state COMMAND test_rpc_connection_state)

    # Reconnection Policy Tests
    add_executable(test_rpc_reconnect_policy test/rpc_reconnect_policy_test.cc)
    target_include_directories(test_rpc_reconnect_policy PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_reconnect_policy PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_reconnect_policy ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_reconnect_policy COMMAND test_rpc_reconnect_policy)

    # Circuit Breaker Tests
    add_executable(test_rpc_circuit_breaker test/rpc_circuit_breaker_test.cc)
    target_include_directories(test_rpc_circuit_breaker PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_circuit_breaker PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_circuit_breaker ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_circuit_breaker COMMAND test_rpc_circuit_breaker)

    # Heartbeat Tests
    add_executable(test_rpc_heartbeat test/rpc_heartbeat_test.cc)
    target_include_directories(test_rpc_heartbeat PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_heartbeat PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_heartbeat ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_heartbeat COMMAND test_rpc_heartbeat)

    # Error Handling Tests
    add_executable(test_rpc_errors test/rpc_errors_test.cc)
    target_include_directories(test_rpc_errors PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_errors PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_errors ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_errors COMMAND test_rpc_errors)

    # SRPC Book API Symbol Drift Guard
    add_executable(test_rpc_docs_symbols test/rpc_docs_symbols_test.cc)
    target_include_directories(test_rpc_docs_symbols PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_docs_symbols PRIVATE ${TEST_COMPILE_OPTIONS})
    target_compile_definitions(test_rpc_docs_symbols PRIVATE
        SRPC_BOOK_PATH="${CMAKE_SOURCE_DIR}/docs/srpc-book.md"
        SRPC_MIGRATION_GUIDE_PATH="${CMAKE_SOURCE_DIR}/docs/rpc/migration-guide.md"
    )
    target_link_libraries(test_rpc_docs_symbols ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_docs_symbols COMMAND test_rpc_docs_symbols)

    find_program(PYTHON3_EXECUTABLE python3 REQUIRED)
    add_test(
        NAME test_rpc_docs_snippet_compile
        COMMAND ${PYTHON3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/test/rpc_docs_snippet_compile_test.py
                --book ${CMAKE_SOURCE_DIR}/docs/srpc-book.md
                --repo ${CMAKE_SOURCE_DIR}
                --cxx ${CMAKE_CXX_COMPILER}
                --min-snippets 9
    )
    add_test(
        NAME test_rpc_rpcgen_typed_structs
        COMMAND ${PYTHON3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/test/rpcgen_typed_structs_test.py
                --repo ${CMAKE_SOURCE_DIR}
    )

    # Callback System Tests (Phase 6.2)
    add_executable(test_rpc_callbacks test/rpc_callbacks_test.cc)
    target_include_directories(test_rpc_callbacks PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_callbacks PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_callbacks ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_callbacks COMMAND test_rpc_callbacks)

    # Request Queue Tests (Phase 2.1)
    add_executable(test_rpc_request_queue test/rpc_request_queue_test.cc)
    target_include_directories(test_rpc_request_queue PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_request_queue PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_request_queue ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_request_queue COMMAND test_rpc_request_queue)

    add_executable(test_rpc_request_buffering test/rpc_request_buffering_test.cc)
    target_include_directories(test_rpc_request_buffering PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_request_buffering PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_request_buffering ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_request_buffering COMMAND test_rpc_request_buffering)

    # Timeout and Retry Tests (Phase 2.4)
    add_executable(test_rpc_timeout_retry test/rpc_timeout_retry_test.cc)
    target_include_directories(test_rpc_timeout_retry PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_timeout_retry PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_timeout_retry ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_timeout_retry COMMAND test_rpc_timeout_retry)

    # ============================================================================
    # RPC Reliability Integration Tests (Phase 7.2)
    # ============================================================================

    # State Machine Integration Tests
    add_executable(test_rpc_state_integration test/rpc_state_integration_test.cc test/benchmark_service.cc)
    target_include_directories(test_rpc_state_integration PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_state_integration PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_state_integration ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_state_integration COMMAND test_rpc_state_integration)

    # Reconnection Integration Tests
    add_executable(test_rpc_reconnect_integration test/rpc_reconnect_integration_test.cc test/benchmark_service.cc)
    target_include_directories(test_rpc_reconnect_integration PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_reconnect_integration PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_reconnect_integration ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_reconnect_integration COMMAND test_rpc_reconnect_integration)

    # Circuit Breaker Integration Tests
    add_executable(test_rpc_circuit_breaker_integration test/rpc_circuit_breaker_integration_test.cc test/benchmark_service.cc)
    target_include_directories(test_rpc_circuit_breaker_integration PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_circuit_breaker_integration PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_circuit_breaker_integration ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_circuit_breaker_integration COMMAND test_rpc_circuit_breaker_integration)

    # Error Integration Tests
    add_executable(test_rpc_error_integration test/rpc_error_integration_test.cc test/benchmark_service.cc)
    target_include_directories(test_rpc_error_integration PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_error_integration PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_error_integration ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_error_integration COMMAND test_rpc_error_integration)

    # Combined Reliability Tests
    add_executable(test_rpc_combined_reliability test/rpc_combined_reliability_test.cc test/benchmark_service.cc)
    target_include_directories(test_rpc_combined_reliability PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_combined_reliability PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_combined_reliability ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_combined_reliability COMMAND test_rpc_combined_reliability)

    # Graceful Shutdown Tests (Phase 4.1)
    add_executable(test_rpc_graceful_shutdown test/rpc_graceful_shutdown_test.cc)
    target_include_directories(test_rpc_graceful_shutdown PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_graceful_shutdown PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_graceful_shutdown ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_graceful_shutdown COMMAND test_rpc_graceful_shutdown)

    # Server Restart Detection Tests (Phase 4.2)
    add_executable(test_rpc_restart_detection test/rpc_restart_detection_test.cc)
    target_include_directories(test_rpc_restart_detection PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_restart_detection PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_restart_detection ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_restart_detection COMMAND test_rpc_restart_detection)

    # Connection Validation Tests (Phase 3.3)
    add_executable(test_rpc_validation test/rpc_validation_test.cc test/benchmark_service.cc)
    target_include_directories(test_rpc_validation PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_validation PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_validation ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_validation COMMAND test_rpc_validation)

    # Connection Health Metrics Tests (Phase 3.2)
    add_executable(test_rpc_metrics test/rpc_metrics_test.cc test/benchmark_service.cc)
    target_include_directories(test_rpc_metrics PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_metrics PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_metrics ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_metrics COMMAND test_rpc_metrics)

    # ClientPool Health-Aware Tests (Phase 5.1)
    add_executable(test_rpc_client_pool test/rpc_client_pool_test.cc test/benchmark_service.cc)
    target_include_directories(test_rpc_client_pool PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_client_pool PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_client_pool ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_client_pool COMMAND test_rpc_client_pool)

    # Log Storage Tests (Phase 1.1)
    add_executable(test_rpc_log_storage test/rpc_log_storage_test.cc)
    target_include_directories(test_rpc_log_storage PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_log_storage PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_log_storage ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_log_storage COMMAND test_rpc_log_storage)

    # RocksDB Log Storage Tests (Phase 1.2)
    add_executable(test_rpc_rocksdb_log_storage test/rpc_rocksdb_log_storage_test.cc)
    target_include_directories(test_rpc_rocksdb_log_storage PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_rocksdb_log_storage PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_rocksdb_log_storage ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_rpc_rocksdb_log_storage COMMAND test_rpc_rocksdb_log_storage)

    # Transaction Timeout Tests
    add_executable(test_txn_timeout test/deptran/txn_timeout_test.cc)
    target_include_directories(test_txn_timeout PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_txn_timeout PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_txn_timeout ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_txn_timeout COMMAND test_txn_timeout)

    # Stress Crash Recovery Tests (Phase 7.3.1)
    add_executable(test_rpc_stress_crash test/rpc_stress_crash_test.cc test/benchmark_service.cc)
    target_include_directories(test_rpc_stress_crash PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_stress_crash PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_stress_crash ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    # NOTE: Stress tests take too long for regular CI (10+ minutes each).
    # They are labeled "stress" and excluded from ctest by default.
    # Run manually with: ctest -L stress --timeout 1800
    add_test(NAME test_rpc_stress_crash COMMAND test_rpc_stress_crash)
    set_tests_properties(test_rpc_stress_crash PROPERTIES
        TIMEOUT 1800
        LABELS "stress"
    )

    # Network Partition Simulation Tests (Phase 7.3.2)
    add_executable(test_rpc_partition test/rpc_partition_test.cc test/benchmark_service.cc)
    target_include_directories(test_rpc_partition PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc_partition PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_rpc_partition ${RPC_TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc_partition COMMAND test_rpc_partition)
    set_tests_properties(test_rpc_partition PROPERTIES
        TIMEOUT 600
        LABELS "stress"
    )

    # Integration tests from CI
    add_test(NAME simpleTransaction COMMAND simpleTransaction)
    add_test(NAME test_rpc_replication_config_generation
        COMMAND bash -c "bash ./test/rpc_replication_config_generation_test.sh"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_rpc_replication_config_generation PROPERTIES TIMEOUT 30)

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
        DEPENDS test_marshal test_rpc test_future test_masstree test_reactor test_rpc_extended test_reactor_extended 
                test_silo_varint test_silo_rcu_thread test_silo_allocator_tuple 
                test_sto_transaction test_sto_transaction_real
                rpcbench simpleTransaction simplePaxos shard1ReplicationSimple  # test_alock removed
        COMMENT "Running all tests (RRR + Silo/STO)"
    )
    if(ENABLE_BORROW_CHECKING AND TARGET borrow_check_rrr)
        add_dependencies(run_tests borrow_check_rrr)
    endif()

    # Add borrow checking for test files if enabled
    # NOTE: Test borrow checking is currently disabled due to false positives
    # from recent rusty-cpp updates. The same issues affecting RRR files also
    # affect test files:
    #   - "Cannot return 'value' because it has been moved"
    #   - "Cannot borrow from 'this': variable is not alive in current scope"
    # Re-enable when these issues are resolved in rusty-cpp.
    if(ENABLE_BORROW_CHECKING)
        # Disabled due to false positives:
        # add_borrow_check_target(test_marshal)
        # add_borrow_check_target(test_rpc)
        # add_borrow_check_target(test_future)
        # add_borrow_check_target(test_reactor)
        # Create an empty convenience target
        add_custom_target(borrow_check_tests
            COMMAND ${CMAKE_COMMAND} -E echo "Test borrow checking disabled - see CMakeLists.txt comments"
            COMMENT "Test borrow checking disabled pending rusty-cpp fixes"
        )
    endif()

    # ============================================================================
    # SILO/STO TESTS (merged from tests/ directory)
    # ============================================================================

    # Test: Silo Varint (Utility)
    add_executable(test_silo_varint test/test_silo_varint.cc)
    target_link_libraries(test_silo_varint
        mako
        rrr
        erpc
        ${TXLOG_LINK_LIBS}
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(test_silo_varint PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME SiloVarintTests COMMAND test_silo_varint)

    # Test: Silo RCU + Thread
    add_executable(test_silo_rcu_thread test/test_silo_rcu_thread.cc)
    target_link_libraries(test_silo_rcu_thread
        mako
        rrr
        erpc
        ${TXLOG_LINK_LIBS}
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(test_silo_rcu_thread PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME SiloRCUThreadTests COMMAND test_silo_rcu_thread)

    # Test: Silo Allocator + Tuple Integration (Real System)
    add_executable(test_silo_allocator_tuple test/test_silo_allocator_tuple.cc)
    target_link_libraries(test_silo_allocator_tuple
        mako
        rrr
        erpc
        ${TXLOG_LINK_LIBS}
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(test_silo_allocator_tuple PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME SiloAllocatorTupleTests COMMAND test_silo_allocator_tuple)

    # Test: STO Transaction (TRcu)
    add_executable(test_sto_transaction test/test_sto_transaction.cc)
    target_link_libraries(test_sto_transaction
        mako
        rrr
        erpc
        ${TXLOG_LINK_LIBS}
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(test_sto_transaction PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME STOTransactionTests COMMAND test_sto_transaction)

    # Test: STO Transaction Real (Integration)
    add_executable(test_sto_transaction_real test/test_sto_transaction_real.cc)
    target_link_libraries(test_sto_transaction_real
        mako
        rrr
        erpc
        ${TXLOG_LINK_LIBS}
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(test_sto_transaction_real PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME STOTransactionRealTests COMMAND test_sto_transaction_real)

    # Custom target to run all SILO/STO tests
    add_custom_target(run_silo_sto_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "Silo|STO"
        DEPENDS test_silo_varint test_silo_rcu_thread test_silo_allocator_tuple 
                test_sto_transaction test_sto_transaction_real
        COMMENT "Running all Silo/STO tests"
    )

    message(STATUS "Test targets configured:")
    message(STATUS "  RRR Framework Tests:")
    message(STATUS "    - test_marshal: Marshal/Unmarshal tests")
    message(STATUS "    - test_rpc: RPC client/server tests")
    message(STATUS "    - test_rpc_extended: Extended RPC tests (timeouts, reconnection, load)")
    message(STATUS "    - test_future: Future/async operation tests")
    message(STATUS "    - test_masstree: Core Masstree b-tree regression tests")
    message(STATUS "    - test_reactor: Reactor/event system tests")
    message(STATUS "    - test_reactor_extended: Extended reactor tests (events, coroutines, load)")
    message(STATUS "    - rpcbench: RPC performance benchmark test")
    message(STATUS "  Silo/STO Tests:")
    message(STATUS "    - test_silo_varint: Varint encoding/decoding (22 tests)")
    message(STATUS "    - test_silo_rcu_thread: RCU memory management (9 tests)")
    message(STATUS "    - test_silo_allocator_tuple: Allocator + Tuple integration (18 tests)")
    message(STATUS "    - test_sto_transaction: STO Transaction RCU (13 tests)")
    message(STATUS "    - test_sto_transaction_real: STO Transaction integration tests")
    message(STATUS "Additional perf binaries:")
    message(STATUS "  - masstree_perf: On-demand Masstree throughput benchmark (JSON output)")
    message(STATUS "")
    message(STATUS "Run all tests with: make run_tests")
    message(STATUS "Run Silo/STO tests: make run_silo_sto_tests")
    message(STATUS "Or individually: make test && ctest")
endif()
