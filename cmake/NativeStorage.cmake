# Native dependency closure for Rust applications. This file must not add
# Mako application, replication, RPC, or generated Rust-to-C++ providers.
set(USE_MALLOC_MODE 1 CACHE STRING "Native storage allocator: 0=libc, 1=jemalloc, 2=tcmalloc, 3=flow")
set(MASSTREE_MAX_KEY_LEN 1024 CACHE STRING "Maximum native Masstree key bytes")
set(MASSTREE_ROW_TYPE bag CACHE STRING "Masstree row layout")
option(MASSTREE_ENABLE_SUPERPAGE "Enable native storage superpages" ON)
option(CHECK_INVARIANTS "Enable native storage invariants" OFF)
option(DEBUG "Enable native storage assertions" OFF)
# Keep sanitizer instrumentation scoped to the native storage dependency.
set(_native_sanitizer "")
foreach(_kind ASAN UBSAN TSAN)
    option(MAKO_${_kind} "Instrument native storage with ${_kind}" "$ENV{MAKO_${_kind}}")
    if(MAKO_${_kind})
        if(_native_sanitizer)
            message(FATAL_ERROR "Select one native sanitizer per build")
        endif()
        if(_kind STREQUAL "ASAN")
            set(_native_sanitizer address)
        elseif(_kind STREQUAL "UBSAN")
            set(_native_sanitizer undefined)
        else()
            set(_native_sanitizer thread)
        endif()
    endif()
endforeach()
if(MAKO_ASAN OR MAKO_TSAN)
    set(USE_MALLOC_MODE 0 CACHE STRING "Sanitizers require the system allocator" FORCE)
endif()
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
if(_native_sanitizer)
    target_compile_options(mako_native_storage PRIVATE -fsanitize=${_native_sanitizer} -fno-omit-frame-pointer)
    target_link_options(mako_native_storage INTERFACE -fsanitize=${_native_sanitizer})
    message(STATUS "Native storage sanitizer: ${_native_sanitizer}")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
option(MAKO_STORAGE_TESTS_ONLY "Build the separate C++ Masstree component tests" OFF)
if(MAKO_STORAGE_TESTS_ONLY)
    if(MAKO_BUILD_PROFILE STREQUAL "MASSTREE_TX")
        message(FATAL_ERROR "MASSTREE_TX owns transaction adapter tests; use a separate NATIVE_RUST build with MAKO_STORAGE_TESTS_ONLY for index tests")
    endif()
    include("${CMAKE_SOURCE_DIR}/src/masstree/CMakeLists.txt")
    return()
endif()
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
set(_native_libraries "static:-bundle=mako_native_storage,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread")
if(MAKO_ALLOCATOR_LINK_NAME)
    string(APPEND _native_libraries ",dylib=${MAKO_ALLOCATOR_LINK_NAME}")
    get_filename_component(_allocator_dir "${MAKO_ALLOCATOR_LIBRARY}" DIRECTORY)
    string(APPEND _native_library_dirs ":${_allocator_dir}")
endif()
set(MAKO_NATIVE_CARGO_ENV
    "CARGO_TARGET_DIR=${MAKO_NATIVE_CARGO_TARGET}"
    "MAKO_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "LIBCXX_DIR=${MAKO_CLANG_ROOT}/lib"
    "MAKO_MTREE_NATIVE_INTEGRATION=1"
    "MAKO_MTREE_NATIVE_LIB_DIRS=${_native_library_dirs}"
    "MAKO_MTREE_NATIVE_LIBS=${_native_libraries}")
set(_cargo_manifest "${CMAKE_SOURCE_DIR}/crates/Cargo.toml")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_native_profile_default dev)
else()
    set(_native_profile_default release)
endif()
set(MAKO_NATIVE_CARGO_PROFILE "${_native_profile_default}" CACHE STRING "Rust application profile: dev or release")
set_property(CACHE MAKO_NATIVE_CARGO_PROFILE PROPERTY STRINGS dev release)
if(MAKO_NATIVE_CARGO_PROFILE STREQUAL "release")
    set(_native_profile_flags --release)
    set(_native_output release)
elseif(MAKO_NATIVE_CARGO_PROFILE STREQUAL "dev")
    set(_native_profile_flags)
    set(_native_output debug)
else()
    message(FATAL_ERROR "Unknown MAKO_NATIVE_CARGO_PROFILE: ${MAKO_NATIVE_CARGO_PROFILE}")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/MasstreeTransaction.cmake")
if(MAKO_BUILD_PROFILE STREQUAL "MASSTREE_TX")
    message(STATUS "MASSTREE_TX: transaction adapter and wrapper; no application, RocksDB, SRPC or Raft build")
    return()
endif()

