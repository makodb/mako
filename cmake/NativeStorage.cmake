# Native dependency closure for Rust applications. This file must not add
# Mako application, replication, RPC, or generated Rust-to-C++ providers.
set(USE_MALLOC_MODE 1 CACHE STRING "Native storage allocator: 0=libc, 1=jemalloc, 2=tcmalloc, 3=flow")
set(MASSTREE_MAX_KEY_LEN 1024 CACHE STRING "Maximum native Masstree key bytes")
set(MASSTREE_ROW_TYPE bag CACHE STRING "Masstree row layout")
option(MASSTREE_ENABLE_SUPERPAGE "Enable native storage superpages" ON)
option(CHECK_INVARIANTS "Enable native storage invariants" OFF)
option(DEBUG "Enable native storage assertions" OFF)
set(CONFIG_H "${CMAKE_SOURCE_DIR}/src/mako/config/config-perf.h")
include("${CMAKE_SOURCE_DIR}/src/masstree/ConfigureMasstree.cmake")
find_package(PkgConfig REQUIRED)
include("${CMAKE_SOURCE_DIR}/cmake/MakoAllocator.cmake")
mako_configure_allocator()

file(STRINGS "${CMAKE_SOURCE_DIR}/cmake/native-storage-sources.txt" _native_sources
    REGEX "^[^#].*[^ ]$")
