cmake_minimum_required(VERSION 3.30)

foreach(_required IN ITEMS
        MAKO_CARGO_EXECUTABLE
        MAKO_RUST_MANIFEST
        MAKO_CARGO_TARGET_DIR
        MAKO_ARCHIVE
        MASSTREE_ARCHIVE
        MAKO_CXX_RUNTIME_DIR
        MAKO_LOADER_PATH_VARIABLE
        MAKO_NATIVE_LIBS)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunRustMasstreeNativeIntegration: missing ${_required}")
    endif()
endforeach()

foreach(_archive IN ITEMS "${MAKO_ARCHIVE}" "${MASSTREE_ARCHIVE}")
    if(NOT EXISTS "${_archive}")
        message(FATAL_ERROR "Native archive was not built: ${_archive}")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${MAKO_CXX_RUNTIME_DIR}")
    message(FATAL_ERROR
        "C++ runtime directory does not exist: ${MAKO_CXX_RUNTIME_DIR}")
endif()
if(NOT EXISTS "${MAKO_RUST_MANIFEST}")
    message(FATAL_ERROR "Rust workspace manifest does not exist: ${MAKO_RUST_MANIFEST}")
endif()

get_filename_component(_mako_archive_dir "${MAKO_ARCHIVE}" DIRECTORY)
get_filename_component(_masstree_archive_dir "${MASSTREE_ARCHIVE}" DIRECTORY)
set(_native_directories
    "${_mako_archive_dir}"
    "${_masstree_archive_dir}"
    "${MAKO_CXX_RUNTIME_DIR}"
)
list(REMOVE_DUPLICATES _native_directories)
cmake_path(CONVERT "${_native_directories}" TO_NATIVE_PATH_LIST
    _native_path_list NORMALIZE)

set(_loader_directories "${MAKO_CXX_RUNTIME_DIR}")
if(DEFINED ENV{${MAKO_LOADER_PATH_VARIABLE}}
        AND NOT "$ENV{${MAKO_LOADER_PATH_VARIABLE}}" STREQUAL "")
    cmake_path(CONVERT "$ENV{${MAKO_LOADER_PATH_VARIABLE}}"
        TO_CMAKE_PATH_LIST _inherited_loader_directories NORMALIZE)
    list(APPEND _loader_directories ${_inherited_loader_directories})
endif()
list(REMOVE_DUPLICATES _loader_directories)
cmake_path(CONVERT "${_loader_directories}" TO_NATIVE_PATH_LIST
    _loader_path_list NORMALIZE)

set(_native_environment
    "MAKO_MTREE_NATIVE_INTEGRATION=1"
    "MAKO_MTREE_NATIVE_LIB_DIRS=${_native_path_list}"
    "MAKO_MTREE_NATIVE_LIBS=${MAKO_NATIVE_LIBS}"
    "CARGO_TARGET_DIR=${MAKO_CARGO_TARGET_DIR}"
    "${MAKO_LOADER_PATH_VARIABLE}=${_loader_path_list}"
)

message(STATUS "Mako archive: ${MAKO_ARCHIVE}")
message(STATUS "Masstree archive: ${MASSTREE_ARCHIVE}")
message(STATUS "Rust native libraries: ${MAKO_NATIVE_LIBS}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${_native_environment}
        "${MAKO_CARGO_EXECUTABLE}" test
        --manifest-path "${MAKO_RUST_MANIFEST}"
        --locked
        -p masstree
        --test native_integration
    COMMAND_ECHO STDOUT
    RESULT_VARIABLE _masstree_result
)
if(NOT _masstree_result EQUAL 0)
    message(FATAL_ERROR
        "Rust Masstree native integration failed with exit code ${_masstree_result}")
endif()

