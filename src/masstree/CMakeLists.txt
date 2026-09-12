# Masstree library + test/benchmark target wiring.
#
# The library is defined at top level (runs unconditionally when the
# root CMakeLists calls `add_subdirectory(src/masstree)`). The tests
# are wrapped in `mako_add_masstree_tests()`, a function the root
# invokes from inside `if(BUILD_TESTS)` once TEST_INCLUDE_DIRS,
# GTEST_LIBRARIES, etc. have been set up. Keeping everything in one
# CMakeLists makes the masstree subtree easy to scan and reorder.
#
# The library .cc files have no `mako/` includes — they reference
# only other masstree headers and the STL — so libmasstree can be a
# clean static library; libmako then PUBLICly depends on it. A touch
# in src/deptran/ or src/mako/lib/ no longer recompiles masstree
# sources, and vice versa.
#
# The root CMakeLists keeps `add_borrow_check(src/masstree/...cc)`
# entries pointing at these source paths; borrow checking is
# per-file, not per-target, so it doesn't care which library owns
# them.

# ----------------------------------------------------------------------
# libmasstree
# ----------------------------------------------------------------------

set(MASSTREE_LIB_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/compiler.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/str.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/string.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/straccum.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/json.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/kvthread.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/masstree_context.cc"
)

add_library(masstree STATIC ${MASSTREE_LIB_SOURCES})
target_include_directories(masstree PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${MASSTREE_CONFIG_INCLUDE_DIR}
)
# Force-include the generated Masstree config in each ordinary source without
# attaching that option to the target. Target-level options also apply to
# CMake's synthetic `std.cppm` compilation, where injecting a header before
# the leading `module;` declaration is ill-formed. These sources themselves
# import `std`, so the target must retain the project's CXX_MODULE_STD default.
if(DEFINED MASSTREE_FORCE_INCLUDE_FLAG)
    foreach(_MASSTREE_SOURCE IN LISTS MASSTREE_LIB_SOURCES)
        set_property(SOURCE "${_MASSTREE_SOURCE}" APPEND PROPERTY
            COMPILE_OPTIONS ${MASSTREE_FORCE_INCLUDE_FLAG})
    endforeach()
endif()

# ----------------------------------------------------------------------
# Tests & benchmarks — deferred via function so they only fire after
# the root CMakeLists has set BUILD_TESTS / TEST_INCLUDE_DIRS / etc.
# ----------------------------------------------------------------------

# Helper for the upstream-style "legacy" tests carried over from the
# original Masstree GNUmakefile. They build a single .cc against the
# minimal set of masstree source files it needs.
function(_mako_add_masstree_legacy_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    # Function is called from the root scope, so CMAKE_CURRENT_SOURCE_DIR
    # would be the root. Use the absolute masstree path.
    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/masstree
    )
    target_compile_options(${TARGET_NAME} PRIVATE
        ${TEST_COMPILE_OPTIONS}
    )
    mako_force_include_masstree_config(${ARGN})
    target_link_libraries(${TARGET_NAME} pthread)
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
    set_tests_properties(${TARGET_NAME} PROPERTIES
        LABELS "masstree;legacy"
        TIMEOUT 60
    )
endfunction()

function(mako_add_masstree_tests)
    # CMAKE_CURRENT_SOURCE_DIR inside a function is the caller's dir
    # (the root), not the dir where the function was defined. Use
    # absolute paths anchored at CMAKE_SOURCE_DIR so the function
    # works correctly when called from the root.
    set(MASSTREE_TEST_DIR "${CMAKE_SOURCE_DIR}/src/masstree/tests")
    set(MASSTREE_SRC_DIR  "${CMAKE_SOURCE_DIR}/src/masstree")

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

    # Memory & RCU correctness probes — under ASan/LSan these become
    # real leak/UAF detectors (Tier 4).
    add_executable(test_masstree_memory ${MASSTREE_TEST_DIR}/test_masstree_memory.cc)
    target_include_directories(test_masstree_memory PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_masstree_memory PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_masstree_memory mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_masstree_memory COMMAND test_masstree_memory)
    set_tests_properties(test_masstree_memory PROPERTIES TIMEOUT 60)

    # Layered / slice-boundary edge cases (Tier 7). The layer-collapse
    # test doubles as an LSan probe for missed layer frees.
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
    # Not wired into ctest; intended to be invoked by hand or via
    # OSS-Fuzz. See docs/masstree-test-plan.md for the toolchain
    # requirements (libc++-built compiler-rt fuzzer runtime; on
    # Debian/Ubuntu use brew clang 22).
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

    # NOTE: test_silo_runtime, test_silo_multi_site_stress, and
    # repro_finding6 used to live here but were moved to the top-
    # level tests/ directory (and wired from the root CMakeLists)
    # because their subject matter is the Mako SiloRuntime / Mako
    # allocator, not Masstree. The masstree/tests folder is for
    # tests that exercise Masstree behaviour; Mako-side tests
    # belong with their siblings in tests/.

    # On-demand performance benchmark.
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

    # Legacy upstream self-tests carried over from src/masstree/GNUmakefile.in.
    # Kept in CTest for coverage; mttest deliberately not built here because
    # it is a long-running benchmark harness, not a unit test.
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
endfunction()
