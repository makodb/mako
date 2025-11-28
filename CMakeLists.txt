cmake_minimum_required(VERSION 3.10)
set(CMAKE_C_COMPILER "gcc" CACHE STRING "C compiler")
set(CMAKE_CXX_COMPILER "g++" CACHE STRING "C++ compiler")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(mako)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.0)
        message(FATAL_ERROR "GCC version 12 or higher is required.")
    endif()
else()
    message(WARNING "You are not using GCC.")
endif()

# Define paths
set(mkfile_path "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
set(HOME "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ERPC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc")
message(STATUS "I: ${ERPC_PATH}")
set(W "src/mako")
set(TOP "${CMAKE_SOURCE_DIR}")

# Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)
find_package(Python3 COMPONENTS Development REQUIRED)
find_package(Boost REQUIRED COMPONENTS system filesystem thread coroutine context)

# Options
option(DEBUG "Enable debug mode" OFF)
option(CHECK_INVARIANTS "Enable invariant checking" OFF)
option(COCO "Enable COCO" OFF)
set(USE_MALLOC_MODE 1 CACHE STRING "Malloc mode: 0=libc, 1=jemalloc, 2=tcmalloc, 3=flow")
set(MODE "perf" CACHE STRING "Build mode")
option(STO_RMW "Enable STO read-modify-write" OFF)
option(HASHTABLE "Enable hashtable" OFF)
option(GPROF "Enable profiling with gprof" OFF)
option(OPACITY "Enable opacity" OFF)
option(DISABLE_MULTI_VERSION "Disable multi-version" OFF)
option(MEGA_BENCHMARK "Enable MEGA benchmark" OFF)
option(MEGA_BENCHMARK_MICRO "Enable MEGA benchmark micro" OFF)
option(TRACKING_LATENCY "Enable latency tracking" OFF)
option(DISABLE_DISK "Disable disk persistence" ON)
option(FAIL_NEW_VERSION "Enable fail new version" ON)
option(SIMULATE_ONE_SHARD_PER_THREAD "Simulate one shard per thread" OFF)
option(TRACKING_ROLLBACK "Enable rollback tracking" OFF)

# Config Selection
set(OSUFFIX "")
if(CHECK_INVARIANTS)
  set(OSUFFIX ".check")
endif()

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

# -----------------------------------------------------------------------------
# RUSTY-CPP CONFIGURATION (Reference Pattern)
# -----------------------------------------------------------------------------
set(RUSTYCPP_BUILD_TYPE "release")
include(${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/cmake/RustyCppSubmodule.cmake)

set(RUSTYCPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp")
set(CPP_BORROW_CHECKER "${RUSTYCPP_DIR}/target/${RUSTYCPP_BUILD_TYPE}/rusty-cpp-checker")

if(NOT EXISTS ${CPP_BORROW_CHECKER})
    message(STATUS "Building rusty-cpp-checker binary...")
    execute_process(
        COMMAND cargo build --${RUSTYCPP_BUILD_TYPE}
        WORKING_DIRECTORY ${RUSTYCPP_DIR}
        RESULT_VARIABLE BUILD_RESULT
    )
endif()

set(ENABLE_BORROW_CHECKING ON CACHE BOOL "Enable RustyCpp borrow checking" FORCE)

if(ENABLE_BORROW_CHECKING)
    # Define include paths for the checker
    set(BORROW_CHECKER_INCLUDE_PATHS
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mako/masstree
        ${CMAKE_CURRENT_SOURCE_DIR}/src/rrr
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/third_party/asio/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
    )
    if(Boost_INCLUDE_DIRS)
        list(APPEND BORROW_CHECKER_INCLUDE_PATHS ${Boost_INCLUDE_DIRS})
    endif()

    # Define definitions globally for the checker
    add_compile_definitions(CACHE_LINE_SIZE=64)
    add_compile_definitions(STO_OPACITY=${OPACITY})
    add_compile_definitions(READ_MY_WRITES=${STO_RMW})
    add_compile_definitions(HASHTABLE=${HASHTABLE})
    add_compile_definitions(CONFIG_H="${CONFIG_H}")
    add_compile_definitions(MASSTREE_CONFIG_H="${CMAKE_CURRENT_SOURCE_DIR}/${W}/masstree/config.h")

    enable_borrow_checking()
endif()

# -----------------------------------------------------------------------------
# RUST LIB
# -----------------------------------------------------------------------------
set(RUST_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/target/release")
set(RUST_LIB_NAME "rust_redis")
set(RUST_LIB_PATH "${RUST_LIB_DIR}/lib${RUST_LIB_NAME}.a")

add_custom_command(
    OUTPUT ${RUST_LIB_PATH}
    COMMAND cargo build --release
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/src/lib.rs
    COMMENT "Building Rust library if needed"
)
add_custom_target(rust_build ALL DEPENDS ${RUST_LIB_PATH})
add_library(rust_lib STATIC IMPORTED)
set_target_properties(rust_lib PROPERTIES IMPORTED_LOCATION "${RUST_LIB_PATH}")
add_dependencies(rust_lib rust_build)

# -----------------------------------------------------------------------------
# ENVIRONMENT
# -----------------------------------------------------------------------------
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

if(env STREQUAL "ib")
  set(TRANSPORT "infiniband" CACHE STRING "eRPC transport type")
  set(ROCE ON CACHE BOOL "eRPC RoCE setting")
  set(PERF ON CACHE BOOL "eRPC performance setting")
  set(LOG_LEVEL "none" CACHE STRING "eRPC log level")
elseif(env STREQUAL "dpdk")
  set(TRANSPORT "dpdk" CACHE STRING "eRPC transport type")
  set(AZURE ON CACHE BOOL "eRPC Azure setting")
  set(PERF ON CACHE BOOL "eRPC performance setting")
  set(LOG_LEVEL "none" CACHE STRING "eRPC log level")
else()
  set(TRANSPORT "fake" CACHE STRING "eRPC transport type")
  set(ROCE OFF CACHE BOOL "eRPC RoCE setting")
  set(PERF ON CACHE BOOL "eRPC performance setting")
  set(LOG_LEVEL "none" CACHE STRING "eRPC log level")
endif()

add_subdirectory(third-party/erpc)
set_target_properties(erpc PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/third-party/erpc")

set(ERPC_CFLAGS_IB -Isrc/mako -I${ERPC_PATH}/src -DERPC_INFINIBAND=true -march=native -DERPC_LOG_LEVEL=6 -DERPC_TESTING=false -DGFLAGS_IS_A_DLL=0)
set(ERPC_LDFLAGS_IB -Wl,--whole-archive ${DPDK_LDFLAGS} -Wl,--no-whole-archive -lpthread -lnuma -ldl -lgflags -libverbs -lmlx4 -lmlx5)
set(ERPC_CFLAGS_DPDK -Isrc/mako -I${ERPC_PATH}/src -DERPC_DPDK=true -march=native -DERPC_LOG_LEVEL=6 -DERPC_TESTING=false -DGFLAGS_IS_A_DLL=0)
set(ERPC_LDFLAGS_DPDK -Wl,--whole-archive ${DPDK_LDFLAGS} -Wl,--no-whole-archive -lpthread -lnuma -ldl -lgflags -libverbs -lmlx4 -lmlx5)
set(ERPC_CFLAGS_ETH -Isrc/mako -I${ERPC_PATH}/src -DERPC_FAKE=true -march=native -DERPC_LOG_LEVEL=6 -DERPC_TESTING=false -DGFLAGS_IS_A_DLL=0)
set(ERPC_LDFLAGS_ETH -Wl,--whole-archive ${DPDK_LDFLAGS} -Wl,--no-whole-archive -lpthread -lnuma -ldl -lgflags)

# Masstree Config
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

# Compiler Flags
set(CXXFLAGS -w -Wreturn-type -Isrc/mako -MD -MP -I${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4 -Isrc -I${HOME} -DCONFIG_H=\"${CONFIG_H}\")

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
list(APPEND CXXFLAGS -include ${W}/masstree/config.h -DREAD_MY_WRITES=${STO_RMW} -DHASHTABLE=${HASHTABLE} -DSTO_OPACITY=${OPACITY})

if(DEFINED LESSER_OPACITY)
  list(APPEND CXXFLAGS -DLESSER_OPACITY=${LESSER_OPACITY})
endif()
if(DEFINED GV7_OPACITY)
  list(APPEND CXXFLAGS -DGV7_OPACITY)
endif()
if(DEFINED ABORT_ON_LOCKED)
  list(APPEND CXXFLAGS -DSTO_ABORT_ON_LOCKED=${ABORT_ON_LOCKED})
endif()

set(ASIO_PATH "${ERPC_PATH}/third_party/asio")
set(ASIO_CFLAGS -I${ASIO_PATH}/include)
list(APPEND CXXFLAGS ${ASIO_CFLAGS} ${LIBEVENT_CFLAGS})

set(LDFLAGS -lpthread -lnuma -lrt)
if(GPROF)
  list(APPEND LDFLAGS -pg -static-libstdc++ -static-libgcc)
endif()
set(LZ4LDFLAGS -L${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4 -llz4 -Wl,-rpath,${TOP}/third-party/lz4)

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

list(APPEND LDFLAGS -levent_pthreads -pthread -lboost_fiber -lboost_context -lboost_system -lboost_thread)

pkg_check_modules(LIBEVENT REQUIRED libevent)
pkg_check_modules(GFLAGS REQUIRED gflags)
pkg_check_modules(PROTOBUF REQUIRED protobuf)
pkg_check_modules(JEMALLOC REQUIRED jemalloc)
list(APPEND CFLAGS ${LIBEVENT_CFLAGS})
list(APPEND LDFLAGS ${LIBEVENT_LDFLAGS})
list(APPEND CXXFLAGS ${ASIO_CFLAGS})

# -----------------------------------------------------------------------------
# TARGETS
# -----------------------------------------------------------------------------
set(BENCH_CXXFLAGS ${CXXFLAGS})
set(BENCH_LDFLAGS ${LDFLAGS} -lz -lrt -lcrypt -laio -ldl -lssl -lcrypto)

# Masstree
separate_arguments(MASSTREE_CONFIG_ARGS NATIVE_COMMAND "${MASSTREE_CONFIG}")
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${W}/masstree/configure")
  execute_process(COMMAND autoreconf -i WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${W}/masstree")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${W}/masstree/config.h")
  execute_process(COMMAND ./configure ${MASSTREE_CONFIG_ARGS} WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${W}/masstree")
endif()

# RPCGen
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.h ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.py
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/bin/rpcgen --cpp --python src/deptran/rcc_rpc.rpc
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.rpc
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
add_custom_target(rcc_rpc_gen DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.h)

# LZ4
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so")
  execute_process(COMMAND make -C "${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4" library)
endif()

# TXLOG
file(GLOB RRR_SRC "src/rrr/base/*.hpp" "src/rrr/base/*.cpp" "src/rrr/misc/*.hpp" "src/rrr/misc/*.cpp" "src/rrr/reactor/*.h" "src/rrr/reactor/*.cc" "src/rrr/pylib/simplerpc/_pyrpc.cpp" "src/rrr/rpc/*.cpp" "src/rrr/rpc/*.hpp" "src/rrr/utils/*.h" "src/rrr/rrr.h" "src/rrr/rrr.hpp")
file(GLOB MEMDB_SRC "src/memdb/*.h" "src/memdb/*.cc")
file(GLOB_RECURSE DEPTRAN_SRC "src/deptran/*.cc" "src/deptran/*/*.cc" "src/bench/*/*.cc")
list(FILTER DEPTRAN_SRC EXCLUDE REGEX "src/deptran/s_main.cc")
list(FILTER DEPTRAN_SRC EXCLUDE REGEX "src/deptran/paxos_main_helper.cc")
list(FILTER DEPTRAN_SRC EXCLUDE REGEX "src/bench/paxos_lib/.*")

add_library(txlog SHARED src/deptran/paxos_main_helper.cc ${DEPTRAN_SRC} ${RRR_SRC} ${MEMDB_SRC})
add_dependencies(txlog rcc_rpc_gen)
target_include_directories(txlog PRIVATE src src/rrr src/memdb src/deptran src/bench ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include dependencies/yaml-cpp/include ${Boost_INCLUDE_DIRS} ${Python3_INCLUDE_DIRS})
target_compile_options(txlog PRIVATE ${BENCH_CXXFLAGS})
target_compile_definitions(txlog PRIVATE CONFIG_H=\"${CONFIG_H}\")
target_link_libraries(txlog yaml-cpp Boost::system Boost::filesystem Boost::thread Boost::coroutine Boost::context pthread ${CMAKE_DL_LIBS} Python3::Python)

add_custom_command(
  OUTPUT ${O}/${W}/buildstamp ${O}/${W}/buildstamp.bench ${O}/${W}/buildstamp.masstree
  COMMAND mkdir -p "${O}/${W}"
  COMMAND echo > "${O}/${W}/buildstamp"
  COMMAND echo > "${O}/${W}/buildstamp.bench"
  COMMAND echo > "${O}/${W}/buildstamp.masstree"
)

if(NOT EXISTS "${W}/masstree/GNUmakefile.in")
  execute_process(COMMAND git submodule init WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  execute_process(COMMAND git submodule update WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(NOT "${MASSTREE_CONFIG}" STREQUAL "${DEP_MASSTREE_CONFIG}")
  execute_process(COMMAND mkdir -p "${O}/${W}" COMMAND echo > "${O}/${W}/buildstamp.masstree")
  set(DEP_MASSTREE_CONFIG "${MASSTREE_CONFIG}" CACHE INTERNAL "Stored MASSTREE_CONFIG")
endif()

# MAKO Library
set(MAKO_SRCFILES
  "${W}/allocator.cc" "${W}/btree.cc" "${W}/core.cc" "${W}/counter.cc" "${W}/memory.cc"
  "${W}/rocksdb_persistence.cc" "${W}/rcu.cc" "${W}/stats_server.cc" "${W}/thread.cc"
  "${W}/ticker.cc" "${W}/tuple.cc" "${W}/txn_btree.cc" "${W}/txn.cc" "${W}/txn_proto2_impl.cc"
  "${W}/varint.cc" "${W}/masstree/compiler.cc" "${W}/masstree/str.cc" "${W}/masstree/string.cc"
  "${W}/masstree/straccum.cc" "${W}/masstree/json.cc" "${W}/masstree/kvthread.cc"
  "${W}/benchmarks/bench.cc" "${W}/benchmarks/encstress.cc" "${W}/benchmarks/bid.cc"
  "${W}/benchmarks/queue.cc" "${W}/benchmarks/tpcc.cc" "${W}/benchmarks/rpc_setup.cc"
  "${W}/benchmarks/sync_util_init.cc"
  "${W}/benchmarks/sto/Transaction.cc" "${W}/benchmarks/sto/MassTrans.cc"
  "${W}/benchmarks/sto/common.cc" "${W}/benchmarks/sto/TRcu.cc"
  "${W}/benchmarks/sto/Packer.cc" "${W}/benchmarks/sto/ReplayDB.cc"
  "${W}/benchmarks/sto/ThreadPool.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/lookup3.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/kv_store.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/rust_wrapper.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/message.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/memory.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/helper_queue.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/transport.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/fasttransport.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/configuration.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/timestamp.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/promise.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/client.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/shardClient.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/server.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/benchmarks/abstract_db.cc"
)

add_library(mako STATIC ${MAKO_SRCFILES})
target_include_directories(mako PUBLIC
    "${W}/masstree"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mako"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${HOME}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include"
)
target_compile_options(mako PUBLIC ${BENCH_CXXFLAGS})
target_compile_definitions(mako PUBLIC CONFIG_H=\"${CONFIG_H}\")
target_include_directories(mako PUBLIC ${LIBEVENT_INCLUDE_DIRS})
set_target_properties(mako PROPERTIES OUTPUT_NAME "mako")

# -----------------------------------------------------------------------------
# BORROW CHECK REGISTRATION
# -----------------------------------------------------------------------------
if(ENABLE_BORROW_CHECKING)
    # STO Files
    add_borrow_check(${W}/benchmarks/sto/Transaction.cc)
    add_borrow_check(${W}/benchmarks/sto/TRcu.cc)
    add_borrow_check(${W}/benchmarks/sto/MassTrans.cc)
    # Silo Files
    add_borrow_check(${W}/allocator.cc)
    add_borrow_check(${W}/tuple.cc)
    add_borrow_check(${W}/thread.cc)
    add_borrow_check(${W}/varint.cc)
    
    # DBTest
    add_borrow_check(${W}/benchmarks/dbtest.cc)
endif()

# -----------------------------------------------------------------------------
# APPS
# -----------------------------------------------------------------------------
function(add_apps exec_name exec_path)
  add_executable(${exec_name} ${exec_path})
  target_include_directories(${exec_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/mako ${CMAKE_CURRENT_SOURCE_DIR}/src/mako/masstree ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/src ${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc/third_party/asio/include ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(${exec_name} PRIVATE ${BENCH_CXXFLAGS})
  target_link_libraries(${exec_name} mako erpc rust_lib txlog ${BENCH_LDFLAGS} yaml-cpp pthread numa event_pthreads rocksdb ${LIBEVENT_LDFLAGS} ${GFLAGS_LDFLAGS} ${PROTOBUF_LDFLAGS} ${JEMALLOC_LDFLAGS})
  add_dependencies(${exec_name} mako erpc rust_lib txlog)
endfunction()

add_apps(dbtest src/mako/benchmarks/dbtest.cc)
add_apps(simpleTransaction examples/simpleTransaction.cc)
add_apps(simpleTransactionRep examples/simpleTransactionRep.cc)
add_apps(simplePaxos examples/simplePaxos.cc)
add_apps(test_rocksdb_persistence examples/test_rocksdb_persistence.cc)
add_apps(rocksdb_replay_app src/mako/benchmarks/rocksdb_replay_app.cc)
add_apps(test_callback_demo examples/test_callback_demo.cc)
add_apps(test_ordered_callbacks examples/test_ordered_callbacks.cc)
add_apps(test_partitioned_queues examples/test_partitioned_queues.cc)
add_apps(test_stress_partitioned_queues examples/test_stress_partitioned_queues.cc)
add_apps(paxos_async_commit_test src/mako/benchmarks/paxos_async_commit_test.cc)
add_apps(basic src/mako/benchmarks/ut/basic.cc)
add_apps(erpc_client src/mako/benchmarks/ut/erpc_client.cc)
add_apps(erpc_server src/mako/benchmarks/ut/erpc_server.cc)

add_executable(rpcbench test/rpcbench.cc test/benchmark_service.cc)
target_include_directories(rpcbench PRIVATE src src/rrr test)
target_compile_options(rpcbench PRIVATE ${BENCH_CXXFLAGS})
target_link_libraries(rpcbench rrr memdb pthread ${BENCH_LDFLAGS} ${LIBEVENT_LDFLAGS})

file(GLOB RRR_BENCH_SRC "src/rrr/base/*.cpp" "src/rrr/misc/*.cpp" "src/rrr/rpc/*.cpp" "src/rrr/reactor/*.cc")
if(NOT TARGET rrr)
    add_library(rrr STATIC ${RRR_BENCH_SRC})
    target_include_directories(rrr PUBLIC src src/rrr ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include)
    target_compile_options(rrr PUBLIC ${BENCH_CXXFLAGS})
    target_link_libraries(rrr pthread ${Boost_LIBRARIES})
    
    if(ENABLE_BORROW_CHECKING)
        add_custom_target(borrow_check_rrr DEPENDS borrow_check_all_rrr)
    endif()
endif()

if(NOT TARGET memdb)
    file(GLOB MEMDB_BENCH_SRC "src/memdb/*.cc")
    add_library(memdb STATIC ${MEMDB_BENCH_SRC})
    target_include_directories(memdb PUBLIC src src/rrr src/memdb)
    target_compile_options(memdb PUBLIC ${BENCH_CXXFLAGS})
    target_link_libraries(memdb rrr pthread)
endif()

enable_testing()
set(BUILD_TESTS OFF)
find_package(GTest QUIET)
if(GTest_FOUND)
    set(BUILD_TESTS ON)
endif()

if(BUILD_TESTS)
    set(TEST_INCLUDE_DIRS src src/rrr src/memdb test)
    set(TEST_LINK_LIBS rrr memdb pthread ${BENCH_LDFLAGS} ${LIBEVENT_LDFLAGS})

    add_executable(test_marshal test/test_marshal.cc)
    target_include_directories(test_marshal PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_marshal PRIVATE ${BENCH_CXXFLAGS})
    target_link_libraries(test_marshal ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_marshal COMMAND test_marshal)

    add_executable(test_rpc test/test_rpc.cc test/benchmark_service.cc)
    target_include_directories(test_rpc PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_rpc PRIVATE ${BENCH_CXXFLAGS})
    target_link_libraries(test_rpc ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_rpc COMMAND test_rpc)

    add_executable(test_future test/test_future.cc)
    target_include_directories(test_future PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_future PRIVATE ${BENCH_CXXFLAGS})
    target_link_libraries(test_future ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_future COMMAND test_future)

    add_executable(test_reactor test/test_reactor.cc)
    target_include_directories(test_reactor PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_reactor PRIVATE ${BENCH_CXXFLAGS})
    target_link_libraries(test_reactor ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_reactor COMMAND test_reactor)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/coroutine.cc")
        add_executable(test_coroutine test/coroutine.cc)
        target_include_directories(test_coroutine PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
        target_compile_options(test_coroutine PRIVATE ${BENCH_CXXFLAGS})
        target_link_libraries(test_coroutine ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
        add_test(NAME test_coroutine COMMAND test_coroutine)
    endif()

    add_test(NAME simpleTransaction COMMAND simpleTransaction)
    add_test(NAME simplePaxos COMMAND bash -c "bash ./src/mako/update_config.sh && bash ./examples/simplePaxos.sh" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME shard1ReplicationSimple COMMAND bash -c "bash ./examples/test_1shard_replication_simple.sh" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_custom_target(run_tests COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure)
endif()# Tests subdirectory
add_subdirectory(tests)
