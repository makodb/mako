cmake_minimum_required(VERSION 3.30)

foreach(_required IN ITEMS
        MAKO_CARGO_EXECUTABLE
        MAKO_RUST_MANIFEST
        MAKO_CARGO_TARGET_DIR
        MAKO_ARCHIVE
        MASSTREE_ARCHIVE
        MAKO_CXX_RUNTIME_DIR
        MAKO_LOADER_PATH_VARIABLE
        MAKO_NATIVE_LIBS
        MAKO_MASSTREE_NATIVE_TEST
        MAKO_STO_NATIVE_TEST
        MAKO_STO_HISTORY_TEST
        MAKO_STO_TPCC_NATIVE_TEST)
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
foreach(_required_test IN ITEMS
        "${MAKO_MASSTREE_NATIVE_TEST}"
        "${MAKO_STO_NATIVE_TEST}"
        "${MAKO_STO_HISTORY_TEST}"
        "${MAKO_STO_TPCC_NATIVE_TEST}")
    if(NOT EXISTS "${_required_test}")
        message(FATAL_ERROR
            "Required Rust STO native integration suite does not exist: "
            "${_required_test}")
    endif()
endforeach()

get_filename_component(_rust_workspace_dir "${MAKO_RUST_MANIFEST}" DIRECTORY)
set(_expected_MAKO_MASSTREE_NATIVE_TEST
    "${_rust_workspace_dir}/masstree/tests/native_integration.rs")
set(_expected_MAKO_STO_NATIVE_TEST
    "${_rust_workspace_dir}/sto-masstree/tests/native_integration.rs")
set(_expected_MAKO_STO_HISTORY_TEST
    "${_rust_workspace_dir}/sto-masstree/tests/history_oracle.rs")
set(_expected_MAKO_STO_TPCC_NATIVE_TEST
    "${_rust_workspace_dir}/sto-tpcc-ffi/tests/native_ffi.rs")
foreach(_required_test_variable IN ITEMS
        MAKO_MASSTREE_NATIVE_TEST
        MAKO_STO_NATIVE_TEST
        MAKO_STO_HISTORY_TEST
        MAKO_STO_TPCC_NATIVE_TEST)
    cmake_path(ABSOLUTE_PATH ${_required_test_variable}
        NORMALIZE OUTPUT_VARIABLE _required_test_path)
    cmake_path(ABSOLUTE_PATH _expected_${_required_test_variable}
        NORMALIZE OUTPUT_VARIABLE _expected_test_path)
    if(NOT _required_test_path STREQUAL _expected_test_path)
        message(FATAL_ERROR
            "${_required_test_variable} does not select its authoritative "
            "workspace suite: ${_required_test_path}; expected "
            "${_expected_test_path}")
    endif()
endforeach()

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

