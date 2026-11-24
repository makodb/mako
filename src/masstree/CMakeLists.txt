# Separate native storage component build. No application, SRPC or Rust engine.
# cmake -S . -B build_storage_tests -G Ninja -DMAKO_STORAGE_TESTS_ONLY=ON
# cmake --build build_storage_tests && ctest --test-dir build_storage_tests
include(CTest)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/googletest"
                 "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
include(GoogleTest)
foreach(_test test_mtree_abi test_mtx_abi test_silo_allocator test_silo_rcu_thread)
    add_executable(${_test} "${CMAKE_SOURCE_DIR}/tests/${_test}.cc")
    target_link_libraries(${_test} PRIVATE mako_native_storage GTest::gtest_main)
    gtest_discover_tests(${_test} TEST_PREFIX "${_test}." DISCOVERY_TIMEOUT 60
        PROPERTIES TIMEOUT 120 LABELS "masstree")
endforeach()
foreach(_test test_masstree test_masstree_property test_masstree_headers
        test_masstree_internals test_masstree_layered test_masstree_multi_instance
        test_masstree_memory test_masstree_concurrent test_masstree_linearizability
        test_masstree_soak)
    add_executable(${_test} "${CMAKE_SOURCE_DIR}/src/masstree/tests/${_test}.cc")
    target_link_libraries(${_test} PRIVATE mako_native_storage GTest::gtest_main)
    gtest_discover_tests(${_test} TEST_PREFIX "${_test}." DISCOVERY_TIMEOUT 60
        PROPERTIES TIMEOUT 120 LABELS "masstree")
endforeach()
add_executable(masstree_perf "${CMAKE_SOURCE_DIR}/src/masstree/tests/masstree_perf.cc")
target_link_libraries(masstree_perf PRIVATE mako_native_storage)
add_test(NAME masstree_perf_smoke COMMAND masstree_perf --keys 1000
    --lookup-rounds 1 --repetitions 1 --threads 1,2)
set_tests_properties(masstree_perf_smoke PROPERTIES TIMEOUT 120 LABELS "masstree")
option(MASSTREE_FUZZER "Build the native tree's libFuzzer entry point" OFF)
if(MASSTREE_FUZZER)
    add_executable(fuzz_masstree "${CMAKE_SOURCE_DIR}/src/masstree/tests/fuzz_masstree.cc")
    target_link_libraries(fuzz_masstree PRIVATE mako_native_storage)
    target_compile_options(fuzz_masstree PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_masstree PRIVATE -fsanitize=fuzzer)
endif()
foreach(_header mtree_abi sto_tpcc_ffi)
    add_executable(${_header}_c11_header "${CMAKE_SOURCE_DIR}/tests/${_header}_c11_header.c")
    set_target_properties(${_header}_c11_header PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
    target_include_directories(${_header}_c11_header PRIVATE
        "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/crates/sto-tpcc-ffi/include")
    add_test(NAME ${_header}_c11_header COMMAND ${_header}_c11_header)
endforeach()
