# Native Rust STO + RocksDB + Raft. Entered before Mako's compiler/dependency setup.
find_program(MAKO_CARGO_EXECUTABLE cargo REQUIRED)
find_program(MAKO_RUSTC_EXECUTABLE rustc REQUIRED)
execute_process(
    COMMAND "${MAKO_RUSTC_EXECUTABLE}" --version
    OUTPUT_VARIABLE _rust_version OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
if(NOT _rust_version MATCHES "rustc ([0-9]+\\.[0-9]+\\.[0-9]+)")
    message(FATAL_ERROR "Cannot parse Rust compiler version: ${_rust_version}")
endif()
if(CMAKE_MATCH_1 VERSION_LESS "1.97.1")
    message(FATAL_ERROR "ROCKSDB_RAFT requires Rust >= 1.97.1; found ${_rust_version}")
endif()

if(DEFINED ENV{ROCKSDB_LIB_DIR})
    find_library(MAKO_ROCKSDB_LIBRARY rocksdb
        PATHS "$ENV{ROCKSDB_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
else()
    find_library(MAKO_ROCKSDB_LIBRARY rocksdb
        PATHS /usr/local/lib /usr/lib/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu
              /usr/lib64 /usr/lib REQUIRED)
endif()
get_filename_component(_rocksdb_lib_dir "${MAKO_ROCKSDB_LIBRARY}" DIRECTORY)
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/src/srpc/third-party/lion/lion-reactor/Cargo.toml")
    message(FATAL_ERROR
        "Initialize the pinned native SRPC dependency: "
        "git submodule update --init src/srpc/third-party/lion")
endif()

if(DEFINED ENV{CARGO_TARGET_DIR})
    set(_rust_target_default "$ENV{CARGO_TARGET_DIR}")
else()
    set(_rust_target_default "${PROJECT_BINARY_DIR}/cargo")
endif()
set(MAKO_RUST_TARGET_DIR "${_rust_target_default}" CACHE PATH "Isolated Rust build artifacts")
set(_cargo_manifest "${PROJECT_SOURCE_DIR}/crates/Cargo.toml")
set(_cargo_env "ROCKSDB_LIB_DIR=${_rocksdb_lib_dir}")
set(_cargo_common --locked --manifest-path "${_cargo_manifest}" --target-dir "${MAKO_RUST_TARGET_DIR}")
option(MAKO_ROCKSDB_RAFT_TEST_HOOKS "Enable test-only service failure injection" OFF)
set(_cargo_service_features)
if(MAKO_ROCKSDB_RAFT_TEST_HOOKS)
    set(_cargo_service_features --features rocksdb-raft-service/test-hooks)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_cargo_profile debug)
    set(_cargo_build_flags)
else()
    set(_cargo_profile release)
    set(_cargo_build_flags --release)
endif()
set(_client_library "librocksdb_raft_service${CMAKE_SHARED_LIBRARY_SUFFIX}")

# Let Cargo track its complete dependency graph; no duplicated source manifest.
add_custom_target(rocksdb_raft ALL
    COMMAND "${CMAKE_COMMAND}" -E env ${_cargo_env}
        "${MAKO_CARGO_EXECUTABLE}" build ${_cargo_common} ${_cargo_build_flags}
        --package rocksdb-raft-service --bins --lib ${_cargo_service_features}
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${MAKO_RUST_TARGET_DIR}/${_cargo_profile}/rocksdb_raft_server"
        "${MAKO_RUST_TARGET_DIR}/${_cargo_profile}/rocksdb_raft_client"
        "${MAKO_RUST_TARGET_DIR}/${_cargo_profile}/${_client_library}"
        "${MAKO_RUST_TARGET_DIR}/${_cargo_profile}/rocksdb_raft_bench"
        "${PROJECT_BINARY_DIR}"
    BYPRODUCTS
        "${PROJECT_BINARY_DIR}/rocksdb_raft_server"
        "${PROJECT_BINARY_DIR}/rocksdb_raft_client"
        "${PROJECT_BINARY_DIR}/${_client_library}"
        "${PROJECT_BINARY_DIR}/rocksdb_raft_bench"
    USES_TERMINAL VERBATIM)
foreach(_binary rocksdb_raft_server rocksdb_raft_client rocksdb_raft_bench)
    install(PROGRAMS "${PROJECT_BINARY_DIR}/${_binary}" DESTINATION bin)
endforeach()
install(FILES "${PROJECT_BINARY_DIR}/${_client_library}" DESTINATION lib)
install(FILES "${PROJECT_SOURCE_DIR}/src/rocks_interface/rocksdb_raft.h"
    DESTINATION include/mako)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME sto_core_local
        COMMAND "${CMAKE_COMMAND}" -E env ${_cargo_env}
            "${MAKO_CARGO_EXECUTABLE}" test ${_cargo_common}
            --package sto-core --no-default-features --features test-install-observer)
    add_test(NAME sto_core_default
        COMMAND "${CMAKE_COMMAND}" -E env ${_cargo_env}
            "${MAKO_CARGO_EXECUTABLE}" test ${_cargo_common}
            --package sto-core --features test-install-observer)
    set(_rocksdb_raft_tests sto_core_local sto_core_default)
    foreach(_package mako-clock mako-engine-api)
        add_test(NAME ${_package}_default
            COMMAND "${CMAKE_COMMAND}" -E env ${_cargo_env}
                "${MAKO_CARGO_EXECUTABLE}" test ${_cargo_common} --package ${_package})
        add_test(NAME ${_package}_local
            COMMAND "${CMAKE_COMMAND}" -E env ${_cargo_env}
                "${MAKO_CARGO_EXECUTABLE}" test ${_cargo_common}
                --package ${_package} --no-default-features)
        list(APPEND _rocksdb_raft_tests ${_package}_default ${_package}_local)
    endforeach()
    foreach(_package mrx-rocks sto-rocksdb raft-host rocksdb-raft-service)
        set(_test_features)
        set(_test_arguments)
        if(_package STREQUAL "raft-host")
            # Protocol scenarios start real SRPC runtimes, disk fsyncs and
            # large snapshots; isolate scenarios rather than sharing timers.
            set(_test_arguments -- --test-threads=1)
        endif()
        if(_package STREQUAL "sto-rocksdb" OR _package STREQUAL "raft-host")
            set(_test_features --features test-hooks)
        elseif(_package STREQUAL "rocksdb-raft-service" AND MAKO_ROCKSDB_RAFT_TEST_HOOKS)
            set(_test_features --features test-hooks)
        endif()
        add_test(NAME ${_package}
            COMMAND "${CMAKE_COMMAND}" -E env ${_cargo_env}
                "${MAKO_CARGO_EXECUTABLE}" test ${_cargo_common}
                --package ${_package} --all-targets ${_test_features} ${_test_arguments})
        list(APPEND _rocksdb_raft_tests ${_package})
    endforeach()
    # The host depends on this separate workspace; testing raft-host alone does
    # not execute the store's WAL/rotation/power-cut and native base regressions.
    add_test(NAME raft_store
        COMMAND "${CMAKE_COMMAND}" -E env ${_cargo_env}
            "${MAKO_CARGO_EXECUTABLE}" test --locked
            --manifest-path "${PROJECT_SOURCE_DIR}/src/deptran/raft/Cargo.toml"
            --target-dir "${MAKO_RUST_TARGET_DIR}"
            --package raft-store --features rocksdb -- --test-threads=1)
    list(APPEND _rocksdb_raft_tests raft_store)
    set_tests_properties(${_rocksdb_raft_tests}
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 1800)
endif()
message(STATUS "ROCKSDB_RAFT: native Rust STO, system RocksDB, native Raft/SRPC; no Mako cache engines")
