cmake_minimum_required(VERSION 3.30)

set(MAKO_BUILD_PROFILE "NATIVE_RUST" CACHE STRING "Build profile: NATIVE_RUST, ROCKSDB_RUNTIME or MASSTREE_TX")
set_property(CACHE MAKO_BUILD_PROFILE PROPERTY STRINGS NATIVE_RUST ROCKSDB_RUNTIME MASSTREE_TX)
if(MAKO_BUILD_PROFILE STREQUAL "ROCKSDB_RUNTIME")
    project(mako_rocksdb_runtime LANGUAGES NONE)
    include(cmake/RocksdbRuntime.cmake)
    return()
elseif(NOT MAKO_BUILD_PROFILE STREQUAL "NATIVE_RUST" AND NOT MAKO_BUILD_PROFILE STREQUAL "MASSTREE_TX")
    message(FATAL_ERROR "Unsupported MAKO_BUILD_PROFILE: ${MAKO_BUILD_PROFILE}. Use NATIVE_RUST or ROCKSDB_RUNTIME for applications, or MASSTREE_TX for the isolated component, in a fresh build directory.")
endif()

# Native storage uses C++23 standard-library modules. Configure CMake's
# import-std support before project() initializes the compiler.
# The token that unlocks CMake's experimental `import std;` changes between
# CMake releases, even between patch releases of one line (Help/dev/
# experimental.rst per tag: 3.30.0-3.31.6 0e5b6991, 4.0.0 a9e1cf81, 3.31.8 and
# 4.0.3-4.2.x d0edc3af, 4.3.x 451f2fe2, 4.4.x f35a9ac6), so no version table
# stays right.
# The running cmake carries its own token as a string in its executable
# (Source/cmExperimental.cxx), so take whichever known token it contains.
# An explicit -DCMAKE_EXPERIMENTAL_CXX_IMPORT_STD=... still wins.
if(NOT DEFINED CMAKE_EXPERIMENTAL_CXX_IMPORT_STD)
    set(_mako_import_std_tokens
        "f35a9ac6-8463-4d38-8eec-5d6008153e7d"
        "451f2fe2-a8a2-47c3-bc32-94786d8fc91b"
        "d0edc3af-4c50-42ea-a356-e2862fe7a444"
        "a9e1cf81-9932-4810-974b-6eccaf14e457"
        "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
    list(JOIN _mako_import_std_tokens "|" _mako_import_std_regex)
    file(STRINGS "${CMAKE_COMMAND}" _mako_import_std_hits
         REGEX "${_mako_import_std_regex}")
    foreach(_mako_token IN LISTS _mako_import_std_tokens)
        if(_mako_import_std_hits MATCHES "${_mako_token}")
            set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "${_mako_token}")
            break()
        endif()
    endforeach()
    if(NOT DEFINED CMAKE_EXPERIMENTAL_CXX_IMPORT_STD)
        message(FATAL_ERROR
            "CMake ${CMAKE_VERSION} (${CMAKE_COMMAND}) carries none of the known "
            "`import std` experimental tokens. Find this release's token in its "
            "Help/dev/experimental.rst and pass "
            "-DCMAKE_EXPERIMENTAL_CXX_IMPORT_STD=<token>, or add it above.")
    endif()
endif()
message(STATUS "import std token: ${CMAKE_EXPERIMENTAL_CXX_IMPORT_STD} (CMake ${CMAKE_VERSION})")

# Native Masstree ownership headers require the Clang 22 toolchain used by CI.
# Homebrew's LLVM keg also installs its libc++.so.1 outside the driver's
# default runtime search path. The linker setup below resolves libc++ from the
# selected toolchain so a stale system copy cannot shadow it.
#
# To install on Ubuntu:
#     wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s 22
#     sudo apt install -y clang-22 libc++-22-dev libc++abi-22-dev
#
# If the caller pre-set CMAKE_CXX_COMPILER (e.g. `cmake -DCMAKE_CXX_COMPILER=...`),
# respect that — but still gate it on the version check below.
# Search hints for the brew llvm@22 keg. Brew on macOS / Linuxbrew
# doesn't symlink versioned names (clang++-22) — it only installs
# `clang` / `clang++` inside the keg, which points at clang-22. So
# we add the keg's bin dir as an explicit HINT, and accept both
# `clang++-22` (apt.llvm.org convention) and `clang++` from inside
# the llvm@22 keg.
set(MAKO_CLANG22_HINTS
    /home/linuxbrew/.linuxbrew/opt/llvm@22/bin
    $ENV{HOMEBREW_PREFIX}/opt/llvm@22/bin
    /opt/homebrew/opt/llvm@22/bin
    /usr/local/opt/llvm@22/bin
)
# Many users have a per-user brew root under their home dir.
if(DEFINED ENV{HOME})
    list(APPEND MAKO_CLANG22_HINTS $ENV{HOME}/.linuxbrew/opt/llvm@22/bin)
endif()