add_custom_target(workload_boundary_audit ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_workload_boundary.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" VERBATIM)

# Cargo owns all application and network code. Only the audited storage archive
# crosses the native boundary; SRPC is compiled directly as a Rust dependency.
foreach(_program mako_node mako_bench mako_transactions mako_redis)
    set(_program_package mako-runtime)
    if(_program STREQUAL "mako_bench")
        set(_program_package mako-workloads)
    endif()
    add_custom_target(${_program} ALL
        COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
            "${MAKO_NATIVE_CARGO}" build --locked --manifest-path "${_cargo_manifest}" ${_native_profile_flags}
            -p ${_program_package} --features replication,rocksdb-engine --bin ${_program}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MAKO_NATIVE_CARGO_TARGET}/${_native_output}/${_program}" "${CMAKE_BINARY_DIR}/${_program}"
        DEPENDS mako_native_storage
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
endforeach()

# External clients only use native SRPC and the checked wire protocol.
foreach(_program mako_client mako_admin)
    add_custom_target(${_program} ALL
        COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${MAKO_NATIVE_CARGO_TARGET}"
            "${MAKO_NATIVE_CARGO}" build --locked --manifest-path "${_cargo_manifest}" ${_native_profile_flags}
            -p mako-runtime --bin ${_program}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MAKO_NATIVE_CARGO_TARGET}/${_native_output}/${_program}" "${CMAKE_BINARY_DIR}/${_program}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
endforeach()

# Component tests remain separate. The transaction-manager suite never receives
# native library settings; application/adapter tests explicitly use the tree.
add_custom_target(rust_sto_transaction_tests
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${MAKO_NATIVE_CARGO_TARGET}"
        "${MAKO_NATIVE_CARGO}" test --locked --manifest-path "${_cargo_manifest}"
        -p sto-core -p sto-test-datatypes
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_custom_target(rust_support_tests
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${MAKO_NATIVE_CARGO_TARGET}"
        "${MAKO_NATIVE_CARGO}" test --locked --manifest-path "${_cargo_manifest}"
        -p mako-history -p shard-host-core
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_custom_target(rust_masstree_tests
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_native_rust_tests.py"
        --cargo "${MAKO_NATIVE_CARGO}" --manifest "${_cargo_manifest}"
        --package masstree
        --package mtree-sys
    DEPENDS mako_native_storage
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
# Compatibility name; the component now owns both adapter packages.
add_custom_target(rust_application_adapter_tests DEPENDS masstree_tx_tests)

enable_testing()
add_custom_target(rust_runtime_tests
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${MAKO_NATIVE_CARGO_TARGET}"
        "${MAKO_NATIVE_CARGO}" test --locked --manifest-path "${_cargo_manifest}"
        -p mako-runtime --lib
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_custom_target(rust_workload_tests
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s scripts/tests -p test_workload_boundary.py
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_native_rust_tests.py"
        --cargo "${MAKO_NATIVE_CARGO}" --manifest "${_cargo_manifest}"
        --package mako-workloads --features replication,rocksdb-engine
    DEPENDS mako_native_storage
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_custom_target(rust_shard_integration_tests
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_native_rust_tests.py"
        --cargo "${MAKO_NATIVE_CARGO}" --manifest "${_cargo_manifest}"
        --package mako-runtime --package mako-rocksdb --package mako-workloads --features mako-runtime/replication,mako-runtime/rocksdb-engine,mako-rocksdb/masstree-engine,mako-rocksdb/native-fixtures,mako-workloads/replication,mako-workloads/rocksdb-engine
    DEPENDS mako_native_storage
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_test(NAME rust_shard_integration
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_native_rust_tests.py"
        --cargo "${MAKO_NATIVE_CARGO}" --manifest "${_cargo_manifest}"
        --package mako-runtime --package mako-rocksdb --package mako-workloads --features mako-runtime/replication,mako-runtime/rocksdb-engine,mako-rocksdb/masstree-engine,mako-rocksdb/native-fixtures,mako-workloads/replication,mako-workloads/rocksdb-engine)
add_custom_target(rust_runtime_release_tests
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_native_rust_tests.py"
        --cargo "${MAKO_NATIVE_CARGO}" --manifest "${_cargo_manifest}"
        --package mako-runtime --package mako-rocksdb --package mako-workloads --features mako-runtime/replication,mako-runtime/rocksdb-engine,mako-rocksdb/masstree-engine,mako-rocksdb/native-fixtures,mako-workloads/replication,mako-workloads/rocksdb-engine --release
    DEPENDS mako_native_storage
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_custom_target(rust_native_dependency_audit
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/audit_native_dependencies.py"
        --build "${CMAKE_BINARY_DIR}" --cargo "${MAKO_NATIVE_CARGO}"
        --nm "${CMAKE_NM}"
    DEPENDS mako_native_storage
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_custom_target(rust_native_quality
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${MAKO_NATIVE_CARGO}" clippy --locked --manifest-path "${_cargo_manifest}"
        -p mako-runtime -p mako-rocksdb -p mako-workloads -p mako-storage-api -p mako-storage-rocksdb -p raft-host
        --features mako-runtime/replication,mako-runtime/rocksdb-engine,mako-rocksdb/masstree-engine,mako-rocksdb/native-fixtures,mako-workloads/replication,mako-workloads/rocksdb-engine --all-targets --no-deps -- -D warnings
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${MAKO_NATIVE_CARGO}" test --locked --manifest-path "${_cargo_manifest}"
        -p mako-runtime -p mako-rocksdb -p mako-workloads --features mako-runtime/replication,mako-runtime/rocksdb-engine,mako-rocksdb/masstree-engine,mako-rocksdb/native-fixtures,mako-workloads/replication,mako-workloads/rocksdb-engine --doc
    DEPENDS mako_native_storage
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_test(NAME native_storage_source_audit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_native_rust_build.py"
        --build "${CMAKE_BINARY_DIR}" --archive "$<TARGET_FILE:mako_native_storage>"
        --nm "${CMAKE_NM}")
set_tests_properties(native_storage_source_audit rust_shard_integration PROPERTIES
    LABELS "native-rust" TIMEOUT 900)

foreach(_program mako_node mako_bench mako_transactions mako_redis mako_client mako_admin)
    install(PROGRAMS "${CMAKE_BINARY_DIR}/${_program}" DESTINATION bin)
endforeach()
