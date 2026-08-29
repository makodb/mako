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

# Keep the lower integration target forward-compatible with the transactional
# adapter without making a not-yet-present test an error. Reconfiguring after
# that test file is added automatically enables this second command.
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
            --test native_integration
        COMMAND_ECHO STDOUT
        RESULT_VARIABLE _sto_masstree_result
    )
    if(NOT _sto_masstree_result EQUAL 0)
        message(FATAL_ERROR
            "Rust STO Masstree native integration failed with exit code ${_sto_masstree_result}")
    endif()
endif()

if(DEFINED MAKO_STO_TPCC_NATIVE_TEST
        AND NOT "${MAKO_STO_TPCC_NATIVE_TEST}" STREQUAL "")
    if(NOT EXISTS "${MAKO_STO_TPCC_NATIVE_TEST}")
        message(FATAL_ERROR
            "Configured sto-tpcc-ffi native test does not exist: ${MAKO_STO_TPCC_NATIVE_TEST}")
    endif()
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
endif()