add_library(mako_native_storage STATIC ${_native_sources})
set_target_properties(mako_native_storage PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(mako_native_storage BEFORE PUBLIC
    "${MASSTREE_CONFIG_INCLUDE_DIR}" "${CMAKE_BINARY_DIR}/generated")
target_include_directories(mako_native_storage PUBLIC
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/mako"
    "${CMAKE_SOURCE_DIR}/src/masstree"
    "${CMAKE_SOURCE_DIR}/third-party/rusty-cpp/include")
target_compile_definitions(mako_native_storage PUBLIC
    CONFIG_H="${CONFIG_H}" MASSTREE_CONFIG_H="${MASSTREE_GENERATED_CONFIG_H}"
    RUSTYCPP_DISABLE_ARC_LOG HASHTABLE=0)
if(USE_MALLOC_MODE EQUAL 1)
    target_compile_definitions(mako_native_storage PRIVATE USE_JEMALLOC)
elseif(USE_MALLOC_MODE EQUAL 2)
    target_compile_definitions(mako_native_storage PRIVATE USE_TCMALLOC)
elseif(USE_MALLOC_MODE EQUAL 3)
    target_compile_definitions(mako_native_storage PRIVATE USE_FLOW)
endif()
# A forced include on the target would also affect CMake's synthetic std.cppm,
# where the leading module declaration must be the first token.
foreach(_source IN LISTS _native_sources)
    set_property(SOURCE "${_source}" APPEND PROPERTY COMPILE_OPTIONS
        -include "${MASSTREE_GENERATED_CONFIG_H}")
endforeach()
target_link_libraries(mako_native_storage PUBLIC mako_allocator numa pthread)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(native_storage_source_audit ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_native_rust_build.py"
        --build "${CMAKE_BINARY_DIR}" --archive "$<TARGET_FILE:mako_native_storage>"
        --nm "${CMAKE_NM}"
    DEPENDS mako_native_storage
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VERBATIM)

find_program(MAKO_NATIVE_CARGO cargo REQUIRED)
set(MAKO_NATIVE_CARGO_TARGET "${CMAKE_BINARY_DIR}/cargo" CACHE PATH
    "Cargo output directory for the native Rust application")
set(_native_library_dirs "${CMAKE_BINARY_DIR}")
if(EXISTS "${MAKO_CLANG_ROOT}/lib/libc++.so")
    string(APPEND _native_library_dirs ":${MAKO_CLANG_ROOT}/lib")
endif()
set(_native_libraries "static=mako_native_storage,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread")
if(MAKO_ALLOCATOR_LINK_NAME)
    string(APPEND _native_libraries ",dylib=${MAKO_ALLOCATOR_LINK_NAME}")
    get_filename_component(_allocator_dir "${MAKO_ALLOCATOR_LIBRARY}" DIRECTORY)
    string(APPEND _native_library_dirs ":${_allocator_dir}")
endif()
set(MAKO_NATIVE_CARGO_ENV
    "CARGO_TARGET_DIR=${MAKO_NATIVE_CARGO_TARGET}"
    "MAKO_MTREE_NATIVE_INTEGRATION=1"
    "MAKO_MTREE_NATIVE_LIB_DIRS=${_native_library_dirs}"
    "MAKO_MTREE_NATIVE_LIBS=${_native_libraries}")
set(_cargo_manifest "${CMAKE_SOURCE_DIR}/crates/Cargo.toml")

# Cargo owns all application and network code. Only the audited storage archive
# crosses the native boundary; SRPC is compiled directly as a Rust dependency.
add_custom_target(mako_node ALL
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${MAKO_NATIVE_CARGO}" build --locked --manifest-path "${_cargo_manifest}"
        -p mako-runtime --features replication --bin mako_node
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${MAKO_NATIVE_CARGO_TARGET}/debug/mako_node" "${CMAKE_BINARY_DIR}/mako_node"
    DEPENDS mako_native_storage
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)

# The existing direct RocksDB service also uses native SRPC/Raft. Keep its
# production tools in the new profile without introducing a Masstree dependency.
add_custom_target(rocksdb_raft_tools ALL
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${MAKO_NATIVE_CARGO_TARGET}"
        "${MAKO_NATIVE_CARGO}" build --locked --manifest-path "${_cargo_manifest}"
        -p rocksdb-raft-service --bins
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${MAKO_NATIVE_CARGO_TARGET}/debug/rocksdb_raft_server"
        "${MAKO_NATIVE_CARGO_TARGET}/debug/rocksdb_raft_client"
        "${MAKO_NATIVE_CARGO_TARGET}/debug/rocksdb_raft_bench"
        "${CMAKE_BINARY_DIR}/"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)

# Component tests remain separate. The transaction-manager suite never receives
# native library settings; application/adapter tests explicitly use the tree.
add_custom_target(rust_sto_transaction_tests
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${MAKO_NATIVE_CARGO_TARGET}"
        "${MAKO_NATIVE_CARGO}" test --locked --manifest-path "${_cargo_manifest}"
        -p sto-core -p sto-test-datatypes
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_custom_target(rust_masstree_tests
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_native_rust_tests.py"
        --cargo "${MAKO_NATIVE_CARGO}" --manifest "${_cargo_manifest}"
        --package masstree --package sto-masstree
    DEPENDS mako_native_storage
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_custom_target(rust_application_adapter_tests
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_native_rust_tests.py"
        --cargo "${MAKO_NATIVE_CARGO}" --manifest "${_cargo_manifest}"
        --package sto-tpcc-ffi
    DEPENDS mako_native_storage
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)

enable_testing()
add_custom_target(rust_runtime_tests
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${MAKO_NATIVE_CARGO_TARGET}"
        "${MAKO_NATIVE_CARGO}" test --locked --manifest-path "${_cargo_manifest}"
        -p mako-runtime --lib
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_custom_target(rust_shard_integration_tests
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_native_rust_tests.py"
        --cargo "${MAKO_NATIVE_CARGO}" --manifest "${_cargo_manifest}"
        --package mako-runtime --features replication
    DEPENDS mako_native_storage
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_test(NAME rust_shard_integration
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_native_rust_tests.py"
        --cargo "${MAKO_NATIVE_CARGO}" --manifest "${_cargo_manifest}"
        --package mako-runtime --features replication)
add_test(NAME native_storage_source_audit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_native_rust_build.py"
        --build "${CMAKE_BINARY_DIR}" --archive "$<TARGET_FILE:mako_native_storage>"
        --nm "${CMAKE_NM}")
add_test(NAME rust_application_adapter
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_native_rust_tests.py"
        --cargo "${MAKO_NATIVE_CARGO}" --manifest "${_cargo_manifest}"
        --package sto-tpcc-ffi)
set_tests_properties(native_storage_source_audit rust_application_adapter rust_shard_integration PROPERTIES
    LABELS "native-rust" TIMEOUT 900)
