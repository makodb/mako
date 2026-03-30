cmake_minimum_required(VERSION 3.16)
project(mako_kv_store VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS_DEBUG "-g -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -Wall -Wextra -DNDEBUG")

# Find required packages
find_package(Threads REQUIRED)

# Rust library path
set(RUST_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/target/release")
set(RUST_LIB_NAME "rust_redis")

# Custom target to build Rust library
add_custom_target(rust_build ALL
    COMMAND cargo build --release
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib
    COMMENT "Building Rust library"
    VERBATIM
)

# Create imported library for Rust library
add_library(rust_lib STATIC IMPORTED)
set_target_properties(rust_lib PROPERTIES
    IMPORTED_LOCATION "${RUST_LIB_DIR}/lib${RUST_LIB_NAME}.a"
)
add_dependencies(rust_lib rust_build)

# Source files
set(SOURCES
    src/main.cpp
    src/rust_wrapper.cc
    src/kv_store.cc
)

set(HEADERS
    src/rust_wrapper.h
    src/kv_store.h
)

# Create executable
add_executable(mako_server ${SOURCES} ${HEADERS})

# Include directories
target_include_directories(mako_server PRIVATE src)

# Link libraries
target_link_libraries(mako_server 
    PRIVATE 
    rust_lib
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

# Ensure Rust library is built before C++ executable
add_dependencies(mako_server rust_lib)

# Custom targets for convenience
add_custom_target(clean_all
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target clean
    COMMAND cargo clean
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib
    COMMENT "Cleaning all build artifacts"
)