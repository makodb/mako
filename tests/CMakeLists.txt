cmake_minimum_required(VERSION 3.10)

# Add test executable for Silo varint tests
add_executable(test_silo_varint
    silo/test_varint.cc
)

target_link_libraries(test_silo_varint
    mako
    gtest
    gtest_main
    pthread
)

target_include_directories(test_silo_varint PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mako
)

# Add performance benchmark for Silo varint
add_executable(bench_silo_varint
    silo/bench_varint.cc
)

target_link_libraries(bench_silo_varint
    mako
    gtest
    pthread
    benchmark
    benchmark_main
)

target_include_directories(bench_silo_varint PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mako
)

# Add test executable for STO transactions
add_executable(test_sto_transaction
    sto/test_transaction.cc
)

target_link_libraries(test_sto_transaction
    mako
    gtest
    gtest_main
    pthread
)

target_include_directories(test_sto_transaction PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mako
    ${CMAKE_SOURCE_DIR}/src/mako/benchmarks
)

# Add performance benchmark for STO transactions
add_executable(bench_sto_transaction
    sto/bench_transaction.cc
)

target_link_libraries(bench_sto_transaction
    mako
    gtest
    pthread
    benchmark
    benchmark_main
)

target_include_directories(bench_sto_transaction PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mako
    ${CMAKE_SOURCE_DIR}/src/mako/benchmarks
)

# Register tests with CTest
enable_testing()

add_test(NAME SiloVarintTests COMMAND test_silo_varint)
add_test(NAME STOTransactionTests COMMAND test_sto_transaction)

# Add custom target to run all tests
add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_silo_varint test_sto_transaction
    COMMENT "Running all unit tests"
)

# Add custom target to run all benchmarks
add_custom_target(run_all_benchmarks
    COMMAND ./bench_silo_varint
    COMMAND ./bench_sto_transaction
    DEPENDS bench_silo_varint bench_sto_transaction
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running all performance benchmarks"
)
