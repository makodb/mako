cmake_minimum_required(VERSION 3.10)
# using g++
set(CMAKE_C_COMPILER "gcc" CACHE STRING "C compiler")
set(CMAKE_CXX_COMPILER "g++" CACHE STRING "C++ compiler")
project(mako)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

# Define paths
set(mkfile_path "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
set(HOME "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ERPC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third-party/erpc")
message(STATUS "I: ${ERPC_PATH}")
set(W "src/mako")
set(TOP "${CMAKE_SOURCE_DIR}")

# Read environment variable from env.txt (ib or dpdk)
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
  -L${ERPC_PATH}/build
  -Wl,--whole-archive
  ${DPDK_LDFLAGS} 
  -Wl,--no-whole-archive
  -lerpc
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
  # ${DPDK_CFLAGS} 
  -DERPC_LOG_LEVEL=6
  -DERPC_TESTING=false
  -DGFLAGS_IS_A_DLL=0
)
set(ERPC_LDFLAGS_IB
  -L${ERPC_PATH}/build
  -Wl,--whole-archive 
  ${DPDK_LDFLAGS} 
  -Wl,--no-whole-archive
  -lpthread
  -lerpc
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
  # ${DPDK_CFLAGS} 
  -DERPC_LOG_LEVEL=6
  -DERPC_TESTING=false
  -DGFLAGS_IS_A_DLL=0
)
set(ERPC_LDFLAGS_ETH
  -L${ERPC_PATH}/build
  -Wl,--whole-archive 
  ${DPDK_LDFLAGS} 
  -Wl,--no-whole-archive
  -lpthread
  -lerpc
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
option(PAXOS_LIB_ENABLED "Enable Paxos library" OFF)
option(DISABLE_MULTI_VERSION "Disable multi-version" OFF)
option(MICRO_BENCHMARK "Enable micro benchmark" OFF)
option(MEGA_BENCHMARK "Enable MEGA benchmark" OFF)
option(MEGA_BENCHMARK_MICRO "Enable MEGA benchmark micro" OFF)
option(TRACKING_LATENCY "Enable latency tracking" OFF)
set(SHARDS 1 CACHE STRING "Number of shards")
set(MERGE_KEYS_GROUPS "${SHARDS}" CACHE STRING "Merge keys groups")
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
  -DCONFIG_H="${CONFIG_H}" # No need for extra escaping with quotes
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
if(PAXOS_LIB_ENABLED)
  list(APPEND CXXFLAGS -DPAXOS_LIB_ENABLED)
endif()
if(DISABLE_MULTI_VERSION)
  list(APPEND CXXFLAGS -DDISABLE_MULTI_VERSION)
endif()
if(MICRO_BENCHMARK)
  list(APPEND CXXFLAGS -DMICRO_BENCHMARK)
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
  -DSHARDS=${SHARDS}
  -DMERGE_KEYS_GROUPS=${MERGE_KEYS_GROUPS}
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
  -lmemcached
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
set(SRCFILES
  "${W}/allocator.cc"
  "${W}/btree.cc"
  "${W}/core.cc"
  "${W}/counter.cc"
  "${W}/memory.cc"
  "${W}/rcu.cc"
  "${W}/stats_server.cc"
  "${W}/thread.cc"
  "${W}/ticker.cc"
  "${W}/tuple.cc"
  "${W}/txn_btree.cc"
  "${W}/txn.cc"
  "${W}/txn_proto2_impl.cc"
  "${W}/varint.cc"
)

set(MASSTREE_SRCFILES
  "${W}/masstree/compiler.cc"
  "${W}/masstree/str.cc"
  "${W}/masstree/string.cc"
  "${W}/masstree/straccum.cc"
  "${W}/masstree/json.cc"
  "${W}/masstree/kvthread.cc"
)

set(BENCH_SRCFILES
  "${W}/benchmarks/bench.cc"
  "${W}/benchmarks/encstress.cc"
  "${W}/benchmarks/bid.cc"
  "${W}/benchmarks/queue.cc"
  "${W}/benchmarks/tpcc.cc"
  "${W}/benchmarks/tpcc_simple.cc"
  "${W}/benchmarks/ycsb.cc"
  "${W}/benchmarks/sto/Transaction.cc"
  "${W}/benchmarks/sto/MassTrans.cc"
  "${W}/benchmarks/sto/common.cc"
  "${W}/benchmarks/sto/TRcu.cc"
  "${W}/benchmarks/sto/Packer.cc"
  "${W}/benchmarks/sto/ReplayDB.cc"
  "${W}/benchmarks/sto/ThreadPool.cc"
)

set(LIB_SRCFILES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/lookup3.cc"
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
add_custom_command(
  OUTPUT ${W}/masstree/config.h
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  #COMMAND rm -f "${W}/masstree/config.h"
  COMMAND cd "${W}/masstree" && ./configure ${MASSTREE_CONFIG_ARGS}
  COMMAND test -f "${W}/masstree/config.h" && touch "${W}/masstree/config.h" || true
  DEPENDS "${O}/${W}/buildstamp.masstree" "${W}/masstree/configure" "${W}/masstree/config.h.in"
)

add_custom_command(
  OUTPUT ${W}/masstree/configure ${W}/masstree/config.h.in
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMAND cd "${W}/masstree" && autoreconf -i && touch configure config.h.in
  DEPENDS "${W}/masstree/configure.ac"
)

# Custom command for lz4 library
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so
  COMMAND make -C "${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4" library
)

# Custom command for libtxlog.so
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/build/libtxlog.so
  COMMAND python3 waf configure build -M
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
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

# Add a masstree dependency
set_source_files_properties(${SRCFILES} PROPERTIES OBJECT_DEPENDS "${W}/masstree/config.h")

# Make all dependencies and objects into a common function
function(make_common_objects tgt)
  add_library(${tgt} OBJECT
    ${SRCFILES} # basic for mako: allocator or btree
    ${MASSTREE_SRCFILES} # masstree
    ${BENCH_SRCFILES} # tpcc and ycsb, and txn proto
    ${LIB_SRCFILES} # erpc library
  )
  target_include_directories(${tgt} PRIVATE
    "${W}/masstree"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mako"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${HOME}"
  )
  target_compile_options(${tgt} PRIVATE ${BENCH_CXXFLAGS})
  target_compile_definitions(${tgt} PRIVATE CONFIG_H="${CONFIG_H}")
  target_include_directories(${tgt} PRIVATE ${LIBEVENT_INCLUDE_DIRS})


endfunction()

######## ---------------- Compile a dbtest ---------------- ######## 
set(EXEC_NAME "dbtest")
set(MAIN_FILE
  "${W}/benchmarks/dbtest.cc"
)
make_common_objects(${EXEC_NAME}_common_objects)
target_sources(${EXEC_NAME}_common_objects PRIVATE ${MAIN_FILE})
add_executable(${EXEC_NAME}
  $<TARGET_OBJECTS:${EXEC_NAME}_common_objects>
  ${CMAKE_CURRENT_SOURCE_DIR}/build/libtxlog.so
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so
)
target_link_libraries(${EXEC_NAME} PRIVATE 
  ${BENCH_LDFLAGS} 
  ${LZ4LDFLAGS} 
  -lyaml-cpp 
  -L${CMAKE_CURRENT_SOURCE_DIR}/build
  -ljemalloc
  ${CMAKE_CURRENT_SOURCE_DIR}/build/libtxlog.so
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so
)
target_include_directories(${EXEC_NAME} PRIVATE ${LIBEVENT_INCLUDE_DIRS})

# ######## ---------------- Compile a basic ---------------- ######## 
set(EXEC_NAME "basic")
set(MAIN_FILE
  "${W}/benchmarks/ut/basic.cc"
)
make_common_objects(${EXEC_NAME}_common_objects)
target_sources(${EXEC_NAME}_common_objects PRIVATE ${MAIN_FILE})
add_executable(${EXEC_NAME}
  $<TARGET_OBJECTS:${EXEC_NAME}_common_objects>
  ${CMAKE_CURRENT_SOURCE_DIR}/build/libtxlog.so
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so
)
target_link_libraries(${EXEC_NAME} PRIVATE 
  ${BENCH_LDFLAGS} 
  ${LZ4LDFLAGS} 
  -lyaml-cpp 
  -L${CMAKE_CURRENT_SOURCE_DIR}/build
  -ljemalloc
  ${CMAKE_CURRENT_SOURCE_DIR}/build/libtxlog.so
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so
)
target_include_directories(${EXEC_NAME} PRIVATE ${LIBEVENT_INCLUDE_DIRS})

######## ---------------- Compile a paxos_async_commit_test ---------------- ######## 
set(EXEC_NAME "paxos_async_commit_test")
set(MAIN_FILE
  "${W}/benchmarks/paxos_async_commit_test.cc"
)
make_common_objects(${EXEC_NAME}_common_objects)
target_sources(${EXEC_NAME}_common_objects PRIVATE ${MAIN_FILE})
add_executable(${EXEC_NAME}
  $<TARGET_OBJECTS:${EXEC_NAME}_common_objects>
  ${CMAKE_CURRENT_SOURCE_DIR}/build/libtxlog.so
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so
)
target_link_libraries(${EXEC_NAME} PRIVATE 
  ${BENCH_LDFLAGS} 
  ${LZ4LDFLAGS} 
  -lyaml-cpp 
  -L${CMAKE_CURRENT_SOURCE_DIR}/build
  -ljemalloc
  ${CMAKE_CURRENT_SOURCE_DIR}/build/libtxlog.so
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so
)
target_include_directories(${EXEC_NAME} PRIVATE ${LIBEVENT_INCLUDE_DIRS})

# ######## ---------------- Compile a eprc_client ---------------- ######## 
set(EXEC_NAME "erpc_client")
set(MAIN_FILE
  "${W}/benchmarks/ut/erpc_client.cc"
)
make_common_objects(${EXEC_NAME}_common_objects)
target_sources(${EXEC_NAME}_common_objects PRIVATE ${MAIN_FILE})
add_executable(${EXEC_NAME}
  $<TARGET_OBJECTS:${EXEC_NAME}_common_objects>
  ${CMAKE_CURRENT_SOURCE_DIR}/build/libtxlog.so
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so
)
target_link_libraries(${EXEC_NAME} PRIVATE 
  ${BENCH_LDFLAGS} 
  ${LZ4LDFLAGS} 
  -lyaml-cpp 
  -L${CMAKE_CURRENT_SOURCE_DIR}/build
  -ljemalloc
  ${CMAKE_CURRENT_SOURCE_DIR}/build/libtxlog.so
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so
)
target_include_directories(${EXEC_NAME} PRIVATE ${LIBEVENT_INCLUDE_DIRS})

# ######## ---------------- Compile a erpc_server ---------------- ######## 
set(EXEC_NAME "erpc_server")
set(MAIN_FILE
  "${W}/benchmarks/ut/erpc_server.cc"
)
make_common_objects(${EXEC_NAME}_common_objects)
target_sources(${EXEC_NAME}_common_objects PRIVATE ${MAIN_FILE})
add_executable(${EXEC_NAME}
  $<TARGET_OBJECTS:${EXEC_NAME}_common_objects>
  ${CMAKE_CURRENT_SOURCE_DIR}/build/libtxlog.so
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so
)
target_link_libraries(${EXEC_NAME} PRIVATE 
  ${BENCH_LDFLAGS} 
  ${LZ4LDFLAGS} 
  -lyaml-cpp 
  -L${CMAKE_CURRENT_SOURCE_DIR}/build
  -ljemalloc
  ${CMAKE_CURRENT_SOURCE_DIR}/build/libtxlog.so
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so
)
target_include_directories(${EXEC_NAME} PRIVATE ${LIBEVENT_INCLUDE_DIRS})