# Run each transactional-adapter suite in its own process. Native runtime and
# worker registrations have process-wide lifetimes, so process isolation keeps
# one suite's registrations out of the next suite's fixed worker budget.
if(DEFINED MAKO_STO_NATIVE_TEST
        AND NOT "${MAKO_STO_NATIVE_TEST}" STREQUAL "")
    if(NOT EXISTS "${MAKO_STO_NATIVE_TEST}")
        message(FATAL_ERROR
            "Configured sto-masstree native test does not exist: ${MAKO_STO_NATIVE_TEST}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${_native_environment}
            "${MAKO_CARGO_EXECUTABLE}" test
            --manifest-path "${MAKO_RUST_MANIFEST}"
            --locked
            -p sto-masstree
            --all-features
            --test native_integration
        COMMAND_ECHO STDOUT
        RESULT_VARIABLE _sto_masstree_result
    )
    if(NOT _sto_masstree_result EQUAL 0)
        message(FATAL_ERROR
            "Rust STO Masstree native integration failed with exit code ${_sto_masstree_result}")
    endif()
endif()

if(DEFINED MAKO_STO_HISTORY_TEST
        AND NOT "${MAKO_STO_HISTORY_TEST}" STREQUAL "")
    if(NOT EXISTS "${MAKO_STO_HISTORY_TEST}")
        message(FATAL_ERROR
            "Configured sto-masstree history test does not exist: ${MAKO_STO_HISTORY_TEST}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${_native_environment}
            "${MAKO_CARGO_EXECUTABLE}" test
            --manifest-path "${MAKO_RUST_MANIFEST}"
            --locked
            -p sto-masstree
            --all-features
            --test history_oracle
        COMMAND_ECHO STDOUT
        RESULT_VARIABLE _sto_masstree_history_result
    )
    if(NOT _sto_masstree_history_result EQUAL 0)
        message(FATAL_ERROR
            "Rust STO Masstree history oracle failed with exit code ${_sto_masstree_history_result}")
    endif()
endif()