function(_mako_assert_exact_test_inventory label list_output expected_variable)
    string(REPLACE "\r\n" "\n" _list_output "${list_output}")
    string(REPLACE "\n" ";" _list_lines "${_list_output}")
    set(_actual_tests "")
    foreach(_list_line IN LISTS _list_lines)
        string(STRIP "${_list_line}" _list_line)
        if(_list_line MATCHES "^(.+): test$")
            list(APPEND _actual_tests "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(NOT _actual_tests)
        message(FATAL_ERROR "Cargo listed no tests for ${label}:\n${list_output}")
    endif()

    list(LENGTH _actual_tests _actual_count)
    set(_unique_actual_tests ${_actual_tests})
    list(REMOVE_DUPLICATES _unique_actual_tests)
    list(LENGTH _unique_actual_tests _unique_actual_count)
    if(NOT _actual_count EQUAL _unique_actual_count)
        message(FATAL_ERROR "Cargo listed duplicate test names for ${label}")
    endif()

    set(_expected_tests ${${expected_variable}})
    list(SORT _actual_tests)
    list(SORT _expected_tests)
    if(NOT "${_actual_tests}" STREQUAL "${_expected_tests}")
        set(_missing_tests ${_expected_tests})
        foreach(_actual_test IN LISTS _actual_tests)
            list(REMOVE_ITEM _missing_tests "${_actual_test}")
        endforeach()
        set(_unexpected_tests ${_actual_tests})
        foreach(_expected_test IN LISTS _expected_tests)
            list(REMOVE_ITEM _unexpected_tests "${_expected_test}")
        endforeach()
        message(FATAL_ERROR
            "${label} test inventory changed. Missing: ${_missing_tests}; "
            "unexpected: ${_unexpected_tests}")
    endif()
    list(LENGTH _expected_tests _expected_count)
    message(STATUS "Verified ${label} inventory: ${_expected_count} tests")
endfunction()

function(_mako_run_native_suite)
    set(_options ALL_FEATURES)
    set(_one_value_args LABEL PACKAGE TARGET EXPECTED_VARIABLE)
    cmake_parse_arguments(SUITE "${_options}" "${_one_value_args}" "" ${ARGN})
    if(SUITE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unexpected ${SUITE_LABEL} runner arguments: ${SUITE_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_argument IN ITEMS LABEL PACKAGE TARGET EXPECTED_VARIABLE)
        if(NOT DEFINED SUITE_${_argument} OR "${SUITE_${_argument}}" STREQUAL "")
            message(FATAL_ERROR "Native suite runner is missing ${_argument}")
        endif()
    endforeach()

    set(_suite_args
        test
        --manifest-path "${MAKO_RUST_MANIFEST}"
        --locked
        ${_cargo_platform_args}
        -p "${SUITE_PACKAGE}")
    if(SUITE_ALL_FEATURES)
        list(APPEND _suite_args --all-features)
    endif()
    list(APPEND _suite_args --test "${SUITE_TARGET}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${_native_environment}
            "${MAKO_CARGO_EXECUTABLE}" ${_suite_args}
            -- --list --format terse
        COMMAND_ECHO STDOUT
        RESULT_VARIABLE _list_result
        OUTPUT_VARIABLE _list_output
        ERROR_VARIABLE _list_error)
    if(NOT _list_result EQUAL 0)
        message(FATAL_ERROR
            "Could not list ${SUITE_LABEL} tests (exit code ${_list_result}):\n"
            "${_list_error}${_list_output}")
    endif()
    _mako_assert_exact_test_inventory(
        "${SUITE_LABEL}" "${_list_output}" "${SUITE_EXPECTED_VARIABLE}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${_native_environment}
            "${MAKO_CARGO_EXECUTABLE}" ${_suite_args}
            -- --include-ignored
        COMMAND_ECHO STDOUT
        RESULT_VARIABLE _suite_result)
    if(NOT _suite_result EQUAL 0)
        message(FATAL_ERROR
            "${SUITE_LABEL} failed with exit code ${_suite_result}")
    endif()
endfunction()

set(_masstree_native_expected_tests
    negotiated_point_directory_round_trip_and_cross_worker_read
    one_shot_fixed_reads_reuse_results_and_end_native_guards
    trusted_fixed_insert_batch_preserves_order_and_reuses_scratch
    structure_seal_keeps_reads_open_and_rejects_every_publication_lane
    copied_scan_bounds_directions_and_resumption_round_trip
    trusted_bounded_record_id_scan_keeps_bounds_and_inclusive_next_continuation
    scoped_reads_end_before_insert_and_drop_during_unwind
    worker_rcu_scope_spans_trees_and_drops_during_unwind)
set(_sto_masstree_native_expected_tests
    native_directory_seal_preserves_records_and_rejects_scalar_and_fixed_misses
    native_point_commit_read_and_abort_round_trip
    native_scalar_borrowed_modify_resolves_and_reuses_the_record_token
    private_direct_tree_tokens_survive_segment_growth_batches_and_wrong_table_rejection
    supplied_tree_constructor_keeps_the_public_record_id_lane
    fixed_u64_public_native_loader_mutation_and_terminal_read_round_trip
    eager_contiguous_registry_native_read_write_round_trip
    point_session_closes_on_miss_and_scan_and_reopens_after_each_boundary
    fixed_point_batch_handles_binary_hits_misses_and_reuses_storage
    fixed_resolving_visitors_return_reusable_tokens_and_preserve_duplicates
    dense_resolved_cache_checks_bounds_identity_publication_and_lifetime
    hinted_fixed_batches_preserve_order_mixes_duplicates_and_table_identity
    fixed_expected_absent_batch_reuses_scratch_and_preserves_duplicates
    fixed_point_batch_capacity_error_dooms_and_clears_results
    private_direct_native_scan_handles_chunks_bounds_binary_keys_and_tombstones
    private_keyless_value_scan_accepts_32_33_and_300_but_rejects_301
    private_direct_native_scan_stop_excludes_the_copied_suffix
    native_transactional_scan_resumes_and_rejects_a_phantom)
set(_sto_masstree_history_expected_tests
    native_masstree_histories_are_strictly_serializable)

_mako_run_native_suite(
    LABEL "Rust Masstree native integration"
    PACKAGE masstree
    TARGET native_integration
    EXPECTED_VARIABLE _masstree_native_expected_tests)

# Run each transactional-adapter suite in its own process. Native runtime and
# worker registrations have process-wide lifetimes, so process isolation keeps
# one suite's registrations out of the next suite's fixed worker budget.
_mako_run_native_suite(
    LABEL "Rust STO Masstree native integration"
    PACKAGE sto-masstree
    TARGET native_integration
    EXPECTED_VARIABLE _sto_masstree_native_expected_tests
    ALL_FEATURES)
_mako_run_native_suite(
    LABEL "Rust STO Masstree history oracle"
    PACKAGE sto-masstree
    TARGET history_oracle
    EXPECTED_VARIABLE _sto_masstree_history_expected_tests
    ALL_FEATURES)

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
    set(_sto_tpcc_unit_expected_tests
        tests::checked_get_zeroes_actual_before_rejecting_an_invalid_handle
        tests::dense_item_and_stock_paths_warm_mix_and_disable_on_warehouse_mismatch
        tests::fixed_key_views_require_exact_nonoverflowing_storage
        tests::fixed_mutation_access_error_reports_only_the_delivered_prefix
        tests::fixed_mutation_preserves_actions_duplicates_size_and_failure_boundaries
        tests::fixed_put_supports_all_widths_mixed_rows_duplicates_abort_and_scratch_reuse
        tests::fixed_read_visits_all_widths_and_aborts_on_callback_failure
        tests::insert_many_accounting_failure_aborts_staged_transaction
        tests::last_error_copy_rejects_overlapping_outputs_before_writing
        tests::last_error_copy_validates_scalar_and_output_ranges
        tests::last_error_is_bounded_and_utf8
        tests::last_only_lane_does_not_grow_the_inline_cache_object
        tests::last_only_policy_isolates_its_fixed_lane_from_full_cache
        tests::logical_row_adjustment_preserves_value_on_overflow_and_underflow
        tests::new_order_codecs_preserve_stock_tail_and_value_layouts
        tests::new_order_stock_tokens_drive_stock_level_hit_and_compact_miss_paths
        tests::payment_customer_codec_matches_field_order_and_grows_varints
        tests::payment_customer_codec_rejects_truncation_malformed_values_and_overflow
        tests::payment_name_selection_uses_lower_median_through_the_cpp_limit
        tests::payment_output_ranges_detect_overlap_without_rejecting_adjacency
        tests::payment_private_abi_layout_is_stable
        tests::payment_tail_codecs_match_mako_packed_layouts
        tests::payment_zigzag_varints_cover_boundaries_and_reject_malformed_forms
        tests::post_install_row_count_failure_marks_runtime_indeterminate
        tests::public_and_trusted_byte_endpoints_reject_impossible_ranges
        tests::raw_slice_views_reject_impossible_ranges_before_dereference
        tests::resolved_cache_hash_distributes_big_endian_customer_ids
        tests::resolved_cache_reuses_point_misses_and_only_small_scan_rows
        tests::row_count_is_published_before_conflicting_writer_can_commit
        tests::shared_ffi_boundary_contains_panics
        tests::stable_resolved_cache_policies_match_header
        tests::stable_status_numbers_match_header
        tests::thread_affinity_cookie_is_stable_and_distinct
        tests::tpcc_table_config_c_layout_appends_the_bounded_value_flag
        tests::tpcc_table_configuration_always_selects_unique_lock_requests)
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
    _mako_assert_exact_test_inventory(
        "Rust STO TPC-C FFI unit"
        "${_sto_tpcc_unit_list_output}"
        _sto_tpcc_unit_expected_tests)

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
                --include-ignored
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

    set(_sto_tpcc_native_expected_tests
        bounded_atomic_value_config_round_trips_cell_boundaries_through_the_c_abi
        sealed_directory_keeps_existing_rows_mutable_and_rejects_new_keys
        fixed_mutation_batches_preserve_duplicate_order_size_and_abort_recovery
        fixed_put_packs_variable_values_and_reports_sequential_duplicates
        heterogeneous_insert_many_preserves_order_duplicates_and_validation_atomicity
        ffi_crud_scan_read_your_writes_and_cross_table_atomicity
        borrowed_point_and_scan_bytes_preserve_ffi_results_and_read_your_writes
        streaming_scan_counts_rows_delivered_before_a_later_error_and_reuses_thread_scratch
        additive_cache_policy_creator_preserves_crud_and_rejects_unknown_values)
    _mako_run_native_suite(
        LABEL "Rust STO TPC-C FFI integration"
        PACKAGE sto-tpcc-ffi
        TARGET native_ffi
        EXPECTED_VARIABLE _sto_tpcc_native_expected_tests)

    get_filename_component(_sto_tpcc_test_dir
        "${MAKO_STO_TPCC_NATIVE_TEST}" DIRECTORY)
    set(_sto_tpcc_trusted_test "${_sto_tpcc_test_dir}/trusted_ffi.rs")
    if(NOT EXISTS "${_sto_tpcc_trusted_test}")
        message(FATAL_ERROR
            "Rust STO TPC-C trusted integration test does not exist: ${_sto_tpcc_trusted_test}")
    endif()
    set(_sto_tpcc_trusted_ffi_expected_tests
        trusted_endpoints_share_public_transaction_semantics)
    _mako_run_native_suite(
        LABEL "Rust STO TPC-C trusted FFI integration"
        PACKAGE sto-tpcc-ffi
        TARGET trusted_ffi
        EXPECTED_VARIABLE _sto_tpcc_trusted_ffi_expected_tests)

    # Run each endpoint in its own process. The native Masstree runtime has a
    # process-wide lifecycle, and isolation also proves each private fused
    # transaction boundary from a clean runtime state.
    set(_sto_tpcc_payment_prefix_expected_tests
        payment_prefix_commits_aborts_scans_and_fails_atomically)
    set(_sto_tpcc_payment_full_expected_tests
        full_payment_commits_gc_bc_and_duplicate_history_as_a_noop)
    set(_sto_tpcc_new_order_full_expected_tests
        full_new_order_commits_exact_bytes_and_rolls_back_collisions)
    set(_sto_tpcc_delivery_full_expected_tests
        full_delivery_preserves_scalar_empty_zero_line_and_rollback_semantics)
    set(_sto_tpcc_stock_level_full_expected_tests
        full_stock_level_matches_scalar_scan_dedup_threshold_and_failure_semantics)
    foreach(_sto_tpcc_payment_test_name IN ITEMS
            payment_prefix payment_full new_order_full delivery_full stock_level_full)
        set(_sto_tpcc_payment_test
            "${_sto_tpcc_test_dir}/${_sto_tpcc_payment_test_name}.rs")
        if(NOT EXISTS "${_sto_tpcc_payment_test}")
            message(FATAL_ERROR
                "Rust STO TPC-C ${_sto_tpcc_payment_test_name} integration test does not exist: ${_sto_tpcc_payment_test}")
        endif()
        _mako_run_native_suite(
            LABEL "Rust STO TPC-C ${_sto_tpcc_payment_test_name} integration"
            PACKAGE sto-tpcc-ffi
            TARGET "${_sto_tpcc_payment_test_name}"
            EXPECTED_VARIABLE
                "_sto_tpcc_${_sto_tpcc_payment_test_name}_expected_tests")
    endforeach()
endif()
