# The index transaction adapter and its owned Rust/native ABI wrapper.
# NativeStorage provides only the index archive and explicit linkage settings.
set(_masstree_tx_default)
if(MAKO_BUILD_PROFILE STREQUAL "MASSTREE_TX")
    set(_masstree_tx_default ALL)
endif()
add_custom_target(masstree_tx_build ${_masstree_tx_default}
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/check_masstree_tx_component.py"
        --cargo "${MAKO_NATIVE_CARGO}" ${_native_profile_flags}
        --output "${CMAKE_BINARY_DIR}/masstree-tx-dependencies.json"
    DEPENDS native_storage_source_audit
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)

# Include the optional fixed-u64 cases as well as the general byte-key adapter.
# Cases that poison process-global native state must each get a fresh process.
add_custom_target(masstree_tx_tests
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s scripts/tests -p test_masstree_tx_component.py
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_native_rust_tests.py"
        --cargo "${MAKO_NATIVE_CARGO}" --manifest "${_cargo_manifest}"
        --package sto-masstree --package sto-tpcc-ffi
        --features sto-masstree/fixed-u64 ${_native_profile_flags}
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${MAKO_NATIVE_CARGO}" test --locked --manifest-path "${_cargo_manifest}"
        -p sto-masstree -p sto-tpcc-ffi --features sto-masstree/fixed-u64
        ${_native_profile_flags} --doc
    DEPENDS masstree_tx_build
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)

# Preserve the wrapper's existing strict lint gate under its component owner.
add_custom_target(masstree_tx_quality
    COMMAND "${CMAKE_COMMAND}" -E env ${MAKO_NATIVE_CARGO_ENV}
        "${MAKO_NATIVE_CARGO}" clippy --locked --manifest-path "${_cargo_manifest}"
        -p sto-tpcc-ffi --all-targets --no-deps -- -D warnings
    DEPENDS masstree_tx_build
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" USES_TERMINAL VERBATIM)

enable_testing()
add_test(NAME masstree_transaction_component
    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target masstree_tx_tests)
set_tests_properties(masstree_transaction_component PROPERTIES LABELS "masstree-tx" TIMEOUT 900)
