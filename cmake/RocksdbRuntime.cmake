# The distributed Mako runtime with direct RocksDB data, without a Masstree build.
find_program(MAKO_CARGO_EXECUTABLE cargo REQUIRED)
find_program(MAKO_RUSTC_EXECUTABLE rustc REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(COMMAND "${MAKO_RUSTC_EXECUTABLE}" --version
    OUTPUT_VARIABLE _rust_version OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT _rust_version MATCHES "rustc ([0-9]+\\.[0-9]+\\.[0-9]+)")
    message(FATAL_ERROR "Cannot parse Rust version: ${_rust_version}")
endif()
if(CMAKE_MATCH_1 VERSION_LESS "1.97.1")
    message(FATAL_ERROR "ROCKSDB_RUNTIME requires Rust >= 1.97.1")
endif()
if(DEFINED ENV{ROCKSDB_LIB_DIR})
    find_library(MAKO_ROCKSDB_LIBRARY rocksdb PATHS "$ENV{ROCKSDB_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
else()
    find_library(MAKO_ROCKSDB_LIBRARY rocksdb
        PATHS /usr/local/lib /usr/lib/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu /usr/lib64 /usr/lib REQUIRED)
endif()
get_filename_component(_rocksdb_library_dir "${MAKO_ROCKSDB_LIBRARY}" DIRECTORY)
set(MAKO_RUST_TARGET_DIR "${PROJECT_BINARY_DIR}/cargo" CACHE PATH "Isolated Rust artifacts")
set(_cargo_env "ROCKSDB_LIB_DIR=${_rocksdb_library_dir}" "CARGO_TARGET_DIR=${MAKO_RUST_TARGET_DIR}")
set(_manifest "${PROJECT_SOURCE_DIR}/crates/Cargo.toml")
set(_common --locked --manifest-path "${_manifest}" --package mako-runtime
    --no-default-features --features logging,rocksdb-engine)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_profile debug)
    set(_profile_flags)
else()
    set(_profile release)
    set(_profile_flags --release)
endif()
set(_programs mako_node mako_admin mako_client mako_bench mako_transactions mako_redis mako_convert)
add_custom_target(mako_runtime ALL
    COMMAND "${CMAKE_COMMAND}" -E env ${_cargo_env}
        "${MAKO_CARGO_EXECUTABLE}" build ${_common} ${_profile_flags} --bins
    COMMAND "${CMAKE_COMMAND}" -E env ${_cargo_env}
        "${MAKO_CARGO_EXECUTABLE}" build --locked --manifest-path "${_manifest}"
        --package mako-rocksdb --features rocksdb-engine ${_profile_flags} --example rocksdb_transactions
    USES_TERMINAL VERBATIM)
foreach(_program IN LISTS _programs)
    add_custom_command(TARGET mako_runtime POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${MAKO_RUST_TARGET_DIR}/${_profile}/${_program}" "${PROJECT_BINARY_DIR}/${_program}" VERBATIM)
    add_custom_target(${_program} DEPENDS mako_runtime)
    install(PROGRAMS "${PROJECT_BINARY_DIR}/${_program}" DESTINATION bin)
endforeach()
add_custom_target(rocks_runtime_audit
    COMMAND "${CMAKE_COMMAND}" -E env ${_cargo_env}
        "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/audit_rocks_runtime.py"
        --cargo "${MAKO_CARGO_EXECUTABLE}" --target-dir "${MAKO_RUST_TARGET_DIR}" --profile ${_profile}
        --output "${PROJECT_BINARY_DIR}/rocks-runtime-audit.json"
    DEPENDS mako_runtime USES_TERMINAL VERBATIM)
add_custom_target(rust_runtime_tests
    COMMAND "${CMAKE_COMMAND}" -E env ${_cargo_env}
        "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/run_native_rust_tests.py"
        --cargo "${MAKO_CARGO_EXECUTABLE}" --manifest "${_manifest}"
        --package mako-runtime --package mako-rocksdb --features mako-runtime/logging,mako-runtime/rocksdb-engine,mako-rocksdb/rocksdb-engine ${_profile_flags}
    USES_TERMINAL VERBATIM)
message(STATUS "ROCKSDB_RUNTIME: distributed STO/OCC+2PC, local/Raft logs and Rust SRPC; no Masstree compiler or archive")
