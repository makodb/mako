# Masstree-specific test and benchmark target wiring.

function(_mako_add_masstree_legacy_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_include_directories(${TARGET_NAME} PRIVATE
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/${MASSTREE_DIR}
    )
    target_compile_options(${TARGET_NAME} PRIVATE ${TEST_COMPILE_OPTIONS} ${MASSTREE_FORCE_INCLUDE_FLAG})
    target_link_libraries(${TARGET_NAME} pthread)
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
    set_tests_properties(${TARGET_NAME} PROPERTIES
        LABELS "masstree;legacy"
        TIMEOUT 60
    )
endfunction()

function(mako_add_masstree_tests)
    set(MASSTREE_TEST_DIR "${CMAKE_SOURCE_DIR}/src/masstree/tests")

    # Masstree unit tests (run with ctest/make test)
    add_executable(test_masstree ${MASSTREE_TEST_DIR}/test_masstree.cc)
    target_include_directories(test_masstree PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_masstree PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_masstree mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_masstree COMMAND test_masstree)

    # Masstree property-based test (std::map oracle, seeded RNG)
    add_executable(test_masstree_property ${MASSTREE_TEST_DIR}/test_masstree_property.cc)
    target_include_directories(test_masstree_property PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_masstree_property PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_masstree_property mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_masstree_property COMMAND test_masstree_property)

    # Masstree concurrent stress tests (multi-threaded shared tree)
    add_executable(test_masstree_concurrent ${MASSTREE_TEST_DIR}/test_masstree_concurrent.cc)
    target_include_directories(test_masstree_concurrent PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_masstree_concurrent PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_masstree_concurrent mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_masstree_concurrent COMMAND test_masstree_concurrent)
    set_tests_properties(test_masstree_concurrent PROPERTIES TIMEOUT 60)

    # Masstree per-key linearizability check
    add_executable(test_masstree_linearizability ${MASSTREE_TEST_DIR}/test_masstree_linearizability.cc)
    target_include_directories(test_masstree_linearizability PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_masstree_linearizability PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_masstree_linearizability mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_masstree_linearizability COMMAND test_masstree_linearizability)
    set_tests_properties(test_masstree_linearizability PROPERTIES TIMEOUT 60)

    # Masstree internals tests (threadinfo, globalepoch, RCU)
    add_executable(test_masstree_internals ${MASSTREE_TEST_DIR}/test_masstree_internals.cc)
    target_include_directories(test_masstree_internals PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_masstree_internals PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_masstree_internals mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_masstree_internals COMMAND test_masstree_internals)

    # Masstree multi-instance stress tests (MasstreeContext isolation)
    add_executable(test_masstree_multi_instance ${MASSTREE_TEST_DIR}/test_masstree_multi_instance.cc)
    target_include_directories(test_masstree_multi_instance PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_masstree_multi_instance PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_masstree_multi_instance mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_masstree_multi_instance COMMAND test_masstree_multi_instance)

    # SiloRuntime tests (per-site runtime isolation)
    add_executable(test_silo_runtime ${MASSTREE_TEST_DIR}/test_silo_runtime.cc)
    target_include_directories(test_silo_runtime PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_silo_runtime PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_silo_runtime mako ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_silo_runtime COMMAND test_silo_runtime)

    # Multi-site Silo stress tests
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

    # Masstree performance benchmark (on-demand throughput output)
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

    # Legacy Masstree self-tests from src/masstree/GNUmakefile.in.
    # Keep these in CTest for CI coverage while leaving mttest out of CI
    # because it is a long-running benchmark harness by default.
    _mako_add_masstree_legacy_test(test_masstree_legacy_string
        src/masstree/test_string.cc
        src/masstree/string.cc
        src/masstree/straccum.cc
        src/masstree/compiler.cc
    )

    _mako_add_masstree_legacy_test(test_masstree_legacy_atomics
        src/masstree/test_atomics.cc
        src/masstree/string.cc
        src/masstree/straccum.cc
        src/masstree/kvrandom.cc
        src/masstree/json.cc
        src/masstree/compiler.cc
        src/masstree/kvio.cc
        src/masstree/str.cc
    )

    _mako_add_masstree_legacy_test(test_masstree_legacy_json
        src/masstree/jsontest.cc
        src/masstree/string.cc
        src/masstree/straccum.cc
        src/masstree/json.cc
        src/masstree/compiler.cc
    )

    _mako_add_masstree_legacy_test(test_masstree_legacy_msgpack
        src/masstree/msgpacktest.cc
        src/masstree/string.cc
        src/masstree/straccum.cc
        src/masstree/json.cc
        src/masstree/compiler.cc
        src/masstree/msgpack.cc
    )

    if(EXISTS "${CMAKE_SOURCE_DIR}/${MASSTREE_DIR}/log.hh")
        _mako_add_masstree_legacy_test(test_masstree_legacy_scan
            src/masstree/scantest.cc
            src/masstree/query_masstree.cc
            src/masstree/value_string.cc
            src/masstree/value_array.cc
            src/masstree/value_versioned_array.cc
            src/masstree/string_slice.cc
            src/masstree/compiler.cc
            src/masstree/json.cc
            src/masstree/string.cc
            src/masstree/straccum.cc
            src/masstree/str.cc
            src/masstree/msgpack.cc
            src/masstree/kvrandom.cc
            src/masstree/memdebug.cc
            src/masstree/kvthread.cc
            src/masstree/masstree_context.cc
        )
    else()
        message(STATUS "Skipping test_masstree_legacy_scan: missing ${MASSTREE_DIR}/log.hh")
    endif()
endfunction()
