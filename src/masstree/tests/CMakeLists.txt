# Masstree test & benchmark target wiring.
#
# This subdirectory is added by the root CMakeLists *inside* the
# `if(BUILD_TESTS)` block, so by the time we get here:
#   * libmasstree, libmako, and the gtest helper libraries exist;
#   * TEST_INCLUDE_DIRS / GTEST_INCLUDE_DIRS / TEST_COMPILE_OPTIONS /
#     TEST_LINK_LIBS / GTEST_LIBRARIES / GTEST_MAIN_LIBRARIES are set;
#   * MASSTREE_FORCE_INCLUDE_FLAG is set.
#
# Each test links `mako` (which transitively pulls in `masstree`).
# Tests don't need to repeat the masstree include path because libmako
# re-exports it via target_link_libraries(mako PUBLIC masstree).

set(MASSTREE_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(MASSTREE_SRC_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Keep test binaries at the build root (build_local/, build_asan/, ...)
# rather than the CMake default of build_local/src/masstree/tests/.
# Docs, CI scripts, and the perf-baseline workflow all reference the
# build-root path; preserve it after the layout move.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ----------------------------------------------------------------------
# Helper for the upstream-style "legacy" tests carried over from the
# original Masstree GNUmakefile. They build a single .cc against the
# minimal set of masstree source files it needs.
# ----------------------------------------------------------------------
function(_mako_add_masstree_legacy_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${MASSTREE_SRC_DIR}
    )
    target_compile_options(${TARGET_NAME} PRIVATE
        ${TEST_COMPILE_OPTIONS}
        ${MASSTREE_FORCE_INCLUDE_FLAG}
    )
    target_link_libraries(${TARGET_NAME} pthread)
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
    set_tests_properties(${TARGET_NAME} PROPERTIES
        LABELS "masstree;legacy"
        TIMEOUT 60
    )
endfunction()

# ----------------------------------------------------------------------
# gtest-based regression suites.
# ----------------------------------------------------------------------

# Core b-tree regression tests (Tier 1).
add_executable(test_masstree ${MASSTREE_TEST_DIR}/test_masstree.cc)
target_include_directories(test_masstree PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
target_compile_options(test_masstree PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_masstree mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
add_test(NAME test_masstree COMMAND test_masstree)

# Property-based test against std::map oracle (Tier 2).
add_executable(test_masstree_property ${MASSTREE_TEST_DIR}/test_masstree_property.cc)
target_include_directories(test_masstree_property PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
target_compile_options(test_masstree_property PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_masstree_property mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
add_test(NAME test_masstree_property COMMAND test_masstree_property)

# Concurrent stress tests on a shared concurrent_btree (Tier 3.3-3.5).
add_executable(test_masstree_concurrent ${MASSTREE_TEST_DIR}/test_masstree_concurrent.cc)
target_include_directories(test_masstree_concurrent PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
target_compile_options(test_masstree_concurrent PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_masstree_concurrent mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
add_test(NAME test_masstree_concurrent COMMAND test_masstree_concurrent)
set_tests_properties(test_masstree_concurrent PROPERTIES TIMEOUT 60)

# Per-key linearizability check (Tier 3.2).
add_executable(test_masstree_linearizability ${MASSTREE_TEST_DIR}/test_masstree_linearizability.cc)
target_include_directories(test_masstree_linearizability PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
target_compile_options(test_masstree_linearizability PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_masstree_linearizability mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
add_test(NAME test_masstree_linearizability COMMAND test_masstree_linearizability)
set_tests_properties(test_masstree_linearizability PROPERTIES TIMEOUT 60)

# Memory & RCU correctness probes — under ASan/LSan these become real
# leak/UAF detectors; under a normal build they pass as functional
# checks (Tier 4).
add_executable(test_masstree_memory ${MASSTREE_TEST_DIR}/test_masstree_memory.cc)
target_include_directories(test_masstree_memory PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
target_compile_options(test_masstree_memory PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_masstree_memory mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
add_test(NAME test_masstree_memory COMMAND test_masstree_memory)
set_tests_properties(test_masstree_memory PROPERTIES TIMEOUT 60)

# Layered / slice-boundary edge cases (Tier 7). Layer-collapse test
# doubles as an LSan probe for missed layer frees.
add_executable(test_masstree_layered ${MASSTREE_TEST_DIR}/test_masstree_layered.cc)
target_include_directories(test_masstree_layered PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
target_compile_options(test_masstree_layered PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_masstree_layered mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
add_test(NAME test_masstree_layered COMMAND test_masstree_layered)

# Long-running soak (Tier 8). Default 30 s; set MASSTREE_SOAK_SECONDS=N
# for longer runs (clamped to 24 h in source). CTest TIMEOUT must
# exceed the soak length; bump it before extending the default.
add_executable(test_masstree_soak ${MASSTREE_TEST_DIR}/test_masstree_soak.cc)
target_include_directories(test_masstree_soak PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
target_compile_options(test_masstree_soak PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_masstree_soak mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
add_test(NAME test_masstree_soak COMMAND test_masstree_soak)
set_tests_properties(test_masstree_soak PROPERTIES TIMEOUT 120)

# Header self-containment sentinel (Tier 9). Build is the test.
add_executable(test_masstree_headers ${MASSTREE_TEST_DIR}/test_masstree_headers.cc)
target_include_directories(test_masstree_headers PRIVATE ${TEST_INCLUDE_DIRS})
target_compile_options(test_masstree_headers PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_masstree_headers mako ${TEST_LINK_LIBS} pthread)
add_test(NAME test_masstree_headers COMMAND test_masstree_headers)

# libFuzzer differential harness — gated on MAKO_FUZZER=1 (Tier 6).
# Not wired into ctest; intended to be invoked by hand or via OSS-Fuzz.
# See docs/masstree-test-plan.md for the toolchain requirements (a
# libc++-built compiler-rt fuzzer runtime; on Debian/Ubuntu use brew
# clang 22).
if(MAKO_FUZZER_ENABLED)
    add_executable(fuzz_masstree ${MASSTREE_TEST_DIR}/fuzz_masstree.cc)
    target_include_directories(fuzz_masstree PRIVATE ${TEST_INCLUDE_DIRS})
    target_compile_options(fuzz_masstree PRIVATE ${TEST_COMPILE_OPTIONS} -fsanitize=fuzzer)
    target_link_libraries(fuzz_masstree mako ${TEST_LINK_LIBS} yaml-cpp pthread)
    target_link_options(fuzz_masstree PRIVATE -fsanitize=fuzzer)
endif()

# Internals tests (threadinfo, globalepoch, RCU).
add_executable(test_masstree_internals ${MASSTREE_TEST_DIR}/test_masstree_internals.cc)
target_include_directories(test_masstree_internals PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
target_compile_options(test_masstree_internals PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_masstree_internals mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
add_test(NAME test_masstree_internals COMMAND test_masstree_internals)

# MasstreeContext multi-instance isolation stress.
add_executable(test_masstree_multi_instance ${MASSTREE_TEST_DIR}/test_masstree_multi_instance.cc)
target_include_directories(test_masstree_multi_instance PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
target_compile_options(test_masstree_multi_instance PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_masstree_multi_instance mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
add_test(NAME test_masstree_multi_instance COMMAND test_masstree_multi_instance)

# SiloRuntime per-site isolation.
add_executable(test_silo_runtime ${MASSTREE_TEST_DIR}/test_silo_runtime.cc)
target_include_directories(test_silo_runtime PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
target_compile_options(test_silo_runtime PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_silo_runtime mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
add_test(NAME test_silo_runtime COMMAND test_silo_runtime)

# Multi-site Silo stress.
add_executable(test_silo_multi_site_stress ${MASSTREE_TEST_DIR}/test_silo_multi_site_stress.cc)
target_include_directories(test_silo_multi_site_stress PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
target_compile_options(test_silo_multi_site_stress PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_silo_multi_site_stress mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
add_test(NAME test_silo_multi_site_stress COMMAND test_silo_multi_site_stress)

# Borrow checking for Silo tests
# Note: compare_key and increment functions now have correct @unsafe annotations.
# Full borrow checking is disabled because other Masstree code (nodeversion.hh,
# string.hh) has violations that require broader refactoring.
# if(ENABLE_BORROW_CHECKING)
#     add_borrow_check_target(test_silo_runtime)
#     add_borrow_check_target(test_silo_multi_site_stress)
# endif()

# ----------------------------------------------------------------------
# On-demand performance benchmark.
# ----------------------------------------------------------------------
set(MASSTREE_PERF_SRC "${MASSTREE_TEST_DIR}/masstree_perf.cc")
if(MAKO_CXX_COMPILER_BASENAME STREQUAL "fragilec")
    # Use a fragile-friendly source variant to keep benchmark logic transpilable.
    if(EXISTS "${MASSTREE_TEST_DIR}/masstree_perf_fragile.cc")
        set(MASSTREE_PERF_SRC "${MASSTREE_TEST_DIR}/masstree_perf_fragile.cc")
    endif()
endif()
add_executable(masstree_perf ${MASSTREE_PERF_SRC})
target_include_directories(masstree_perf PRIVATE ${TEST_INCLUDE_DIRS})
target_compile_options(masstree_perf PRIVATE ${TEST_COMPILE_OPTIONS})
if(MAKO_CXX_COMPILER_BASENAME STREQUAL "fragilec")
    # Keep strict fragile benchmark builds focused on Masstree-only code paths.
    # Linking the full `mako` library pulls in unrelated RPC translation units
    # that are not required for this benchmark and currently block compilation.
    target_link_libraries(masstree_perf pthread)
else()
    target_link_libraries(masstree_perf mako ${TEST_LINK_LIBS})
endif()

# ----------------------------------------------------------------------
# Legacy upstream self-tests carried over from src/masstree/GNUmakefile.in.
# Kept in CTest for coverage; mttest deliberately not built here because
# it is a long-running benchmark harness, not a unit test.
# ----------------------------------------------------------------------
_mako_add_masstree_legacy_test(test_masstree_legacy_string
    ${MASSTREE_SRC_DIR}/test_string.cc
    ${MASSTREE_SRC_DIR}/string.cc
    ${MASSTREE_SRC_DIR}/straccum.cc
    ${MASSTREE_SRC_DIR}/compiler.cc
)

_mako_add_masstree_legacy_test(test_masstree_legacy_atomics
    ${MASSTREE_SRC_DIR}/test_atomics.cc
    ${MASSTREE_SRC_DIR}/string.cc
    ${MASSTREE_SRC_DIR}/straccum.cc
    ${MASSTREE_SRC_DIR}/kvrandom.cc
    ${MASSTREE_SRC_DIR}/json.cc
    ${MASSTREE_SRC_DIR}/compiler.cc
    ${MASSTREE_SRC_DIR}/kvio.cc
    ${MASSTREE_SRC_DIR}/str.cc
)

_mako_add_masstree_legacy_test(test_masstree_legacy_json
    ${MASSTREE_SRC_DIR}/jsontest.cc
    ${MASSTREE_SRC_DIR}/string.cc
    ${MASSTREE_SRC_DIR}/straccum.cc
    ${MASSTREE_SRC_DIR}/json.cc
    ${MASSTREE_SRC_DIR}/compiler.cc
)

_mako_add_masstree_legacy_test(test_masstree_legacy_msgpack
    ${MASSTREE_SRC_DIR}/msgpacktest.cc
    ${MASSTREE_SRC_DIR}/string.cc
    ${MASSTREE_SRC_DIR}/straccum.cc
    ${MASSTREE_SRC_DIR}/json.cc
    ${MASSTREE_SRC_DIR}/compiler.cc
    ${MASSTREE_SRC_DIR}/msgpack.cc
)

if(EXISTS "${MASSTREE_SRC_DIR}/log.hh")
    _mako_add_masstree_legacy_test(test_masstree_legacy_scan
        ${MASSTREE_SRC_DIR}/scantest.cc
        ${MASSTREE_SRC_DIR}/query_masstree.cc
        ${MASSTREE_SRC_DIR}/value_string.cc
        ${MASSTREE_SRC_DIR}/value_array.cc
        ${MASSTREE_SRC_DIR}/value_versioned_array.cc
        ${MASSTREE_SRC_DIR}/string_slice.cc
        ${MASSTREE_SRC_DIR}/compiler.cc
        ${MASSTREE_SRC_DIR}/json.cc
        ${MASSTREE_SRC_DIR}/string.cc
        ${MASSTREE_SRC_DIR}/straccum.cc
        ${MASSTREE_SRC_DIR}/str.cc
        ${MASSTREE_SRC_DIR}/msgpack.cc
        ${MASSTREE_SRC_DIR}/kvrandom.cc
        ${MASSTREE_SRC_DIR}/memdebug.cc
        ${MASSTREE_SRC_DIR}/kvthread.cc
        ${MASSTREE_SRC_DIR}/masstree_context.cc
    )
else()
    message(STATUS "Skipping test_masstree_legacy_scan: missing log.hh")
endif()
