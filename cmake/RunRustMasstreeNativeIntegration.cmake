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
if(DEFINED MAKO_RUSTFLAGS AND NOT "${MAKO_RUSTFLAGS}" STREQUAL "")
    list(APPEND _native_environment "RUSTFLAGS=${MAKO_RUSTFLAGS}")
endif()
if(DEFINED MAKO_RUSTDOCFLAGS AND NOT "${MAKO_RUSTDOCFLAGS}" STREQUAL "")
    list(APPEND _native_environment "RUSTDOCFLAGS=${MAKO_RUSTDOCFLAGS}")
endif()
if(DEFINED MAKO_RUSTUP_TOOLCHAIN
        AND NOT "${MAKO_RUSTUP_TOOLCHAIN}" STREQUAL "")
    list(APPEND _native_environment
        "RUSTUP_TOOLCHAIN=${MAKO_RUSTUP_TOOLCHAIN}")
endif()
if(DEFINED MAKO_RUST_LINKER_ENV_NAME
        AND NOT "${MAKO_RUST_LINKER_ENV_NAME}" STREQUAL "")
    if(NOT MAKO_RUST_LINKER_ENV_NAME MATCHES
            "^CARGO_TARGET_[A-Z0-9_]+_LINKER$")
        message(FATAL_ERROR
            "Invalid Cargo target-linker environment name: "
            "${MAKO_RUST_LINKER_ENV_NAME}")
    endif()
    if(NOT DEFINED MAKO_RUST_LINKER OR "${MAKO_RUST_LINKER}" STREQUAL ""
            OR NOT EXISTS "${MAKO_RUST_LINKER}")
        message(FATAL_ERROR
            "Rust native integration requires the configured Rust linker")
    endif()
    list(APPEND _native_environment
        "${MAKO_RUST_LINKER_ENV_NAME}=${MAKO_RUST_LINKER}")
endif()
if(DEFINED MAKO_ASAN_OPTIONS AND NOT "${MAKO_ASAN_OPTIONS}" STREQUAL "")
    list(APPEND _native_environment "ASAN_OPTIONS=${MAKO_ASAN_OPTIONS}")
endif()

set(_asan_native_quarantine_tests "")
set(_asan_native_quarantine_environment "")
if(DEFINED MAKO_ASAN_NATIVE_QUARANTINE_TESTS
        AND NOT "${MAKO_ASAN_NATIVE_QUARANTINE_TESTS}" STREQUAL "")
    if(NOT DEFINED MAKO_RUSTFLAGS
            OR NOT MAKO_RUSTFLAGS MATCHES
                "(^|[ \t])-Zsanitizer=address($|[ \t])")
        message(FATAL_ERROR
            "Native ASan quarantine tests require Rust address-sanitizer instrumentation")
    endif()
    if(NOT DEFINED MAKO_ASAN_OPTIONS
            OR NOT MAKO_ASAN_OPTIONS MATCHES
                "(^|:)detect_leaks=1(:|$)")
        message(FATAL_ERROR
            "Native ASan quarantine tests require ASAN_OPTIONS with detect_leaks=1")
    endif()
    foreach(_asan_native_quarantine_test IN LISTS
            MAKO_ASAN_NATIVE_QUARANTINE_TESTS)
        if(NOT _asan_native_quarantine_test MATCHES "^[A-Za-z0-9_.:-]+$")
            message(FATAL_ERROR
                "Invalid native ASan quarantine test name: "
                "${_asan_native_quarantine_test}")
        endif()
        list(FIND _asan_native_quarantine_tests
            "${_asan_native_quarantine_test}" _asan_duplicate_index)
        if(NOT _asan_duplicate_index EQUAL -1)
            message(FATAL_ERROR
                "Duplicate native ASan quarantine test: "
                "${_asan_native_quarantine_test}")
        endif()
        list(APPEND _asan_native_quarantine_tests
            "${_asan_native_quarantine_test}")
    endforeach()
    string(REPLACE "detect_leaks=1" "detect_leaks=0"
        _asan_native_quarantine_options "${MAKO_ASAN_OPTIONS}")
    set(_asan_native_quarantine_environment ${_native_environment})
    list(FILTER _asan_native_quarantine_environment EXCLUDE
        REGEX "^ASAN_OPTIONS=")
    list(APPEND _asan_native_quarantine_environment
        "ASAN_OPTIONS=${_asan_native_quarantine_options}")
endif()

set(_cargo_platform_args "")
if(DEFINED MAKO_RUST_TARGET_TRIPLE
        AND NOT "${MAKO_RUST_TARGET_TRIPLE}" STREQUAL "")
    list(APPEND _cargo_platform_args --target "${MAKO_RUST_TARGET_TRIPLE}")
endif()
if(DEFINED MAKO_RUST_BUILD_STD AND MAKO_RUST_BUILD_STD)
    if(NOT DEFINED MAKO_RUST_TARGET_TRIPLE
            OR "${MAKO_RUST_TARGET_TRIPLE}" STREQUAL "")
        message(FATAL_ERROR
            "MAKO_RUST_BUILD_STD requires MAKO_RUST_TARGET_TRIPLE")
    endif()
    list(APPEND _cargo_platform_args -Zbuild-std)
endif()

message(STATUS "Mako archive: ${MAKO_ARCHIVE}")
message(STATUS "Masstree archive: ${MASSTREE_ARCHIVE}")
message(STATUS "Rust native libraries: ${MAKO_NATIVE_LIBS}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${_native_environment}
        "${MAKO_CARGO_EXECUTABLE}" test
        --manifest-path "${MAKO_RUST_MANIFEST}"
        --locked
        ${_cargo_platform_args}
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
            ${_cargo_platform_args}
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
            ${_cargo_platform_args}
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
            ${_cargo_platform_args}
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

    foreach(_asan_native_quarantine_test IN LISTS
            _asan_native_quarantine_tests)
        list(FIND _sto_tpcc_unit_tests "${_asan_native_quarantine_test}"
            _asan_native_quarantine_index)
        if(_asan_native_quarantine_index EQUAL -1)
            message(FATAL_ERROR
                "Native ASan quarantine test was not listed by Cargo: "
                "${_asan_native_quarantine_test}")
        endif()
    endforeach()

    foreach(_sto_tpcc_unit_test IN LISTS _sto_tpcc_unit_tests)
        set(_sto_tpcc_unit_environment ${_native_environment})
        list(FIND _asan_native_quarantine_tests "${_sto_tpcc_unit_test}"
            _asan_native_quarantine_index)
        if(NOT _asan_native_quarantine_index EQUAL -1)
            set(_sto_tpcc_unit_environment
                ${_asan_native_quarantine_environment})
            message(STATUS
                "Running intentional-quarantine native ASan test with leak "
                "reporting disabled: ${_sto_tpcc_unit_test}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env ${_sto_tpcc_unit_environment}
                "${MAKO_CARGO_EXECUTABLE}" test
                --manifest-path "${MAKO_RUST_MANIFEST}"
                --locked
                ${_cargo_platform_args}
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
            ${_cargo_platform_args}
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
            ${_cargo_platform_args}
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
                ${_cargo_platform_args}
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