if(NOT DEFINED CMAKE_C_COMPILER)
    find_program(MAKO_CLANG_22 NAMES clang-22 clang HINTS ${MAKO_CLANG22_HINTS} NO_DEFAULT_PATH)
    if(NOT MAKO_CLANG_22)
        find_program(MAKO_CLANG_22 NAMES clang-22)
    endif()
    if(NOT MAKO_CLANG_22)
        message(FATAL_ERROR
            "Mako requires Clang 22 (could not find `clang-22` on PATH or in "
            "common Homebrew/Linuxbrew keg paths). Install via apt.llvm.org "
            "(`apt install -y clang-22`) or brew (`brew install llvm@22`), or "
            "pass explicit -DCMAKE_C_COMPILER=/path/to/clang-22 to cmake. "
            "See the comment at CMakeLists.txt:19 for the rationale.")
    endif()
    set(CMAKE_C_COMPILER "${MAKO_CLANG_22}" CACHE STRING "C compiler")
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(MAKO_CLANGXX_22 NAMES clang++-22 clang++ HINTS ${MAKO_CLANG22_HINTS} NO_DEFAULT_PATH)
    if(NOT MAKO_CLANGXX_22)
        find_program(MAKO_CLANGXX_22 NAMES clang++-22)
    endif()
    if(NOT MAKO_CLANGXX_22)
        message(FATAL_ERROR
            "Mako requires Clang 22 (could not find `clang++-22` on PATH or in "
            "common Homebrew/Linuxbrew keg paths). Install via apt.llvm.org "
            "(`apt install -y clang-22`) or brew (`brew install llvm@22`), or "
            "pass explicit -DCMAKE_CXX_COMPILER=/path/to/clang++-22 to cmake. "
            "See the comment at CMakeLists.txt:19 for the rationale.")
    endif()
    set(CMAKE_CXX_COMPILER "${MAKO_CLANGXX_22}" CACHE STRING "C++ compiler")
endif()

# Enable compile_commands.json generation for language servers (clangd, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Brew's llvm@21 keg on Linux does NOT auto-add its own lib/ to the
# linker's search path. Headers from the keg's libc++ inline-reference
# symbols (e.g. std::__1::__hash_memory) that the system's apt
# /usr/lib/x86_64-linux-gnu/libc++.so (from llvm-19) doesn't export, so
# `clang++ -stdlib=libc++ -lc++` resolves to the wrong libc++ and the
# link fails on hash-table code (gtest, rusty::HashMap, etc.).
#
# Workaround: if the selected clang lives inside a Homebrew keg, add
# the keg's lib/ to the linker search path with an rpath so binaries
# can find the right libc++ at runtime too. apt.llvm.org installs are
# unaffected — clang-21 there ships in /usr/bin and uses the matching
# /usr/lib/llvm-21/lib/libc++.so via its built-in search.
# Resolve Homebrew's bin/clang++ symlink before deriving the keg root.  CMake
# preserves an explicitly supplied symlink path (for example
# ~/.linuxbrew/bin/clang++), whose parent is not the versioned LLVM keg and
# therefore cannot supply the matching libc++.
get_filename_component(MAKO_CLANG_REALPATH "${CMAKE_CXX_COMPILER}" REALPATH)
get_filename_component(MAKO_CLANG_BINDIR "${MAKO_CLANG_REALPATH}" DIRECTORY)
get_filename_component(MAKO_CLANG_ROOT "${MAKO_CLANG_BINDIR}" DIRECTORY)
set(MAKO_BREW_LIBCXX_FLAGS "")
if(EXISTS "${MAKO_CLANG_ROOT}/lib/libc++.so")
    # Match brew/linuxbrew (path contains `Cellar/llvm` or `opt/llvm`).
    if(MAKO_CLANG_ROOT MATCHES "(Cellar|opt)/llvm")
        set(MAKO_BREW_LIBCXX_FLAGS
            "-L${MAKO_CLANG_ROOT}/lib -Wl,-rpath,${MAKO_CLANG_ROOT}/lib")
        message(STATUS "Using Homebrew libc++ from ${MAKO_CLANG_ROOT}/lib")
    endif()
endif()

# Force libc++ at toolchain-detect time (before project()). CMake's
# `import std;` machinery rejects libstdc++ and will refuse to create the
# __CMAKE::CXX23 target unless libc++ is the detected standard library.
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-stdlib=libc++ ${MAKO_BREW_LIBCXX_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-stdlib=libc++ ${MAKO_BREW_LIBCXX_FLAGS}")

project(mako)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)  # Must match the GNU-extensions setting used when CMake builds std.pcm for `import std;`
# Required for C++20 named-module import syntax with GCC.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fmodules-ts>)
endif()

# Use libc++ with clang. libc++'s header units share a single submodule for
# their common sub-headers (bits/align.h, <compare>, etc.), which avoids the
# ODR false positives that plague libstdc++ + clang header units when a TU
# imports multiple std header units via `import srpc;`.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
    add_link_options(-stdlib=libc++ -lc++abi)
    # Also apply the detected keg path after project() so an existing build
    # tree picks it up; *_FLAGS_INIT only initializes a fresh CMake cache.
    if(MAKO_BREW_LIBCXX_FLAGS)
        separate_arguments(MAKO_BREW_LIBCXX_OPTIONS NATIVE_COMMAND
            "${MAKO_BREW_LIBCXX_FLAGS}")
        add_link_options(${MAKO_BREW_LIBCXX_OPTIONS})
    endif()
endif()

# Materialize the standard-library module for the native storage target.
set(CMAKE_CXX_MODULE_STD ON)

# Cargo owns all application, transaction, migration and SRPC code.
include(cmake/NativeStorage.cmake)