if(DEFINED MAKO_STO_TPCC_NATIVE_TEST
        AND NOT "${MAKO_STO_TPCC_NATIVE_TEST}" STREQUAL "")
    if(NOT EXISTS "${MAKO_STO_TPCC_NATIVE_TEST}")
        message(FATAL_ERROR
            "Configured sto-tpcc-ffi native test does not exist: ${MAKO_STO_TPCC_NATIVE_TEST}")
    endif()

    # The FFI crate's unit-test binary retains references from its exported
    # entry points into the native Masstree adapter, so link it here with the
    # authoritative CMake archives. Several unit cases create their own native
    # runtime and table set. The native registries have process-wide lifetimes,
    # so discover the cases first and run each one in a fresh process.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${_native_environment}
            "${MAKO_CARGO_EXECUTABLE}" test
            --manifest-path "${MAKO_RUST_MANIFEST}"
            --locked
            -p sto-tpcc-ffi
            --lib
            --
            --list
            --format terse
        COMMAND_ECHO STDOUT
        RESULT_VARIABLE _sto_tpcc_unit_list_result
        OUTPUT_VARIABLE _sto_tpcc_unit_list_output
        ERROR_VARIABLE _sto_tpcc_unit_list_error
    )
    if(NOT _sto_tpcc_unit_list_result EQUAL 0)
        message(FATAL_ERROR
            "Could not list Rust STO TPC-C FFI unit tests (exit code "
            "${_sto_tpcc_unit_list_result}):\n${_sto_tpcc_unit_list_error}"
            "${_sto_tpcc_unit_list_output}")
    endif()

    string(REPLACE "\r\n" "\n" _sto_tpcc_unit_list_output
        "${_sto_tpcc_unit_list_output}")
    string(REPLACE "\n" ";" _sto_tpcc_unit_list_lines
        "${_sto_tpcc_unit_list_output}")
    set(_sto_tpcc_unit_tests "")
    foreach(_sto_tpcc_unit_line IN LISTS _sto_tpcc_unit_list_lines)
        string(STRIP "${_sto_tpcc_unit_line}" _sto_tpcc_unit_line)
        if(_sto_tpcc_unit_line MATCHES "^(.+): test$")
            list(APPEND _sto_tpcc_unit_tests "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(NOT _sto_tpcc_unit_tests)
        message(FATAL_ERROR
            "Cargo listed no Rust STO TPC-C FFI unit tests:\n"
            "${_sto_tpcc_unit_list_output}")
    endif()

    foreach(_sto_tpcc_unit_test IN LISTS _sto_tpcc_unit_tests)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env ${_native_environment}
                "${MAKO_CARGO_EXECUTABLE}" test
                --manifest-path "${MAKO_RUST_MANIFEST}"
                --locked
                -p sto-tpcc-ffi
                --lib
                "${_sto_tpcc_unit_test}"
                --
                --exact
                --test-threads=1
            COMMAND_ECHO STDOUT
            RESULT_VARIABLE _sto_tpcc_unit_result
        )
        if(NOT _sto_tpcc_unit_result EQUAL 0)
            message(FATAL_ERROR
                "Rust STO TPC-C FFI unit test ${_sto_tpcc_unit_test} failed "
                "with exit code ${_sto_tpcc_unit_result}")
        endif()
    endforeach()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${_native_environment}
            "${MAKO_CARGO_EXECUTABLE}" test
            --manifest-path "${MAKO_RUST_MANIFEST}"
            --locked
            -p sto-tpcc-ffi
            --test native_ffi
        COMMAND_ECHO STDOUT
        RESULT_VARIABLE _sto_tpcc_result
    )
    if(NOT _sto_tpcc_result EQUAL 0)
        message(FATAL_ERROR
            "Rust STO TPC-C FFI integration failed with exit code ${_sto_tpcc_result}")
    endif()

    get_filename_component(_sto_tpcc_test_dir
        "${MAKO_STO_TPCC_NATIVE_TEST}" DIRECTORY)
    set(_sto_tpcc_trusted_test "${_sto_tpcc_test_dir}/trusted_ffi.rs")
    if(NOT EXISTS "${_sto_tpcc_trusted_test}")
        message(FATAL_ERROR
            "Rust STO TPC-C trusted integration test does not exist: ${_sto_tpcc_trusted_test}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${_native_environment}
            "${MAKO_CARGO_EXECUTABLE}" test
            --manifest-path "${MAKO_RUST_MANIFEST}"
            --locked
            -p sto-tpcc-ffi
            --test trusted_ffi
        COMMAND_ECHO STDOUT
        RESULT_VARIABLE _sto_tpcc_trusted_result
    )
    if(NOT _sto_tpcc_trusted_result EQUAL 0)
        message(FATAL_ERROR
            "Rust STO TPC-C trusted FFI integration failed with exit code ${_sto_tpcc_trusted_result}")
    endif()

    # Run each endpoint in its own process. The native Masstree runtime has a
    # process-wide lifecycle, and isolation also proves each private fused
    # transaction boundary from a clean runtime state.
    foreach(_sto_tpcc_payment_test_name IN ITEMS payment_prefix payment_full new_order_full delivery_full stock_level_full)
        set(_sto_tpcc_payment_test
            "${_sto_tpcc_test_dir}/${_sto_tpcc_payment_test_name}.rs")
        if(NOT EXISTS "${_sto_tpcc_payment_test}")
            message(FATAL_ERROR
                "Rust STO TPC-C ${_sto_tpcc_payment_test_name} integration test does not exist: ${_sto_tpcc_payment_test}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env ${_native_environment}
                "${MAKO_CARGO_EXECUTABLE}" test
                --manifest-path "${MAKO_RUST_MANIFEST}"
                --locked
                -p sto-tpcc-ffi
                --test "${_sto_tpcc_payment_test_name}"
            COMMAND_ECHO STDOUT
            RESULT_VARIABLE _sto_tpcc_payment_result
        )
        if(NOT _sto_tpcc_payment_result EQUAL 0)
            message(FATAL_ERROR
                "Rust STO TPC-C ${_sto_tpcc_payment_test_name} integration failed with exit code ${_sto_tpcc_payment_result}")
        endif()
    endforeach()
endif()
