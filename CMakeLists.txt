cmake_minimum_required(VERSION 3.30)

# `import std;` is enabled via `src/srpc/std_compat.hpp`. The header
# textually includes every C++ standard header AND then `import std;`s.
# Clang's libc++ rejects `import std; ... #include <vector>` but
# accepts the reverse order, so the include block comes first; that
# also turns later transitive `#include`s from rusty-cpp/yaml-cpp/etc.
# into no-ops via the header guard. The UUID gates CMake's
# experimental `import std;` materialization and must be set BEFORE
# `project()`.
# CMake intentionally rotates this experimental gate. Keep the established
# 3.30/3.31 value while accepting the CMake 4.4 gate used by current developer
# environments.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.4")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "a9e1cf81-9932-4810-974b-6eccaf14e457")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.31.8")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
endif()

# Suppress CMake policy warnings for deprecated modules
# CMP0167: FindBoost module is removed in CMake 3.30+, but we still support older CMake
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)  # Use deprecated FindBoost for compatibility
endif()

# Mako pins its compiler to Clang 22 (or newer). Clang 21 and earlier
# frontend-crash compiling reactor.cpp's C++20-module codegen (the
# `srpc.reactor` named module); Clang 22 fixes it — see the explicit
# version check after project() below.
# The reasons for requiring >= 22:
#   * Clang 21 SIGSEGVs in the frontend on srpc.reactor's module codegen.
#   * Clang 20 and earlier lack alias-template CTAD support that
#     rusty-cpp's platform::threading aliases depend on.
#   * CI standardizes on clang-22 (apt.llvm.org noble repo).
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

# The fiber context-switch trampolines are real `.S` assembly
# (reactor/fiber_context_{x86_64,aarch64}.S) rather than assembly hidden
# in a C++ raw string. `.S` (capital) is preprocessed, so the arch
# guards still work, and the assembler now checks the syntax.
enable_language(ASM)

# Prefer shared libraries when available for third-party dependencies.
set(BUILD_SHARED_LIBS ON)

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

# `std_compat.hpp` does `import std;` at the bottom of its 90-line
# textual aggregate. ON so CMake builds the `std.pcm` BMI and feeds
# it to every C++23 target. Per-target opt-out is via setting
# `CXX_MODULE_STD OFF` on the target.
set(CMAKE_CXX_MODULE_STD ON)

# Check compiler and version. C++23 + named modules require GCC >= 13.1
# or Clang >= 17.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13.1)
        message(FATAL_ERROR "GCC version 13.1 or higher is required. Found ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 22.0)
        message(FATAL_ERROR "Clang version 22.0 or higher is required. Found ${CMAKE_CXX_COMPILER_VERSION}. (clang-21 frontend-crashes compiling reactor.cpp's C++20-module codegen; clang-22 fixes it.)")
    endif()
else()
    message(WARNING "Compiler ${CMAKE_CXX_COMPILER_ID} is unvalidated (requires GCC >= 13.1 or Clang >= 22).")
endif()

# Auto-detect the latest available libclang for rusty-cpp-checker.
# The checker uses libclang to parse C++ headers; if the system installs newer
# clang headers (e.g., clang 19 with __builtin_nondeterministic_value in AVX
# intrinsics), the checker must be linked against the matching libclang version
# to avoid "Fatal parsing errors" from unknown builtins.
#
# Production toolchain is Homebrew/Linuxbrew clang 21 (llvm@21). clang 22's
# libclang has a parse-time crash on first-time user-type template
# instantiations in module purview — see docs/dev/libclang22_parse_crash.md.
# Pick clang 21 first when present, then fall back to system clang.
if(NOT DEFINED ENV{LIBCLANG_PATH})
    set(_LIBCLANG_CANDIDATES
        # Linuxbrew clang 21 (current production)
        "/home/users/shuai/.linuxbrew/opt/llvm@21/lib"
        # macOS Homebrew clang 21
        "/opt/homebrew/opt/llvm@21/lib"
        "/usr/local/opt/llvm@21/lib"
    )
    foreach(_CAND ${_LIBCLANG_CANDIDATES})
        if(EXISTS "${_CAND}/libclang.so" OR EXISTS "${_CAND}/libclang.dylib")
            set(ENV{LIBCLANG_PATH} "${_CAND}")
            message(STATUS "Auto-detected LIBCLANG_PATH=${_CAND} (Homebrew clang 21)")
            break()
        endif()
    endforeach()
endif()
if(NOT DEFINED ENV{LIBCLANG_PATH})
    # Fall back to the newest system-installed libclang, preferring higher versions.
    # We deliberately stop at 21 — see comment above re: clang 22 libclang crash.
    foreach(_CLANG_VER 21 19 18 17 16 15 14)
        set(_CLANG_LIB_DIR "/usr/lib/llvm-${_CLANG_VER}/lib")
        if(EXISTS "${_CLANG_LIB_DIR}/libclang-${_CLANG_VER}.so" OR
           EXISTS "${_CLANG_LIB_DIR}/libclang.so")
            set(ENV{LIBCLANG_PATH} "${_CLANG_LIB_DIR}")
            message(STATUS "Auto-detected LIBCLANG_PATH=${_CLANG_LIB_DIR} (clang ${_CLANG_VER})")
            break()
        endif()
    endforeach()
endif()

# Default ENABLE_BORROW_CHECKING to OFF.  This MUST be set before the
# include below — `third-party/rusty-cpp/cmake/RustyCppSubmodule.cmake`
# calls `option(ENABLE_BORROW_CHECKING ... ON)` which would otherwise
# win the cache race.  `set(... CACHE BOOL ...)` without FORCE is a
# no-op when the cache already has a value, so a `-DENABLE_BORROW_CHECKING=ON`
# on the command line still wins.
# TEMPORARY: disabled during CMake 3.28+ native-module migration — the
# rusty-cpp checker cannot resolve `import srpc;` without being handed the
# BMI paths. Re-enable once the checker integrates with CMake module info.
set(ENABLE_BORROW_CHECKING OFF CACHE BOOL "Enable RustyCpp borrow checking")

# Configure and enable rusty-cpp-checker
include(${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/cmake/RustyCppSubmodule.cmake)
set(RUSTYCPP_BUILD_TYPE "release")  # or "debug"
include(${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/cmake/RustyCppTranspiler.cmake)
create_rustycpp_transpiler_build_target()

# NOTE: RustyCppSubmodule.cmake already computes CPP_BORROW_CHECKER and creates
# build targets. Do not override checker paths here or Docker CARGO_TARGET_DIR
# support is bypassed.
if(ENABLE_BORROW_CHECKING)
    enable_borrow_checking()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

# Find Python for the txlog_core build surface.
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# Define paths
set(mkfile_path "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
set(HOME "${CMAKE_CURRENT_SOURCE_DIR}")
set(BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(W "src/mako")
set(MASSTREE_DIR "src/masstree")
set(TOP "${CMAKE_SOURCE_DIR}")

# Global include roots for `import srpc;` and srpc header-unit/module prebuild.
include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/srpc"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include"
)

# Build yaml-cpp from vendored source to avoid system ABI/version drift.
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "Disable yaml-cpp tests" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "Disable yaml-cpp tools" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "Disable yaml-cpp install targets" FORCE)
add_subdirectory(third-party/yaml-cpp)

# rusty-cpp main HEAD retired the hand-written hashmap/btreemap/btreeset/rc/vec
# headers in favor of transpiled C++20 modules (vec_port.vec, btree_port.btree.*,
# rc_port, std_port/std_port_hashbrown, plus the umbrella rusty.cppm). Pull in
# those targets
# via add_subdirectory so srpc can `import rusty;`. EXCLUDE_FROM_ALL keeps
# rusty-cpp's test/bench targets out of the default build — they only build
# if explicitly requested via `cmake --build build --target <name>.out`.
#
# rusty-cpp's top-level CMakeLists.txt sets CMAKE_CXX_EXTENSIONS=OFF
# (it uses -std=c++23 strict). Mako uses -std=gnu++23 (see line ~45
# above: CMAKE_CXX_EXTENSIONS=ON, to match the GNU-extensions setting
# CMake bakes into std.pcm for `import std;`). PCM consumption checks
# these flags strictly: rusty.cppm + the port PCMs (built strict) can't
# be imported from gnu++23 TUs ("GNU extensions was disabled in
# precompiled file"). Force the port modules onto gnu++23 just for
# this subdirectory so srpc can consume them.
add_subdirectory(third-party/rusty-cpp EXCLUDE_FROM_ALL)
# rusty-cpp's CMakeLists.txt locally sets CMAKE_CXX_EXTENSIONS=OFF.
# Override on each port target so srpc (gnu++23) can `import` them
# without the "GNU extensions was disabled" PCM-config mismatch.
# `std_port` and `std_port_hashbrown` replaced the deleted `hashbrown_port`
# target (rusty-cpp #177): rusty.cppm now `import std_port;` for
# rusty::HashMap / rusty::HashSet, and std_port itself imports the separate
# `hashbrown` module. BOTH have to carry CXX_EXTENSIONS ON — the BMI-config
# check applies to every module in the import closure, not just the umbrella.
foreach(_rusty_port_target IN ITEMS
    vec_port btree_port rc_port std_port std_port_hashbrown rusty_async rusty
    binary_heap_port binary_heap_tests_port linked_list_tests_port
    eq_diff_len_tests_port vec_deque_port cell_port string_port
    arc_port linked_list_port borrow_port ascii_port)
    if(TARGET ${_rusty_port_target})
        set_target_properties(${_rusty_port_target} PROPERTIES CXX_EXTENSIONS ON)
    endif()
endforeach()

# RUST
# Respect CARGO_TARGET_DIR environment variable if set (for Docker builds)
if(DEFINED ENV{CARGO_TARGET_DIR})
    set(RUST_LIB_TARGET_BASE "$ENV{CARGO_TARGET_DIR}")
    message(STATUS "Using CARGO_TARGET_DIR for rust-lib: ${RUST_LIB_TARGET_BASE}")
else()
    set(RUST_LIB_TARGET_BASE "${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/target")
endif()
set(RUST_LIB_DIR "${RUST_LIB_TARGET_BASE}/release")
set(RUST_LIB_NAME "rust_redis")
set(RUST_LIB_PATH "${RUST_LIB_DIR}/lib${RUST_LIB_NAME}.a")

# Use add_custom_command instead of add_custom_target to avoid rebuilding every time
add_custom_command(
    OUTPUT ${RUST_LIB_PATH}
    COMMAND cargo build --release
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/src/lib.rs
    COMMENT "Building Rust library if needed"
)

# Create a custom target that depends on the output
add_custom_target(rust_build ALL DEPENDS ${RUST_LIB_PATH})

# srpc module files and the `srpc` target are defined in
# src/srpc-cmake/CMakeLists.txt.

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Mako targets Clang 22.x (see the rationale at CMakeLists.txt:19 and the
    # >= 22.0 gate after project()). Accept any 22.x minor version; reject
    # everything else. Clang 21 and earlier frontend-crash on srpc.reactor's
    # C++20-module codegen; Clang 23+ is untested.
    if (NOT CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "22.0"
        OR CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "23.0")
        message(FATAL_ERROR
            "Mako requires Clang 22.x (found ${CMAKE_CXX_COMPILER_VERSION} at ${CMAKE_CXX_COMPILER}). "
            "Clang 21 and earlier SIGSEGV in the frontend on srpc.reactor's C++20-module "
            "codegen; Clang 23+ is untested. "
            "Install clang-22 via apt.llvm.org (Ubuntu) or use the CI image.")
    endif()
else()
    # Reject GCC, AppleClang, Intel, MSVC: the project's safety
    # annotations, `import std;` integration, and rusty-cpp are all
    # exercised only against Clang 22.
    message(FATAL_ERROR
        "Mako requires Clang 22 (found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
        "at ${CMAKE_CXX_COMPILER}). Other compilers are untested and unsupported. "
        "Install clang-22 via apt.llvm.org (Ubuntu) or use the CI image.")
endif()
# The remaining libc++/codegen flags below run only after we've established
# we are on Clang 22. The original `if (...Clang)` block continues to gate
# them in case someone widens the supported compiler set later — but for
# now the FATAL_ERROR above guarantees we never reach the else-branch.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")

    # libc++ removed std::unary_function/binary_function (LWG3820, C++20);
    # some in-tree Masstree code still references them. Re-enable the old
    # typedefs for backwards compatibility.
    add_compile_definitions(_LIBCPP_ENABLE_CXX20_REMOVED_BINDER_TYPEDEFS)
    add_compile_definitions(_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION)
    # libc++ removed std::result_of in C++20; re-enable for transitive
    # consumers that still reference it.
    add_compile_definitions(_LIBCPP_ENABLE_CXX20_REMOVED_TYPE_TRAITS)
    # libc++ 19's __bit_reference declares friend functions without the
    # abi_tag attribute on line ~974 and then redeclares the same functions
    # with _LIBCPP_HIDE_FROM_ABI (which adds abi_tag) on line ~177. Clang 19
    # rejects this mismatch. Disable the abi_tag addition in _LIBCPP_HIDE_FROM_ABI.
    add_compile_definitions(_LIBCPP_NO_ABI_TAG)
    # Expose test-only hooks (replay_pending_requests_for_test,
    # update_pending_queue_config_for_test) on ClientConnection. The
    # rpc_request_buffering_test relies on them; defining via macro at consumer
    # TU level doesn't work because module interfaces are pre-compiled.
    add_compile_definitions(RPC_TEST_HOOKS)

    # rusty-cpp's HashMap (SwissTable) and mpsc_lockfree's CPU_RELAX
    # default to including `<immintrin.h>` for SSE2 intrinsics. That
    # header reaches the srpc module's global module fragment via
    # `#include <rusty/rusty.hpp>` in module-interface units, and
    # under clang21+ collides with the consumer's own
    # `<immintrin.h>` (e.g. via masstree headers in deptran code) —
    # see rusty-cpp commit 49e794d for the full diagnosis. Defining
    # this macro project-wide swaps both to scalar / builtin
    # fallbacks consistently across every TU, eliminating the
    # collision and preserving ODR across the program. The
    # measurable cost is tiny (single-digit-percent on hot hashmap
    # paths); the build wins are clang21+ compatibility today.
    add_compile_definitions(RUSTY_PORTABLE_INTRINSICS=1)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "GCC named-module build for `import srpc;` currently hits a compiler ICE on this codebase. "
        "Use clang++ for this branch.")
endif()

# Create imported library for Rust library
add_library(rust_lib STATIC IMPORTED)
set_target_properties(rust_lib PROPERTIES
    IMPORTED_LOCATION "${RUST_LIB_PATH}"
)
add_dependencies(rust_lib rust_build)

# srpc was previously a C++23 named-modules library with a per-build
# PCM cache; de-modularized in 2026-04 (replaced module declarations
# with plain headers). The PCM-cache compile options are no longer
# needed.

# Options
option(DEBUG "Enable debug mode" OFF)
option(CHECK_INVARIANTS "Enable invariant checking" OFF)
option(COCO "Enable COCO" OFF)
option(MAKO_USE_RAFT "Build Mako with the Raft replication helper" OFF)
option(RAFT_TEST "Enable Raft lab testing fiber" OFF)
option(SINGLE_RAFT_INSTANCE "Default runtime Raft group mode (ON=single group, OFF=one group per partition)" ON)

if(MAKO_USE_RAFT)
  add_compile_definitions(MAKO_USE_RAFT=1)
  message(STATUS "Mako Raft replication enabled")
endif()

if(RAFT_TEST)
  add_compile_definitions(RAFT_TEST_CORO=1)
  message(STATUS "Raft lab testing fiber enabled (note: REUSE_FIBER is always enabled)")
endif()

if(SINGLE_RAFT_INSTANCE)
  add_compile_definitions(RAFT_DEFAULT_SINGLE_GROUP=1)
  message(STATUS "Default runtime Raft group mode: single (override with --raft-groups or MAKO_RAFT_GROUP_MODE)")
else()
  message(STATUS "Default runtime Raft group mode: multi (override with --raft-groups or MAKO_RAFT_GROUP_MODE)")
endif()

set(USE_MALLOC_MODE 1 CACHE STRING "Malloc mode: 0=libc, 1=jemalloc, 2=tcmalloc, 3=flow")
set(MODE "perf" CACHE STRING "Build mode: perf, backoff, factor-gc, factor-gc-nowriteinplace, factor-fake-compression, sandbox")
option(STO_RMW "Enable STO read-modify-write" OFF)
option(HASHTABLE "Enable hashtable" OFF)
option(GPROF "Enable profiling with gprof" OFF)
option(OPACITY "Enable opacity" OFF)
option(DISABLE_MULTI_VERSION "Disable multi-version" OFF)
option(MEGA_BENCHMARK "Enable MEGA benchmark" OFF)
option(MEGA_BENCHMARK_MICRO "Enable MEGA benchmark micro" OFF)
option(TRACKING_LATENCY "Enable latency tracking" OFF)
option(DISABLE_DISK "Disable disk persistence" OFF)
option(FAIL_NEW_VERSION "Enable fail new version" ON)
option(SIMULATE_ONE_SHARD_PER_THREAD "Simulate one shard per thread" OFF)
option(TRACKING_ROLLBACK "Enable rollback tracking" OFF)
option(MASSTREE_ENABLE_SUPERPAGE "Enable Masstree superpage support when available" ON)
set(MASSTREE_MAX_KEY_LEN 1024 CACHE STRING "Maximum key length for Masstree (bytes)")
set(MASSTREE_ROW_TYPE "bag" CACHE STRING "Default Masstree row type (bag,array,array_ver,str)")

# Masstree configuration
set(MASSTREE_CONFIG "CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER} --enable-max-key-len=${MASSTREE_MAX_KEY_LEN}")
if(DEBUG)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --enable-assertions")
else()
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --disable-assertions")
endif()
if(CHECK_INVARIANTS)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --enable-invariants --enable-preconditions")
else()
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --disable-invariants --disable-preconditions")
endif()

# Output suffix
set(OSUFFIX_D "")
set(OSUFFIX_S "")
set(OSUFFIX_E "")
set(OSUFFIX_H "")
set(OSUFFIX_R "")
if(DEBUG)
  set(OSUFFIX_D "")
else()
  set(OSUFFIX_D "")
endif()
if(CHECK_INVARIANTS)
  set(OSUFFIX_S ".check")
else()
  set(OSUFFIX_S "")
endif()
if(EVENT_COUNTERS)
  set(OSUFFIX_E ".ectrs")
endif()
if(STO_RMW)
  set(OSUFFIX_R ".rmw")
endif()
if(HASHTABLE)
  set(OSUFFIX_H ".ht")
endif()
set(OSUFFIX "${OSUFFIX_D}${OSUFFIX_S}${OSUFFIX_E}${OSUFFIX_H}${OSUFFIX_R}")

# Output directory and config header
if(MODE STREQUAL "perf")
  set(O "out-perf${OSUFFIX}")
  set(CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/config-perf.h")
elseif(MODE STREQUAL "backoff")
  set(O "out-backoff${OSUFFIX}")
  set(CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/config-backoff.h")
elseif(MODE STREQUAL "factor-gc")
  set(O "out-factor-gc${OSUFFIX}")
  set(CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/config-factor-gc.h")
elseif(MODE STREQUAL "factor-gc-nowriteinplace")
  set(O "out-factor-gc-nowriteinplace${OSUFFIX}")
  set(CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/config-factor-gc-nowriteinplace.h")
elseif(MODE STREQUAL "factor-fake-compression")
  set(O "out-factor-fake-compression${OSUFFIX}")
  set(CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/config-factor-fake-compression.h")
elseif(MODE STREQUAL "sandbox")
  set(O "out-sandbox${OSUFFIX}")
  set(CONFIG_H "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/config-sandbox.h")
else()
  message(FATAL_ERROR "Invalid mode: ${MODE}")
endif()
set(O "${O}.masstree")

# Configure Masstree's config.h using CMake checks (no autoconf dependency).
include(${CMAKE_CURRENT_SOURCE_DIR}/src/masstree/ConfigureMasstree.cmake)

# Borrow checker inputs (place after CONFIG_H so it is available to the checker)
set(BORROW_CHECKER_INCLUDE_PATHS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
    ${CMAKE_CURRENT_SOURCE_DIR}/${MASSTREE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
)

# Make config paths visible to the borrow checker (will be turned into -include)
add_compile_definitions(MASSTREE_CONFIG_H=\"${MASSTREE_GENERATED_CONFIG_H}\")
add_compile_definitions(MASSTREE_SUPPORT_HEADER=\"${CMAKE_CURRENT_SOURCE_DIR}/src/mako/masstree_btree.h\")

# Masstree borrow checking - incrementally enabled per file
# Phase 4 of RustyCpp migration enables borrow checking file by file
# See TODO.md "Masstree RustyCpp Safety Migration" for details
#
# Previously disabled due to false positives that have been addressed by:
#   - Adding @unsafe { reason } annotations to all unsafe functions
#   - Converting raw pointers to rusty::Ptr<T>/MutPtr<T> wrappers
#   - Marking safe getters/setters with @safe annotations
#
# Keep these checks gated behind ENABLE_BORROW_CHECKING so normal CI builds do
# not require the checker's runtime libclang setup.
if(ENABLE_BORROW_CHECKING)
    # add_borrow_check pulls include paths from the current directory's
    # INCLUDE_DIRECTORIES property. Make sure the masstree-generated
    # config dir + its parent are visible at directory scope so that
    # compiler.hh's `#include "config.h"` and src/mako's
    # `#include "masstree/config.h"` both resolve during borrow checking
    # (the force-include in target_compile_options is target-scoped and
    # add_borrow_check doesn't see it).
    include_directories(
        ${MASSTREE_CONFIG_INCLUDE_DIR}
        ${CMAKE_BINARY_DIR}/generated
    )
    # Phase 4.1: Enable borrow checking for masstree_context
    add_borrow_check(src/masstree/masstree_context.cc)
    #
    # Phase 4.2: Enable borrow checking for kvthread
    add_borrow_check(src/masstree/kvthread.cc)
    #
    # Phase 4.3: Enable more files incrementally
    add_borrow_check(src/masstree/value_versioned_array.cc)
    add_borrow_check(src/masstree/query_masstree.cc)
endif()

# Compiler flags
set(CXXFLAGS
  -w
  -Wreturn-type
  -I${MASSTREE_CONFIG_INCLUDE_DIR}
  # Parent of MASSTREE_CONFIG_INCLUDE_DIR so `#include "masstree/config.h"`
  # (the prefixed form used by src/mako/varkey.h, tuple.cc, etc.) resolves
  # to the generated config.h at <build>/generated/masstree/config.h.
  # Without this, local checkouts that happen to have a stale
  # src/masstree/config.h from an autoconf run work by accident via -Isrc;
  # fresh worktrees and Docker CI fail with "masstree/config.h not found".
  -I${CMAKE_BINARY_DIR}/generated
  -Isrc/mako
  -MD
  -MP
  -I${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4
  -Isrc
  -I${HOME}
  -DCONFIG_H=\"${CONFIG_H}\"
  -DRUSTYCPP_DISABLE_ARC_LOG  # Disable Arc debug logging
)

# Enable fiber reuse (required for Raft stability)
list(APPEND CXXFLAGS -DREUSE_FIBER)
message(STATUS "Reuse fiber enabled")

# AddressSanitizer (MAKO_ASAN=1 in env). Diagnostic toggle used to localize
# the shard2ReplicationRaft stack-corruption crash. Forces libc malloc — ASan
# replaces malloc/free, so we cannot also link jemalloc/tcmalloc. Adds
# `-fno-omit-frame-pointer` so backtraces are accurate.
set(MAKO_ASAN_ENABLED OFF)
if(DEFINED ENV{MAKO_ASAN} AND "$ENV{MAKO_ASAN}" STREQUAL "1")
    set(MAKO_ASAN_ENABLED ON)
    message(STATUS "MAKO_ASAN=1: building with -fsanitize=address; forcing USE_MALLOC_MODE=0 (libc malloc), stripping -ljemalloc.")
    set(USE_MALLOC_MODE 0 CACHE STRING "Malloc mode forced to libc by MAKO_ASAN" FORCE)
    list(APPEND CXXFLAGS -fsanitize=address -fno-omit-frame-pointer)
    list(APPEND LDFLAGS  -fsanitize=address)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# UndefinedBehaviorSanitizer (MAKO_UBSAN=1 in env). Compatible with jemalloc
# and with the other sanitizers in principle; we still keep them
# independently configurable so the matrix runs cleanly.
set(MAKO_UBSAN_ENABLED OFF)
if(DEFINED ENV{MAKO_UBSAN} AND "$ENV{MAKO_UBSAN}" STREQUAL "1")
    set(MAKO_UBSAN_ENABLED ON)
    message(STATUS "MAKO_UBSAN=1: building with -fsanitize=undefined.")
    list(APPEND CXXFLAGS -fsanitize=undefined -fno-omit-frame-pointer)
    list(APPEND LDFLAGS  -fsanitize=undefined)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()

# ThreadSanitizer (MAKO_TSAN=1 in env). Like ASan, TSan replaces malloc and is
# incompatible with jemalloc; force libc malloc. Mutually exclusive with ASan.
set(MAKO_TSAN_ENABLED OFF)
if(DEFINED ENV{MAKO_TSAN} AND "$ENV{MAKO_TSAN}" STREQUAL "1")
    if(MAKO_ASAN_ENABLED)
        message(FATAL_ERROR "MAKO_TSAN=1 cannot be combined with MAKO_ASAN=1.")
    endif()
    set(MAKO_TSAN_ENABLED ON)
    message(STATUS "MAKO_TSAN=1: building with -fsanitize=thread; forcing USE_MALLOC_MODE=0 (libc malloc), stripping -ljemalloc.")
    set(USE_MALLOC_MODE 0 CACHE STRING "Malloc mode forced to libc by MAKO_TSAN" FORCE)
    list(APPEND CXXFLAGS -fsanitize=thread -fno-omit-frame-pointer)
    list(APPEND LDFLAGS  -fsanitize=thread)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# libFuzzer coverage instrumentation (MAKO_FUZZER=1 in env). The fuzz
# *target* binary links the fuzzer runtime via -fsanitize=fuzzer (which
# provides main); every other TU is built with -fsanitize=fuzzer-no-link
# so libFuzzer can steer mutations from edge feedback throughout libmako.
# Implies MAKO_ASAN since the libFuzzer + ASan combo is the standard
# pairing — incompatible with TSan because both instrumentations use
# the same coverage hooks.
set(MAKO_FUZZER_ENABLED OFF CACHE INTERNAL "Tier 6 libFuzzer build")
if(DEFINED ENV{MAKO_FUZZER} AND "$ENV{MAKO_FUZZER}" STREQUAL "1")
    if(MAKO_TSAN_ENABLED)
        message(FATAL_ERROR "MAKO_FUZZER=1 cannot be combined with MAKO_TSAN=1.")
    endif()
    set(MAKO_FUZZER_ENABLED ON CACHE INTERNAL "Tier 6 libFuzzer build" FORCE)
    if(NOT MAKO_ASAN_ENABLED)
        set(MAKO_ASAN_ENABLED ON)
        message(STATUS "MAKO_FUZZER=1 implies MAKO_ASAN=1; enabling -fsanitize=address.")
        set(USE_MALLOC_MODE 0 CACHE STRING "Malloc mode forced to libc by MAKO_FUZZER" FORCE)
        list(APPEND CXXFLAGS -fsanitize=address -fno-omit-frame-pointer)
        list(APPEND LDFLAGS  -fsanitize=address)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
    message(STATUS "MAKO_FUZZER=1: building with -fsanitize=fuzzer-no-link for edge coverage.")
    list(APPEND CXXFLAGS -fsanitize=fuzzer-no-link)
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# Add RAFT_TEST_CORO flag if enabled
if(RAFT_TEST)
  message(STATUS "Raft lab testing fiber enabled")
  list(APPEND CXXFLAGS -DRAFT_TEST_CORO)
endif()

# These four flags used to reach the build through the (now removed) eRPC
# per-environment flag sets. They are NOT eRPC-specific and are kept verbatim:
# `-Isrc/mako` and `-march=native` are load-bearing for masstree/Silo SIMD, and
# `-DGFLAGS_IS_A_DLL=0` matches how gflags is linked here.
list(APPEND CXXFLAGS -Isrc/mako -march=native -DGFLAGS_IS_A_DLL=0)
if(GPROF)
  list(APPEND CXXFLAGS -pg -static-libstdc++ -static-libgcc)
endif()
if(DEBUG)
  list(APPEND CXXFLAGS -fno-omit-frame-pointer -DDEBUG -DSIMPLE_WORKLOAD)
else()
  list(APPEND CXXFLAGS -O2 -g)
endif()
if(CHECK_INVARIANTS)
  list(APPEND CXXFLAGS -DCHECK_INVARIANTS)
endif()
if(EVENT_COUNTERS)
  list(APPEND CXXFLAGS -DENABLE_EVENT_COUNTERS)
endif()
if(DISABLE_MULTI_VERSION)
  list(APPEND CXXFLAGS -DDISABLE_MULTI_VERSION)
endif()
if(COCO)
  list(APPEND CXXFLAGS -DCOCO)
endif()
if(MEGA_BENCHMARK)
  list(APPEND CXXFLAGS -DMEGA_BENCHMARK)
endif()
if(MEGA_BENCHMARK_MICRO)
  list(APPEND CXXFLAGS -DMEGA_BENCHMARK_MICRO)
endif()
if(TRACKING_LATENCY)
  list(APPEND CXXFLAGS -DTRACKING_LATENCY)
endif()
if(DISABLE_DISK)
  list(APPEND CXXFLAGS -DDISABLE_DISK)
endif()
if(FAIL_NEW_VERSION)
  list(APPEND CXXFLAGS -DFAIL_NEW_VERSION)
endif()
if(SIMULATE_ONE_SHARD_PER_THREAD)
  list(APPEND CXXFLAGS -DSIMULATE_ONE_SHARD_PER_THREAD)
endif()
if(TRACKING_ROLLBACK)
  list(APPEND CXXFLAGS -DTRACKING_ROLLBACK)
endif()
# CMake spells BOOL values as ON/OFF, but an undefined C preprocessor token in
# `#if` is zero. Materialize numeric values so enabling one of these options
# actually enables its guarded code.
foreach(_MAKO_BOOLEAN_DEFINE IN ITEMS STO_RMW HASHTABLE OPACITY)
  if(${_MAKO_BOOLEAN_DEFINE})
    set(${_MAKO_BOOLEAN_DEFINE}_VALUE 1)
  else()
    set(${_MAKO_BOOLEAN_DEFINE}_VALUE 0)
  endif()
endforeach()
unset(_MAKO_BOOLEAN_DEFINE)
list(APPEND CXXFLAGS
  -DREAD_MY_WRITES=${STO_RMW_VALUE}
  -DHASHTABLE=${HASHTABLE_VALUE}
  -DSTO_OPACITY=${OPACITY_VALUE}
)
# The `-include <masstree/config.h>` forced-include injects tokens at the start
# of every TU. It must NOT be applied to C++20 named module files, which
# require `module;` to be the first token. Kept as a separate flag and added
# only to targets that actually use Masstree, skipping srpc module files.
set(MASSTREE_FORCE_INCLUDE_FLAG -include ${MASSTREE_GENERATED_CONFIG_H})

# Attach Masstree's generated config to real translation units, never to a
# target. CMake forwards target compile options to its synthetic `std.cppm`
# compilation; a forced include there precedes the required leading `module;`
# declaration and makes a clean C++23 build ill-formed.
function(mako_force_include_masstree_config)
  foreach(_MASSTREE_CONFIG_SOURCE IN LISTS ARGN)
    if(_MASSTREE_CONFIG_SOURCE MATCHES "[.](cppm|ixx|mpp)$")
      message(FATAL_ERROR
        "Masstree config cannot be force-included in module source: ${_MASSTREE_CONFIG_SOURCE}")
    endif()
    get_filename_component(_MASSTREE_CONFIG_SOURCE_ABS
      "${_MASSTREE_CONFIG_SOURCE}" ABSOLUTE
      BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    # A translation unit can belong to more than one target (notably
    # dbtest.cc). Source properties are shared by those uses, so append the
    # force-include exactly once instead of growing duplicate `-include`
    # arguments as helper calls accumulate.
    get_property(_MASSTREE_CONFIG_ATTACHED
      SOURCE "${_MASSTREE_CONFIG_SOURCE_ABS}"
      PROPERTY MAKO_MASSTREE_CONFIG_ATTACHED)
    if(NOT _MASSTREE_CONFIG_ATTACHED)
      set_property(SOURCE "${_MASSTREE_CONFIG_SOURCE_ABS}" APPEND PROPERTY
        COMPILE_OPTIONS ${MASSTREE_FORCE_INCLUDE_FLAG})
      set_property(SOURCE "${_MASSTREE_CONFIG_SOURCE_ABS}" PROPERTY
        MAKO_MASSTREE_CONFIG_ATTACHED TRUE)
    endif()
  endforeach()
endfunction()

if(DEFINED LESSER_OPACITY)
  list(APPEND CXXFLAGS -DLESSER_OPACITY=${LESSER_OPACITY})
endif()
if(DEFINED GV7_OPACITY)
  list(APPEND CXXFLAGS -DGV7_OPACITY)
endif()
if(DEFINED ABORT_ON_LOCKED)
  list(APPEND CXXFLAGS -DSTO_ABORT_ON_LOCKED=${ABORT_ON_LOCKED})
endif()

# libevent flags
list(APPEND CXXFLAGS ${LIBEVENT_CFLAGS})

set(LDFLAGS
  -lpthread
  -lnuma
  -lrt
  #-lmemcached
)
if(GPROF)
  list(APPEND LDFLAGS -pg -static-libstdc++ -static-libgcc)
endif()

set(LZ4LDFLAGS
  -L${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4
  -llz4
  -Wl,-rpath,${TOP}/third-party/lz4
)

if(USE_MALLOC_MODE EQUAL 1)
  list(APPEND CXXFLAGS -DUSE_JEMALLOC)
  list(APPEND LDFLAGS -ljemalloc)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --with-malloc=jemalloc")
elseif(USE_MALLOC_MODE EQUAL 2)
  list(APPEND CXXFLAGS -DUSE_TCMALLOC)
  list(APPEND LDFLAGS -ltcmalloc)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --with-malloc=tcmalloc")
elseif(USE_MALLOC_MODE EQUAL 3)
  list(APPEND CXXFLAGS -DUSE_FLOW)
  list(APPEND LDFLAGS -lflow)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --with-malloc=flow")
else()
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --with-malloc=malloc")
endif()

if(DEFINED CUSTOM_LDPATH)
  set(LDFLAGS "${LDFLAGS} ${CUSTOM_LDPATH}")
endif()

# Same story as the CXXFLAGS block above: these C flags and link deps arrived
# via the removed eRPC flag sets but are not eRPC-specific, so they are kept
# byte-for-byte (dpdk whole-archive, pthread, numa, dl, gflags).
set(CFLAGS "")
list(APPEND CFLAGS -Isrc/mako -march=native -DGFLAGS_IS_A_DLL=0)
list(APPEND LDFLAGS
  -Wl,--whole-archive
  ${DPDK_LDFLAGS}
  -Wl,--no-whole-archive
  -lpthread
  -lnuma
  -ldl
  -lgflags
)

list(APPEND LDFLAGS
  -levent_pthreads
  -pthread
)

pkg_check_modules(LIBEVENT REQUIRED libevent)
pkg_check_modules(GFLAGS REQUIRED gflags)
#pkg_check_modules(MEMCACHED REQUIRED memcached)
pkg_check_modules(PROTOBUF REQUIRED protobuf)
pkg_check_modules(JEMALLOC REQUIRED jemalloc)
list(APPEND CFLAGS ${LIBEVENT_CFLAGS})
list(APPEND LDFLAGS ${LIBEVENT_LDFLAGS})
list(APPEND CXXFLAGS -fno-omit-frame-pointer)

# The original Silo transaction engine is retained as source-only history.
# Production Mako uses STO/MassTrans through storage/mbta_wrapper.hh. Keep the
# explicit manifest and configure-time assertion so an archived source cannot
# silently drift back into libmako. The headers also fail closed through
# src/mako/retired_silo_engine.h.
set(RETIRED_SILO_ENGINE_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/${W}/btree.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/${W}/tuple.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/${W}/txn_btree.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/${W}/txn.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/${W}/txn_proto2_impl.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/${W}/test.cc"
)
set(RETIRED_SILO_ENGINE_REALPATHS "")
foreach(RETIRED_SILO_ENGINE_SOURCE IN LISTS RETIRED_SILO_ENGINE_SOURCES)
  file(REAL_PATH "${RETIRED_SILO_ENGINE_SOURCE}"
    RETIRED_SILO_ENGINE_REALPATH)
  list(APPEND RETIRED_SILO_ENGINE_REALPATHS
    "${RETIRED_SILO_ENGINE_REALPATH}")
endforeach()

function(COLLECT_BUILDSYSTEM_TARGETS DIRECTORY_PATH OUTPUT_VARIABLE)
  get_property(LOCAL_TARGETS DIRECTORY "${DIRECTORY_PATH}"
    PROPERTY BUILDSYSTEM_TARGETS)
  set(ALL_TARGETS ${LOCAL_TARGETS})

  get_property(CHILD_DIRECTORIES DIRECTORY "${DIRECTORY_PATH}"
    PROPERTY SUBDIRECTORIES)
  foreach(CHILD_DIRECTORY IN LISTS CHILD_DIRECTORIES)
    COLLECT_BUILDSYSTEM_TARGETS("${CHILD_DIRECTORY}" CHILD_TARGETS)
    list(APPEND ALL_TARGETS ${CHILD_TARGETS})
  endforeach()

  set(${OUTPUT_VARIABLE} "${ALL_TARGETS}" PARENT_SCOPE)
endfunction()

function(ASSERT_NO_RETIRED_SILO_SOURCES)
  COLLECT_BUILDSYSTEM_TARGETS("${CMAKE_SOURCE_DIR}" ALL_BUILD_TARGETS)
  foreach(BUILD_TARGET IN LISTS ALL_BUILD_TARGETS)
    get_target_property(TARGET_SOURCE_DIR "${BUILD_TARGET}" SOURCE_DIR)
    get_target_property(TARGET_SOURCES "${BUILD_TARGET}" SOURCES)
    if(NOT TARGET_SOURCES OR TARGET_SOURCES STREQUAL "TARGET_SOURCES-NOTFOUND")
      continue()
    endif()

    foreach(TARGET_SOURCE IN LISTS TARGET_SOURCES)
      # Generator expressions cannot be canonicalized at configure time, so
      # reject any expression containing an absolute, source-root-relative,
      # or target-source-relative spelling of an archived file before the
      # normal real-path comparison below.
      foreach(RETIRED_SILO_ENGINE_SOURCE IN LISTS RETIRED_SILO_ENGINE_SOURCES)
        file(RELATIVE_PATH RETIRED_SILO_SOURCE_FROM_ROOT
          "${CMAKE_SOURCE_DIR}" "${RETIRED_SILO_ENGINE_SOURCE}")
        file(RELATIVE_PATH RETIRED_SILO_SOURCE_FROM_TARGET
          "${TARGET_SOURCE_DIR}" "${RETIRED_SILO_ENGINE_SOURCE}")
        foreach(RETIRED_SILO_SOURCE_SPELLING
            "${RETIRED_SILO_ENGINE_SOURCE}"
            "${RETIRED_SILO_SOURCE_FROM_ROOT}"
            "${RETIRED_SILO_SOURCE_FROM_TARGET}")
          string(FIND "${TARGET_SOURCE}" "${RETIRED_SILO_SOURCE_SPELLING}"
            RETIRED_SILO_SOURCE_TEXT_INDEX)
          if(NOT RETIRED_SILO_SOURCE_TEXT_INDEX EQUAL -1)
            message(FATAL_ERROR
              "Target ${BUILD_TARGET} attempts to compile the retired "
              "original Silo engine source through '${TARGET_SOURCE}': "
              "${RETIRED_SILO_ENGINE_SOURCE}")
          endif()
        endforeach()
      endforeach()

      if(TARGET_SOURCE MATCHES "\\$<" OR TARGET_SOURCE MATCHES ">")
        continue()
      endif()
      cmake_path(ABSOLUTE_PATH TARGET_SOURCE
        BASE_DIRECTORY "${TARGET_SOURCE_DIR}" NORMALIZE
        OUTPUT_VARIABLE CANONICAL_TARGET_SOURCE)
      if(EXISTS "${CANONICAL_TARGET_SOURCE}")
        file(REAL_PATH "${CANONICAL_TARGET_SOURCE}" CANONICAL_TARGET_SOURCE)
      endif()
      list(FIND RETIRED_SILO_ENGINE_REALPATHS
        "${CANONICAL_TARGET_SOURCE}" RETIRED_SILO_SOURCE_INDEX)
      if(NOT RETIRED_SILO_SOURCE_INDEX EQUAL -1)
        message(FATAL_ERROR
          "Target ${BUILD_TARGET} attempts to compile the retired original "
          "Silo engine source: ${CANONICAL_TARGET_SOURCE}")
      endif()
    endforeach()
  endforeach()
endfunction()

# Source files
set(MAKO_SRCFILES
  "${W}/allocator.cc"
  "${W}/core.cc"
  "${W}/counter.cc"
  "${W}/memory.cc"
  "${W}/rocksdb_persistence.cc"
  "${W}/rcu.cc"
  "${W}/stats_server.cc"
  "${W}/thread.cc"
  "${W}/ticker.cc"
  "${W}/varint.cc"
  # Masstree's .cc files have been lifted into the standalone
  # `masstree` static library (see src/masstree/CMakeLists.txt). They
  # are linked into libmako via target_link_libraries(mako PUBLIC
  # masstree) below.
  "${W}/silo_runtime.cc"
  "${W}/benchmarks/bench.cc"
  "${W}/benchmarks/encstress.cc"
  "${W}/benchmarks/bid.cc"
  "${W}/benchmarks/queue.cc"
  "${W}/benchmarks/tpcc.cc"
  "${W}/benchmarks/rpc_setup.cc"
  #"${W}/benchmarks/tpcc_simple.cc"
  #"${W}/benchmarks/ycsb.cc"
  "${W}/benchmarks/sync_util_init.cc"
  "${W}/sto/Transaction.cc"
  "${W}/sto/MassTrans.cc"
  "${W}/sto/TRcu.cc"
  "${W}/sto/Packer.cc"
  "${W}/sto/ReplayDB.cc"
  "${W}/sto/ThreadPool.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/lookup3.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/kv_store.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/message.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/memory.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/helper_queue.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/transport.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/fasttransport.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/srpc_rpc_backend.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/multi_transport_manager.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/configuration.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/timestamp.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/promise.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/client.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/shardClient.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/server.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/storage/abstract_db.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/storage/mtree_abi.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/cluster_bootstrap.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/rocks_interface/client_service.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/rocks_interface/client_proxy.cc"
)

set(MAKO_USING_FRAGILEC OFF)
if(CMAKE_CXX_COMPILER MATCHES "fragilec")
  set(MAKO_USING_FRAGILEC ON)
endif()

if(MAKO_USING_FRAGILEC)
  # Strict fragilec benchmark runs only need the core Masstree/library paths.
  # Exclude heavyweight benchmark modules that currently pull in unresolved
  # benchmark-config API surfaces and block `masstree_perf` builds.
  list(FILTER MAKO_SRCFILES EXCLUDE REGEX "/benchmarks/(encstress|bid|queue|tpcc|rpc_setup|sync_util_init)\\.cc$")
  list(FILTER MAKO_SRCFILES EXCLUDE REGEX "/sto/.*\\.cc$")
endif()

# Benchmark flags
set(BENCH_CXXFLAGS ${CXXFLAGS})
set(BENCH_LDFLAGS
  ${LDFLAGS}
  -lz
  -lrt
  -lcrypt
  -laio
  -ldl
  -lssl
  -lcrypto
)

# Generate deptran RPC bindings.
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.h
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/bin/rpcgen --cpp src/deptran/rcc_rpc.rpc
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.rpc
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# Create a target for the generated RPC header.
add_custom_target(rcc_rpc_gen DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.h
)

# Custom command for lz4 library - build only if missing
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4/liblz4.so")
  message(STATUS "Building LZ4 library...")
  execute_process(
    COMMAND make -C "${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4" library
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE lz4_result
  )
  if(NOT lz4_result EQUAL 0)
    message(FATAL_ERROR "Failed to build LZ4 library")
  endif()
  message(STATUS "LZ4 library built successfully")
endif()

# Build and export srpc target wiring from a dedicated CMakeLists.
# The glue lives in src/srpc-cmake so that src/srpc itself holds only the
# vendored srpc tree; the explicit binary directory keeps every generated
# artifact under <build>/src/srpc exactly where it has always been.
add_subdirectory(src/srpc-cmake src/srpc)
# Build and export masstree-specific test wiring from a dedicated CMakeLists.
add_subdirectory(src/masstree)

# Build libtxlog.so natively with CMake instead of WAF
# Collect SRPC source files (de-modularized: plain .cpp/.cc impl units).
file(GLOB SRPC_SRC
    "src/srpc/base/*.cpp"
    "src/srpc/misc/*.cpp"
    "src/srpc/reactor/*.cc"
    "src/srpc/reactor/*.S"
    "src/srpc/rpc/*.cpp"
)

# Keep the replication core manifest explicit. Recursive globs silently pulled
# retired transaction engines and test harnesses into every Mako binary, and a
# newly added compatibility source required a manual CMake reconfigure before
# it could be linked from an existing build tree.
set(DEPTRAN_CORE_SRC
    src/deptran/benchmark_control_rpc.cc
    src/deptran/communicator.cc
    src/deptran/config.cc
    src/deptran/frame.cc
    src/deptran/legacy_raft_log_payload.cc
    src/deptran/paxos/commo.cc
    src/deptran/paxos/coordinator.cc
    src/deptran/paxos/frame.cc
    src/deptran/paxos/server.cc
    src/deptran/paxos/service.cc
    src/deptran/paxos_worker.cc
    src/deptran/raft/application_log.cc
    src/deptran/raft/commo.cc
    src/deptran/raft/frame.cc
    src/deptran/raft/raft_worker.cc
    src/deptran/raft/replicated_db.cc
    src/deptran/raft/server.cc
    src/deptran/raft/service.cc
    src/deptran/replication_log_entry.cc
    src/deptran/scheduler.cc
    src/deptran/tpc_command.cc
    src/deptran/tpcc_sharding.cc
    src/deptran/view_data.cc
)

if(RAFT_TEST)
    list(APPEND DEPTRAN_CORE_SRC
        src/deptran/raft/test.cc
        src/deptran/raft/testconf.cc
    )
endif()

# Runtime replication switching: compile BOTH Paxos and Raft helpers
# The replication_helper.cc dispatcher routes calls based on runtime configuration
set(TXLOG_HELPER_SRC
    src/deptran/replication_helper.cc
    src/deptran/paxos_main_helper.cc
    src/deptran/raft_main_helper.cc
)

# ---------------------------------------------------------------------------
# `cluster` — Mako's config/sharding metadata + control-plane component,
# authored as a C++23 named module. src/cluster/*.h are module partition
# interface units (NOT headers — never #included); src/cluster/cluster.ccm is
# the primary interface unit re-exporting them; the two .cc are module
# implementation units. Mirrors the `srpc` module library: consumers write
# `import cluster;` and link this target. See src/cluster/cluster.ccm.
set(CLUSTER_MODULE_INTERFACES
    ${CMAKE_SOURCE_DIR}/src/cluster/kv_store.h
    ${CMAKE_SOURCE_DIR}/src/cluster/in_memory_kv_store.h
    ${CMAKE_SOURCE_DIR}/src/cluster/remote_kv_store.h
    ${CMAKE_SOURCE_DIR}/src/cluster/sharding_policy.h
    ${CMAKE_SOURCE_DIR}/src/cluster/sharding_policy_builder.h
    ${CMAKE_SOURCE_DIR}/src/cluster/sharding_policy_cache.h
    ${CMAKE_SOURCE_DIR}/src/cluster/shard.h
    ${CMAKE_SOURCE_DIR}/src/cluster/config_manager.h
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster_config.h
    ${CMAKE_SOURCE_DIR}/src/cluster/config_watcher.h
    ${CMAKE_SOURCE_DIR}/src/cluster/shard_manager.h
    ${CMAKE_SOURCE_DIR}/src/cluster/shard_router.h
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster.ccm
)
# .h / .ccm module units are not auto-detected as C++ (only .cc/.cpp/.cxx are);
# tell CMake they compile as C++ so the CXX_MODULES fileset schedules them.
set_source_files_properties(${CLUSTER_MODULE_INTERFACES} PROPERTIES LANGUAGE CXX)
# Module implementation units (module cluster;) — regular C++ sources.
set(CLUSTER_MODULE_IMPLS
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster_config.cc
    ${CMAKE_SOURCE_DIR}/src/cluster/shard_router.cc
)
add_library(cluster STATIC ${CLUSTER_MODULE_IMPLS})
target_sources(cluster PUBLIC
    FILE_SET cluster_modules TYPE CXX_MODULES FILES ${CLUSTER_MODULE_INTERFACES}
)
# cluster's module units include STL textually in their global module fragment
# (not `import std;`), so opt out of the global CXX_MODULE_STD (std.pcm) default.
set_target_properties(cluster PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_MODULE_STD OFF)
# src/compat rusty shims must win over third-party/rusty-cpp (see srpc's note).
target_include_directories(cluster BEFORE PUBLIC ${CMAKE_SOURCE_DIR}/src/compat)
target_include_directories(cluster PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mako          # shard_router.cc includes lib/table_registry.h
    ${CMAKE_SOURCE_DIR}/third-party/rusty-cpp/include
)
target_compile_options(cluster PUBLIC ${BENCH_CXXFLAGS})
# std_port supersedes the deleted hashbrown_port target; it PUBLIC-links
# std_port_hashbrown, so naming std_port pulls in both archives/BMIs.
target_link_libraries(cluster PUBLIC srpc pthread btree_port vec_port rc_port std_port)

# Build the replication core from the explicit manifest above.
add_library(txlog_core_obj OBJECT
    ${TXLOG_HELPER_SRC}
    ${DEPTRAN_CORE_SRC}
    # ${SRPC_SRC} removed — srpc is built as a separate library via the
    # `srpc` target (which owns the FILE_SET CXX_MODULES). Compiling
    # these .cpp/.cc files here would re-emit module interface units
    # outside a FILE_SET. Downstream binaries pick srpc up via
    # `txlog_core`'s PUBLIC link to the `srpc` target.
)
add_dependencies(txlog_core_obj rcc_rpc_gen)
# Pull in srpc's PUBLIC include dirs + module BMI paths so deptran .cc files
# that `#include "srpc/srpc.hpp"` find both the
# umbrella header and the imports it references.
target_link_libraries(txlog_core_obj PUBLIC srpc cluster)
set_target_properties(txlog_core_obj PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(txlog_core_obj PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mako/benchmarks
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/yaml-cpp/include
    ${Python3_INCLUDE_DIRS}
)
target_compile_options(txlog_core_obj PRIVATE ${BENCH_CXXFLAGS})
# Apply the masstree forced-include only to non-module sources (module files
# must have `module;` as the first token).
foreach(_SRC IN LISTS TXLOG_HELPER_SRC DEPTRAN_CORE_SRC SRPC_SRC)
    get_filename_component(_SRC_ABS "${_SRC}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set_property(SOURCE "${_SRC_ABS}" APPEND PROPERTY COMPILE_OPTIONS ${MASSTREE_FORCE_INCLUDE_FLAG})
endforeach()
target_compile_definitions(txlog_core_obj PRIVATE CONFIG_H=\"${CONFIG_H}\")

add_library(txlog_core STATIC $<TARGET_OBJECTS:txlog_core_obj>)
target_link_libraries(txlog_core
    PUBLIC
    srpc
    cluster
    yaml-cpp
    pthread
    rocksdb
    lz4
    ${CMAKE_DL_LIBS}
    Python3::Python
)
target_include_directories(txlog_core PUBLIC
    src
    src/srpc
    src/deptran
)

set(TXLOG_LINK_LIBS
    txlog_core
)
set(TXLOG_BUILD_DEPS
    txlog_core
)

# Temporary Raft-only target for borrow checking
file(GLOB RAFT_BORROW_SRC
    "src/deptran/raft/*.cc"
    "src/deptran/raft/*.cpp"
)
# Exclude test files from raft borrow checking:
# - testconf.cc: Test infrastructure with class inheritance patterns not compatible with @safe code
# - test.cc: Test harness that depends on testconf.h
# NOTE: raft_main_helper.cc is also excluded because it includes third-party headers
# (YAML, etc.) that generate 1000+ false positive violations from header code.
#   - replicated_db.cc: includes <rocksdb/c.h> third-party header that generates
#     false positive violations from header code.
list(FILTER RAFT_BORROW_SRC EXCLUDE REGEX ".*test(conf)?\\.cc$")
list(FILTER RAFT_BORROW_SRC EXCLUDE REGEX ".*replicated_db\\.cc$")
# Note: config_manager.cc / cluster_config.cc / config_watcher.cc moved to
# src/cluster/ and are no longer part of RAFT_BORROW_SRC.

if(RAFT_BORROW_SRC)
    add_library(raft_borrow OBJECT ${RAFT_BORROW_SRC})
    set_target_properties(raft_borrow PROPERTIES EXCLUDE_FROM_ALL YES)

    target_include_directories(raft_borrow PUBLIC
        src
        src/srpc
        src/deptran
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
    )
    target_compile_options(raft_borrow PUBLIC ${BENCH_CXXFLAGS})
    mako_force_include_masstree_config(${RAFT_BORROW_SRC})
    target_compile_definitions(raft_borrow PUBLIC CONFIG_H="${CONFIG_H}")

    if(ENABLE_BORROW_CHECKING AND CPP_BORROW_CHECKER)
        set(RAFT_BORROW_STAMPS)
        foreach(SOURCE ${RAFT_BORROW_SRC})
            get_filename_component(FILE_NAME ${SOURCE} NAME_WE)
            get_filename_component(SOURCE_ABS ${SOURCE} ABSOLUTE)
            set(STAMP_FILE "${CMAKE_CURRENT_BINARY_DIR}/borrow_check_raft_${FILE_NAME}.stamp")

            add_custom_command(
                OUTPUT ${STAMP_FILE}
                COMMAND ${CPP_BORROW_CHECKER} --compile-commands ${CMAKE_BINARY_DIR}/compile_commands.json ${SOURCE_ABS}
                COMMAND ${CMAKE_COMMAND} -E touch ${STAMP_FILE}
                DEPENDS ${SOURCE_ABS} ${CPP_BORROW_CHECKER}
                COMMENT "Borrow checking ${SOURCE}"
                VERBATIM
            )

            list(APPEND RAFT_BORROW_STAMPS ${STAMP_FILE})
        endforeach()

        add_custom_target(borrow_check_raft
            DEPENDS ${RAFT_BORROW_STAMPS}
            COMMENT "Running borrow checker on Raft sources only"
        )

        if(TARGET build_rusty_cpp_checker)
            add_dependencies(borrow_check_raft build_rusty_cpp_checker)
        endif()
    endif()
endif()

# Deptran worker files for borrow checking (critical files using rusty::Option/Arc)
# These files had use-after-move bugs that weren't caught because borrow checking
# was not enabled for src/deptran/
#
# NOTE: Deptran borrow checking is currently disabled due to false positives from
# recent rusty-cpp updates that enforce stricter uniform borrow rules for pointers
# and references. The following errors are triggered across multiple files:
#   - "Cannot return 'value' because it has been moved"
#   - "Cannot borrow from 'this': variable is not alive in current scope"
#   - "Cannot modify field 'm_pNode' in const method"
#   - "Cannot call method on 'this.epochs_': field is borrowed by ei"
# These appear to be false positives related to how the checker handles method
# calls on this, return value tracking, and const method field access in C++ code.
# Re-enable when these issues are resolved in rusty-cpp.
#
# Previously excluded files (for reference):
#   - raft_main_helper.cc: includes third-party headers (YAML, etc.) that generate
#     1000+ false positive violations from header code.
#   - replication_helper.cc: thin dispatcher that calls non-borrow-checked impls.
#     Uses rusty::Cell for safe interior mutability of global state.
#
# Deptran borrow checking - all files pass after rusty-cpp fixes:
# - Cell/Condvar const propagation fixed (skips @safe callees with internal @unsafe)
# - Unannotated third-party headers skipped (yaml-cpp, STL, etc.)
#
# NOTE: server_worker.cc is excluded - rusty-cpp flags srpc header functions
#       (RefCell::borrow, Option::unwrap, etc.) as violations when they appear in return
#       statements from headers included by these files. This is a false positive (the files
#       themselves are safe). TODO: Report to rusty-cpp - header functions shouldn't be checked.
set(DEPTRAN_BORROW_SRC
    src/deptran/communicator.cc
    # src/deptran/server_worker.cc  # Excluded: false positives from srpc header functions
    src/deptran/paxos_worker.cc
    src/deptran/replication_helper.cc
    src/deptran/scheduler.cc
    # Files below excluded due to violations from included headers (not the .cc files themselves):
    # src/deptran/shard_router.cc           # 51 violations from masstree/sharding headers
    # src/deptran/sharding_policy_cache.cc  # 14 violations from sharding/Mutex headers
    # src/deptran/tpcc_sharding.cc          # 23 violations from sharding_policy_builder.h headers
)

if(DEPTRAN_BORROW_SRC)
    add_library(deptran_borrow OBJECT ${DEPTRAN_BORROW_SRC})
    set_target_properties(deptran_borrow PROPERTIES EXCLUDE_FROM_ALL YES)

    target_include_directories(deptran_borrow PUBLIC
        src
        src/srpc
        src/deptran
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
        third-party/yaml-cpp/include
    )
    target_compile_options(deptran_borrow PUBLIC ${BENCH_CXXFLAGS})
    mako_force_include_masstree_config(${DEPTRAN_BORROW_SRC})
    target_compile_definitions(deptran_borrow PUBLIC CONFIG_H="${CONFIG_H}")

    # Add borrow checking for deptran files (same style as SRPC library)
    if(ENABLE_BORROW_CHECKING)
        add_borrow_check_target(deptran_borrow)

        # Create a convenience target to check only deptran files
        add_custom_target(borrow_check_deptran
            DEPENDS borrow_check_all_deptran_borrow
            COMMENT "Running borrow checker on Deptran worker sources"
        )
    endif()
endif()

# Build stamp files
add_custom_command(
  OUTPUT ${O}/${W}/buildstamp ${O}/${W}/buildstamp.bench ${O}/${W}/buildstamp.masstree
  COMMAND mkdir -p "${O}/${W}"
  COMMAND echo > "${O}/${W}/buildstamp"
  COMMAND echo > "${O}/${W}/buildstamp.bench"
  COMMAND echo > "${O}/${W}/buildstamp.masstree"
)

# Handle Masstree submodule
if(NOT EXISTS "${MASSTREE_DIR}/GNUmakefile.in")
  execute_process(COMMAND git submodule init WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  execute_process(COMMAND git submodule update WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Track MASSTREE_CONFIG changes
if(NOT "${MASSTREE_CONFIG}" STREQUAL "${DEP_MASSTREE_CONFIG}")
  execute_process(
    COMMAND mkdir -p "${O}/${W}"
    COMMAND echo > "${O}/${W}/buildstamp.masstree"
  )
  set(DEP_MASSTREE_CONFIG "${MASSTREE_CONFIG}" CACHE INTERNAL "Stored MASSTREE_CONFIG")
endif()

# Masstree dependency - not needed since we assume it's pre-configured
# set_source_files_properties(${MAKO_SRCFILES} PROPERTIES OBJECT_DEPENDS "${MASSTREE_DIR}/config.h")

# Add the mako library
add_library(mako STATIC ${MAKO_SRCFILES})
# Run after every subdirectory has been configured so no current or future
# target can hide a retired source behind target_sources() or a helper library.
cmake_language(DEFER CALL ASSERT_NO_RETIRED_SILO_SOURCES)
# masstree/compiler.hh does a bare `#include "config.h"`. Put the masstree
# generated-config dir at the FRONT of this target's include path so it
# resolves to masstree's own config.h and not src/deptran/config.h (also on
# the path), which pulls deptran/__dep__.h's `using namespace std;` and then
# collides with mako's Silo `::allocator` / `::lock_guard` under `import std;`.
# A target-scoped BEFORE slot is required: target includes outrank the
# `-I${MASSTREE_CONFIG_INCLUDE_DIR}` in CXXFLAGS. (This slot used to be filled
# — accidentally — by third-party/erpc/src/config.h.)
target_include_directories(mako BEFORE PUBLIC
    "${MASSTREE_CONFIG_INCLUDE_DIR}"
)
target_include_directories(mako PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/${MASSTREE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mako"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/lz4"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/yaml-cpp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/srpc"
    "${HOME}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include"
)
target_compile_options(mako PUBLIC ${BENCH_CXXFLAGS})
mako_force_include_masstree_config(${MAKO_SRCFILES})
target_compile_definitions(mako PUBLIC CONFIG_H=\"${CONFIG_H}\")
# Inherit srpc module BMI paths from txlog_core_obj.
target_link_libraries(mako PUBLIC txlog_core_obj masstree)
target_include_directories(mako PUBLIC ${LIBEVENT_INCLUDE_DIRS})
set_target_properties(mako PROPERTIES OUTPUT_NAME "mako")

# Inspect the archive rather than trusting the ABI's self-reported symbol
# fingerprint. The checker filters the static archive down to its externally
# visible C `mt_*` surface, so unrelated C++ symbols from Mako do not make the
# allowlist platform- or implementation-dependent.
add_custom_target(check_mtree_abi_exports
    COMMAND "${CMAKE_COMMAND}"
        "-DMAKO_NM=${CMAKE_NM}"
        "-DMAKO_ABI_ARTIFACT=$<TARGET_FILE:mako>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckMtreeAbiExports.cmake"
    DEPENDS mako
    VERBATIM
    COMMENT "Checking the native Masstree ABI exported-symbol allowlist"
)

# Rust's safe Masstree wrapper deliberately leaves the native ABI unresolved
# in ordinary Cargo builds. This opt-in target first builds the authoritative
# CMake archives (so mtree_abi.cc sees the exact same generated config and
# compile definitions as Mako), then runs the downstream Rust integration
# test against those archive paths. It is not part of ALL and does not make
# Cargo a configure-time requirement for non-Rust builds.
set(MAKO_MTREE_RUST_NATIVE_LIBS
    "static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread"
    CACHE STRING
    "Comma-separated rustc native libraries for the Masstree ABI integration test"
)
find_program(MAKO_CARGO_EXECUTABLE NAMES cargo)
if(MAKO_CARGO_EXECUTABLE)
    get_filename_component(_MAKO_CXX_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_MAKO_CXX_COMPILER_ROOT "${_MAKO_CXX_COMPILER_DIR}" DIRECTORY)

    if(APPLE)
        set(_MAKO_MTREE_LOADER_PATH_VARIABLE "DYLD_LIBRARY_PATH")
    else()
        set(_MAKO_MTREE_LOADER_PATH_VARIABLE "LD_LIBRARY_PATH")
    endif()

    set(_MAKO_STO_MASSTREE_NATIVE_TEST "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-masstree/tests/native_integration.rs")
        set(_MAKO_STO_MASSTREE_NATIVE_TEST
            "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-masstree/tests/native_integration.rs")
    endif()

    set(_MAKO_STO_MASSTREE_HISTORY_TEST "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-masstree/tests/history_oracle.rs")
        set(_MAKO_STO_MASSTREE_HISTORY_TEST
            "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-masstree/tests/history_oracle.rs")
    endif()

    set(_MAKO_STO_TPCC_NATIVE_TEST "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-tpcc-ffi/tests/native_ffi.rs")
        set(_MAKO_STO_TPCC_NATIVE_TEST
            "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-tpcc-ffi/tests/native_ffi.rs")
    endif()

    add_custom_target(rust_masstree_native_integration
        COMMAND "${CMAKE_COMMAND}"
            "-DMAKO_CARGO_EXECUTABLE=${MAKO_CARGO_EXECUTABLE}"
            "-DMAKO_RUST_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/crates/Cargo.toml"
            "-DMAKO_CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/rust-native-target"
            "-DMAKO_ARCHIVE=$<TARGET_FILE:mako>"
            "-DMASSTREE_ARCHIVE=$<TARGET_FILE:masstree>"
            "-DMAKO_CXX_RUNTIME_DIR=${_MAKO_CXX_COMPILER_ROOT}/lib"
            "-DMAKO_LOADER_PATH_VARIABLE=${_MAKO_MTREE_LOADER_PATH_VARIABLE}"
            "-DMAKO_NATIVE_LIBS=${MAKO_MTREE_RUST_NATIVE_LIBS}"
            "-DMAKO_STO_NATIVE_TEST=${_MAKO_STO_MASSTREE_NATIVE_TEST}"
            "-DMAKO_STO_HISTORY_TEST=${_MAKO_STO_MASSTREE_HISTORY_TEST}"
            "-DMAKO_STO_TPCC_NATIVE_TEST=${_MAKO_STO_TPCC_NATIVE_TEST}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunRustMasstreeNativeIntegration.cmake"
        DEPENDS check_mtree_abi_exports masstree
        USES_TERMINAL
        VERBATIM
        COMMENT "Running Rust against CMake's native Masstree ABI"
    )
else()
    message(STATUS
        "Cargo not found; rust_masstree_native_integration target is unavailable")
endif()

# ============================================================================
# BORROW CHECKER REGISTRATION SILO/STO
# ============================================================================

if(ENABLE_BORROW_CHECKING)
    message(STATUS "--- Configuring Mako Root & STO Borrow Checks ---")

    # NOTE: Mako root source files (src/mako/*.cc) and STO benchmarks are currently excluded
    # from borrow checking because they use many undeclared functions (atomics, Masstree internals, etc.)
    # Re-enable incrementally as files are properly annotated with @safe/@unsafe
    #
    # file(GLOB MAKO_ROOT_SRCS
    #     "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/*.cc"
    #     "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/*.c"
    # )
    # file(GLOB_RECURSE STO_SRCS
    #     "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/sto/*.cc"
    #     "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/sto/*.c"
    # )
    set(MAKO_ROOT_SRCS "")
    set(STO_SRCS "")

    # Combine lists
    set(TARGET_BORROW_SRCS ${MAKO_ROOT_SRCS} ${STO_SRCS})
    
    # Debug output
    list(LENGTH TARGET_BORROW_SRCS FILE_COUNT)
    message(STATUS "Found ${FILE_COUNT} files to check in src/mako/ and sto/")

    foreach(SRC ${TARGET_BORROW_SRCS})
        get_filename_component(STEM ${SRC} NAME_WE)
        set(CHECK_TARGET "borrow_check_${STEM}")

        if(NOT TARGET ${CHECK_TARGET})
            add_borrow_check(${SRC})
            add_dependencies(mako ${CHECK_TARGET})
        else()
            # If a collision still happens, just link the existing target
            message(STATUS "Linking existing borrow check target: ${CHECK_TARGET}")
            add_dependencies(mako ${CHECK_TARGET})
        endif()
    endforeach()
    
    message(STATUS "--- End Mako Borrow Checks Configuration ---")
endif()

# ------------------------------------- build apps -----------------------------------------------

# Custom targets for additional dependencies
# lz4 is handled during configuration, no need for custom target
# txlog_lib is now built directly as a CMake target, no need for custom target


function(add_apps exec_name exec_path)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs TXLOG_LIBS TXLOG_DEPS)
  cmake_parse_arguments(APP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(APP_TXLOG_LIBS)
    set(APP_TXLOG_LINK_LIBS ${APP_TXLOG_LIBS})
  else()
    set(APP_TXLOG_LINK_LIBS ${TXLOG_LINK_LIBS})
  endif()

  if(APP_TXLOG_DEPS)
    set(APP_TXLOG_BUILD_DEPS ${APP_TXLOG_DEPS})
  else()
    set(APP_TXLOG_BUILD_DEPS ${TXLOG_BUILD_DEPS})
  endif()

  # Add executable
  add_executable(${exec_name} ${exec_path})

  # Configure target
  target_include_directories(${exec_name} PRIVATE
      # masstree's config.h must outrank src/deptran/config.h (see the mako
      # target above for why).
      ${MASSTREE_CONFIG_INCLUDE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
      ${CMAKE_CURRENT_SOURCE_DIR}/${MASSTREE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran
      ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  target_compile_options(${exec_name} PRIVATE ${BENCH_CXXFLAGS}) # include all flags
  mako_force_include_masstree_config(${exec_path})

  # Link dbtest against mako library and dependencies
  target_link_libraries(${exec_name}
      mako
      rust_lib
      ${APP_TXLOG_LINK_LIBS}
      ${BENCH_LDFLAGS}
      yaml-cpp
      pthread
      numa
      event_pthreads
      rocksdb
      ${LIBEVENT_LDFLAGS}
      ${GFLAGS_LDFLAGS}
      ${PROTOBUF_LDFLAGS}
      $<$<NOT:$<BOOL:${MAKO_ASAN_ENABLED}>>:${JEMALLOC_LDFLAGS}>
  )

  # Add dependencies
  if(ENABLE_BORROW_CHECKING)
    add_dependencies(${exec_name} mako rust_lib ${APP_TXLOG_BUILD_DEPS} build_rusty_cpp_checker)
  else()
    add_dependencies(${exec_name} mako rust_lib ${APP_TXLOG_BUILD_DEPS})
  endif()
endfunction()

add_apps(dbtest src/mako/benchmarks/dbtest.cc)
add_apps(simpleTransaction examples/simpleTransaction.cc
    TXLOG_LIBS txlog_core
    TXLOG_DEPS txlog_core
)
add_apps(simpleTransactionRep examples/simpleTransactionRep.cc)
add_apps(continuousTransactions examples/continuousTransactions.cc)
add_apps(simplePaxos examples/simplePaxos.cc)
add_apps(test_rocksdb_persistence examples/test_rocksdb_persistence.cc)
add_apps(rocksdb_replay_app src/mako/benchmarks/rocksdb_replay_app.cc)
add_apps(test_callback_demo examples/test_callback_demo.cc)
add_apps(test_ordered_callbacks examples/test_ordered_callbacks.cc)
add_apps(test_partitioned_queues examples/test_partitioned_queues.cc)
add_apps(test_stress_partitioned_queues examples/test_stress_partitioned_queues.cc)
add_apps(makoCon examples/makoCon.cc)
add_apps(makoConDBBench examples/makoConDBBench.cc)
add_apps(makoConMultiTrd examples/makoConMultiTrd.cc)
# Note: makoServer functionality has been consolidated into simpleTransactionRep.cc
#       Use: ./simpleTransactionRep --server <args> for server-only mode

# Raft test executables (only when MAKO_USE_RAFT is enabled)
if(MAKO_USE_RAFT)
  if(RAFT_TEST)
    add_apps(deptran_server src/deptran/s_main.cc)
    target_sources(deptran_server PRIVATE src/deptran/server_worker.cc)
  endif()
  add_apps(simpleRaft examples/mako-raft-tests/simpleRaft.cc)
  add_apps(simpleTransactionRepRaft examples/mako-raft-tests/simpleTransactionRepRaft.cc)
  add_apps(testPreferredReplicaStartup examples/mako-raft-tests/testPreferredReplicaStartup.cc)
  add_apps(testPreferredReplicaLogReplication examples/mako-raft-tests/testPreferredReplicaLogReplication.cc)
  add_apps(testNoOps examples/mako-raft-tests/testNoOps.cc)
  add_apps(read_raft_disk src/deptran/raft/read_raft_disk.cc)
endif()

# Borrow checking for dbtest DISABLED - uses STO (Software Transactional Objects)
# headers which have reference-returning functions (back(), key()) that would
# require extensive modifications to be borrow-safe. The core masstree files
# are still borrow-checked via file(GLOB_RECURSE MASSTREE_BORROW_CHECK_SOURCES)
# if(ENABLE_BORROW_CHECKING)
#     add_borrow_check_target(dbtest)
# endif()

# Enable deptran borrow checking by default for dbtest and related apps
if(ENABLE_BORROW_CHECKING AND TARGET borrow_check_deptran)
    add_dependencies(dbtest borrow_check_deptran)
    if(TARGET deptran_server)
        add_dependencies(deptran_server borrow_check_deptran)
    endif()
endif()

add_apps(paxos_async_commit_test src/mako/benchmarks/paxos_async_commit_test.cc)
add_apps(basic src/mako/benchmarks/ut/basic.cc)
add_apps(rocksdbInterfaceTest examples/rocksdbInterfaceTest.cc)

# Matched native STO/Masstree half of the Rust comparison benchmark. Keep it
# opt-in and avoid the Rust application target. The existing mako target still
# brings its public native dependency closure into this executable.
add_executable(sto_masstree_cpp_bench EXCLUDE_FROM_ALL
    src/mako/benchmarks/sto_masstree_compare.cc
)
target_include_directories(sto_masstree_cpp_bench PRIVATE
    ${MASSTREE_CONFIG_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
    ${CMAKE_CURRENT_SOURCE_DIR}/${MASSTREE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
)
target_compile_options(sto_masstree_cpp_bench PRIVATE
    ${BENCH_CXXFLAGS}
)
mako_force_include_masstree_config(src/mako/benchmarks/sto_masstree_compare.cc)
target_compile_definitions(sto_masstree_cpp_bench PRIVATE
    CONFIG_H=\"${CONFIG_H}\"
)
target_link_libraries(sto_masstree_cpp_bench PRIVATE
    mako
    srpc
    yaml-cpp
    rocksdb
    lz4
    pthread
    ${BENCH_LDFLAGS}
    ${LIBEVENT_LDFLAGS}
)

# Matched TPC-C application with a selectable native C++ STO/Masstree or
# Rust STO/Masstree storage backend. Both modes execute the same compiled
# workload code; only the abstract_db implementation changes.
if(MAKO_CARGO_EXECUTABLE)
    set(STO_TPCC_RUST_TARGET_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/rust-sto-tpcc-target"
        CACHE PATH "Cargo target directory for the Rust STO TPC-C static library")
    set(STO_TPCC_RUST_EXTRA_RUSTFLAGS "" CACHE STRING
        "Extra rustc flags appended when building the Rust STO TPC-C static library")
    set(_STO_TPCC_RUSTFLAGS
        "-C target-cpu=native -C force-frame-pointers=yes")
    if(NOT STO_TPCC_RUST_EXTRA_RUSTFLAGS STREQUAL "")
        string(APPEND _STO_TPCC_RUSTFLAGS
            " ${STO_TPCC_RUST_EXTRA_RUSTFLAGS}")
    endif()
    set(STO_TPCC_RUST_ARCHIVE
        "${STO_TPCC_RUST_TARGET_DIR}/release/libsto_tpcc_ffi.a")
    set(STO_TPCC_RUST_MANIFESTS
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/masstree/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/masstree/build.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/mtree-sys/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-core/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-masstree/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-masstree/build.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-tpcc-ffi/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-tpcc-ffi/build.rs")
    file(GLOB_RECURSE STO_TPCC_RUST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-tpcc-ffi/*.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-core/src/*.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-masstree/src/*.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/masstree/src/*.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/mtree-sys/src/*.rs")
    add_custom_command(
        OUTPUT "${STO_TPCC_RUST_ARCHIVE}"
        COMMAND "${CMAKE_COMMAND}" -E env
            --unset=CARGO_BUILD_TARGET
            --unset=CARGO_ENCODED_RUSTFLAGS
            "CARGO_TARGET_DIR=${STO_TPCC_RUST_TARGET_DIR}"
            "CARGO_INCREMENTAL=0"
            "CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1"
            "CARGO_PROFILE_RELEASE_DEBUG=1"
            "CARGO_PROFILE_RELEASE_LTO=fat"
            "CARGO_PROFILE_RELEASE_OPT_LEVEL=3"
            "CARGO_PROFILE_RELEASE_PANIC=unwind"
            "RUSTFLAGS=${_STO_TPCC_RUSTFLAGS}"
            "${MAKO_CARGO_EXECUTABLE}" build --release --locked
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/crates/Cargo.toml"
            --package sto-tpcc-ffi
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/crates/Cargo.lock"
            ${STO_TPCC_RUST_MANIFESTS}
            ${STO_TPCC_RUST_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        USES_TERMINAL
        VERBATIM
        COMMENT "Building the Rust STO TPC-C C-ABI adapter")
    add_custom_target(sto_tpcc_rust_build DEPENDS "${STO_TPCC_RUST_ARCHIVE}")
    add_library(sto_tpcc_rust_ffi STATIC IMPORTED)
    set_target_properties(sto_tpcc_rust_ffi PROPERTIES
        IMPORTED_LOCATION "${STO_TPCC_RUST_ARCHIVE}")
    add_dependencies(sto_tpcc_rust_ffi sto_tpcc_rust_build)

    add_executable(sto_tpcc_bench EXCLUDE_FROM_ALL
        src/mako/benchmarks/dbtest.cc
        src/mako/storage/rust_sto_tpcc_wrapper.cc)
    target_include_directories(sto_tpcc_bench PRIVATE
        ${MASSTREE_CONFIG_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
        ${CMAKE_CURRENT_SOURCE_DIR}/${MASSTREE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran
        ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-tpcc-ffi/include)
    target_compile_options(sto_tpcc_bench PRIVATE
        ${BENCH_CXXFLAGS})
    mako_force_include_masstree_config(
        src/mako/benchmarks/dbtest.cc
        src/mako/storage/rust_sto_tpcc_wrapper.cc)
    target_compile_definitions(sto_tpcc_bench PRIVATE
        CONFIG_H="${CONFIG_H}"
        MAKO_RUST_STO_TPCC=1)
    target_link_libraries(sto_tpcc_bench PRIVATE
        sto_tpcc_rust_ffi
        mako
        ${TXLOG_LINK_LIBS}
        ${BENCH_LDFLAGS}
        yaml-cpp
        pthread
        $<$<PLATFORM_ID:Linux>:util>
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
        ${GFLAGS_LDFLAGS}
        ${PROTOBUF_LDFLAGS}
        $<$<NOT:$<BOOL:${MAKO_ASAN_ENABLED}>>:${JEMALLOC_LDFLAGS}>)
    add_dependencies(sto_tpcc_bench mako sto_tpcc_rust_build ${TXLOG_BUILD_DEPS})

    add_executable(sto_tpcc_cpp_wrapper_smoke EXCLUDE_FROM_ALL
        crates/sto-tpcc-ffi/tests/cpp_wrapper_fixed_read_smoke.cc
        src/mako/storage/rust_sto_tpcc_wrapper.cc)
    target_include_directories(sto_tpcc_cpp_wrapper_smoke PRIVATE
        ${MASSTREE_CONFIG_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
        ${CMAKE_CURRENT_SOURCE_DIR}/${MASSTREE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran
        ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-tpcc-ffi/include)
    target_compile_options(sto_tpcc_cpp_wrapper_smoke PRIVATE
        ${BENCH_CXXFLAGS})
    mako_force_include_masstree_config(
        crates/sto-tpcc-ffi/tests/cpp_wrapper_fixed_read_smoke.cc
        src/mako/storage/rust_sto_tpcc_wrapper.cc)
    target_compile_definitions(sto_tpcc_cpp_wrapper_smoke PRIVATE
        CONFIG_H="${CONFIG_H}"
        MAKO_RUST_STO_TPCC=1)
    target_link_libraries(sto_tpcc_cpp_wrapper_smoke PRIVATE
        sto_tpcc_rust_ffi
        mako
        ${TXLOG_LINK_LIBS}
        ${BENCH_LDFLAGS}
        yaml-cpp
        pthread
        $<$<PLATFORM_ID:Linux>:util>
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
        ${GFLAGS_LDFLAGS}
        ${PROTOBUF_LDFLAGS}
        $<$<NOT:$<BOOL:${MAKO_ASAN_ENABLED}>>:${JEMALLOC_LDFLAGS}>)
    add_dependencies(sto_tpcc_cpp_wrapper_smoke
        mako sto_tpcc_rust_build ${TXLOG_BUILD_DEPS})

    add_executable(sto_tpcc_cpp_wrapper_scan_smoke EXCLUDE_FROM_ALL
        crates/sto-tpcc-ffi/tests/cpp_wrapper_scan_smoke.cc
        src/mako/storage/rust_sto_tpcc_wrapper.cc)
    target_include_directories(sto_tpcc_cpp_wrapper_scan_smoke PRIVATE
        ${MASSTREE_CONFIG_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
        ${CMAKE_CURRENT_SOURCE_DIR}/${MASSTREE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran
        ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-tpcc-ffi/include)
    target_compile_options(sto_tpcc_cpp_wrapper_scan_smoke PRIVATE
        ${BENCH_CXXFLAGS})
    mako_force_include_masstree_config(
        crates/sto-tpcc-ffi/tests/cpp_wrapper_scan_smoke.cc
        src/mako/storage/rust_sto_tpcc_wrapper.cc)
    target_compile_definitions(sto_tpcc_cpp_wrapper_scan_smoke PRIVATE
        CONFIG_H="${CONFIG_H}"
        MAKO_RUST_STO_TPCC=1)
    target_link_libraries(sto_tpcc_cpp_wrapper_scan_smoke PRIVATE
        sto_tpcc_rust_ffi
        mako
        ${TXLOG_LINK_LIBS}
        ${BENCH_LDFLAGS}
        yaml-cpp
        pthread
        $<$<PLATFORM_ID:Linux>:util>
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
        ${GFLAGS_LDFLAGS}
        ${PROTOBUF_LDFLAGS}
        $<$<NOT:$<BOOL:${MAKO_ASAN_ENABLED}>>:${JEMALLOC_LDFLAGS}>)
    add_dependencies(sto_tpcc_cpp_wrapper_scan_smoke
        mako sto_tpcc_rust_build ${TXLOG_BUILD_DEPS})
else()
    message(STATUS "Cargo not found; sto_tpcc_bench target is unavailable")
endif()

# ============================================================================
# Test Configuration
# ============================================================================

# Enable testing
enable_testing()

if(TARGET sto_tpcc_cpp_wrapper_smoke)
    add_test(NAME test_sto_tpcc_cpp_wrapper_smoke
        COMMAND sto_tpcc_cpp_wrapper_smoke
            "${CMAKE_CURRENT_SOURCE_DIR}/config/mako_sto_tpcc_local.yml")
    set_tests_properties(test_sto_tpcc_cpp_wrapper_smoke PROPERTIES
        LABELS "sto;masstree;rust;ffi")
endif()

if(TARGET sto_tpcc_cpp_wrapper_scan_smoke)
    add_test(NAME test_sto_tpcc_cpp_wrapper_scan_smoke
        COMMAND sto_tpcc_cpp_wrapper_scan_smoke
            "${CMAKE_CURRENT_SOURCE_DIR}/config/mako_sto_tpcc_local.yml")
    set_tests_properties(test_sto_tpcc_cpp_wrapper_scan_smoke PROPERTIES
        LABELS "sto;masstree;rust;ffi")
endif()

# C consumers must be able to include the public ABI without any C++
# compatibility layer. This executable contains no ABI calls and therefore
# compiles and links independently of libmako and Masstree.
add_executable(test_mtree_abi_c11_header EXCLUDE_FROM_ALL
    tests/mtree_abi_c11_header.c)
target_include_directories(test_mtree_abi_c11_header PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
set_target_properties(test_mtree_abi_c11_header PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    CXX_MODULE_STD OFF
    LINKER_LANGUAGE C
)
target_compile_options(test_mtree_abi_c11_header PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang,GNU>:-Wall;-Wextra;-Wpedantic;-Werror>
)
add_test(NAME test_mtree_abi_c11_header COMMAND test_mtree_abi_c11_header)
set_tests_properties(test_mtree_abi_c11_header PROPERTIES LABELS "masstree;abi;c11")

# Keep the Rust STO public wrapper ABI independently consumable by strict C11
# code. Like the Masstree header check above, this target requires no runtime
# library because it verifies declarations and layout only.
add_executable(test_sto_tpcc_ffi_c11_header EXCLUDE_FROM_ALL
    tests/sto_tpcc_ffi_c11_header.c)
target_include_directories(test_sto_tpcc_ffi_c11_header PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-tpcc-ffi/include"
)
set_target_properties(test_sto_tpcc_ffi_c11_header PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    CXX_MODULE_STD OFF
    LINKER_LANGUAGE C
)
target_compile_options(test_sto_tpcc_ffi_c11_header PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang,GNU>:-Wall;-Wextra;-Wpedantic;-Werror>
)
add_test(NAME test_sto_tpcc_ffi_c11_header
    COMMAND test_sto_tpcc_ffi_c11_header)
set_tests_properties(test_sto_tpcc_ffi_c11_header PROPERTIES
    LABELS "sto;rust;ffi;c11")

# This target is the build half of the Rust STO/Masstree CI gate. Its
# dependencies compile every opt-in C/C++ consumer and run Rust against the
# native archives. The final command covers the rest of the Rust workspace,
# whose ordinary tests deliberately run without native link settings. The
# sto-tpcc-ffi unit-test binary is the exception: its exported entry points
# retain native Masstree calls, so the native runner executes that binary with
# the CMake archive closure instead of relying on linker dead-code grouping.
if(MAKO_CARGO_EXECUTABLE)
    add_custom_target(rust_sto_integration
        COMMAND "${CMAKE_COMMAND}" -E env
            --unset=CARGO_BUILD_TARGET
            --unset=CARGO_ENCODED_RUSTFLAGS
            "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/rust-workspace-target"
            "${MAKO_CARGO_EXECUTABLE}" clippy
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/crates/Cargo.toml"
            --workspace
            --all-targets
            --all-features
            --locked
            --
            -D warnings
        COMMAND "${CMAKE_COMMAND}" -E env
            --unset=CARGO_BUILD_TARGET
            --unset=CARGO_ENCODED_RUSTFLAGS
            "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/rust-workspace-target"
            "${MAKO_CARGO_EXECUTABLE}" test
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/crates/Cargo.toml"
            --workspace
            --exclude sto-tpcc-ffi
            --all-targets
            --all-features
            --locked
        DEPENDS
            check_mtree_abi_exports
            rust_masstree_native_integration
            sto_tpcc_bench
            sto_tpcc_cpp_wrapper_smoke
            sto_tpcc_cpp_wrapper_scan_smoke
            test_mtree_abi_c11_header
            test_sto_tpcc_ffi_c11_header
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        USES_TERMINAL
        VERBATIM
        COMMENT "Checking the Rust STO workspace and native integration"
    )
endif()

# Use vendored GoogleTest only (no system gtest dependency).
set(BUILD_TESTS OFF)
set(FRAGILE_GTEST_FROM_SOURCE OFF)
set(MAKO_GTEST_ROOT "")
set(MAKO_GTEST_MAIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/tests/fragile_gtest_main.cc)

# Vendored GoogleTest under third-party/googletest.
foreach(_gtest_candidate
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/googletest/googletest
)
    if(EXISTS ${_gtest_candidate}/src/gtest-all.cc AND EXISTS ${_gtest_candidate}/include/gtest/gtest.h)
        set(MAKO_GTEST_ROOT ${_gtest_candidate})
        break()
    endif()
endforeach()

if(NOT EXISTS ${MAKO_GTEST_MAIN_SRC})
    message(WARNING "GoogleTest main stub missing at ${MAKO_GTEST_MAIN_SRC}. Tests will be disabled.")
elseif(MAKO_GTEST_ROOT STREQUAL "")
    message(WARNING "Vendored GoogleTest not found under third-party/googletest. Tests will be disabled.")
else()
    set(MAKO_GTEST_SRC ${MAKO_GTEST_ROOT}/src/gtest-all.cc)
    if(CMAKE_CXX_COMPILER MATCHES "(/|^)fragilec$")
        set(FRAGILE_GTEST_FROM_SOURCE ON)
        set(MAKO_GTEST_LIB_TARGET fragile_transpiled_gtest)
        set(MAKO_GTEST_MAIN_TARGET fragile_transpiled_gtest_main)
    else()
        set(MAKO_GTEST_LIB_TARGET mako_gtest)
        set(MAKO_GTEST_MAIN_TARGET mako_gtest_main)
    endif()

    if(NOT TARGET ${MAKO_GTEST_LIB_TARGET})
        add_library(${MAKO_GTEST_LIB_TARGET} STATIC ${MAKO_GTEST_SRC})
        target_include_directories(${MAKO_GTEST_LIB_TARGET} PUBLIC
            ${MAKO_GTEST_ROOT}/include
            ${MAKO_GTEST_ROOT}
        )
        if(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
            target_include_directories(${MAKO_GTEST_LIB_TARGET} PRIVATE
                ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}
            )
        endif()
        target_compile_options(${MAKO_GTEST_LIB_TARGET} PRIVATE ${BENCH_CXXFLAGS})
        mako_force_include_masstree_config(${MAKO_GTEST_SRC})
        target_compile_definitions(${MAKO_GTEST_LIB_TARGET} PUBLIC GTEST_HAS_PTHREAD=1)
        target_link_libraries(${MAKO_GTEST_LIB_TARGET} PUBLIC pthread)
    endif()

    if(NOT TARGET ${MAKO_GTEST_MAIN_TARGET})
        add_library(${MAKO_GTEST_MAIN_TARGET} STATIC ${MAKO_GTEST_MAIN_SRC})
        target_include_directories(${MAKO_GTEST_MAIN_TARGET} PUBLIC
            ${MAKO_GTEST_ROOT}/include
            ${MAKO_GTEST_ROOT}
        )
        if(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
            target_include_directories(${MAKO_GTEST_MAIN_TARGET} PRIVATE
                ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}
            )
        endif()
        target_compile_options(${MAKO_GTEST_MAIN_TARGET} PRIVATE ${BENCH_CXXFLAGS})
        mako_force_include_masstree_config(${MAKO_GTEST_MAIN_SRC})
        target_compile_definitions(${MAKO_GTEST_MAIN_TARGET} PUBLIC GTEST_HAS_PTHREAD=1)
        target_link_libraries(${MAKO_GTEST_MAIN_TARGET} PUBLIC ${MAKO_GTEST_LIB_TARGET} pthread)
    endif()

    set(GTEST_LIBRARIES ${MAKO_GTEST_LIB_TARGET})
    set(GTEST_MAIN_LIBRARIES ${MAKO_GTEST_MAIN_TARGET})
    set(GTEST_INCLUDE_DIRS ${MAKO_GTEST_ROOT}/include ${MAKO_GTEST_ROOT})
    set(BUILD_TESTS ON)
    message(STATUS "GoogleTest configured from vendored source: ${MAKO_GTEST_ROOT}")
endif()

if(BUILD_TESTS)
    # Common test include directories
    set(TEST_INCLUDE_DIRS
        src
        src/srpc
        src/srpc/tests
        src/mako
        test
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include
    )

    # Common test compile options
    set(TEST_COMPILE_OPTIONS ${BENCH_CXXFLAGS})

    # Common test link libraries
    set(TEST_LINK_LIBS
        srpc
        pthread
        ${BENCH_LDFLAGS}
        ${LIBEVENT_LDFLAGS}
    )
    set(RPC_TEST_LINK_LIBS
        srpc
        pthread
        ${BENCH_LDFLAGS}
        ${LIBEVENT_LDFLAGS}
    )



    # Marshal Microbenchmark (no gtest — standalone main; prints ns/op).
    # Measures the Vec<uint8_t>+read_pos Marshal — same harness used
    # during the chunk-list → Vec rewrite (see

    # MakoClientService Unit Tests
    add_executable(test_client_service tests/test_client_service.cc)
    target_include_directories(test_client_service PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_client_service PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_client_service ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_client_service COMMAND test_client_service)

    # Sharding Policy Tests
    #
    # The policy value types now serialize via their srpc Serializable
    # save()/load() DSL methods (BinaryWriteArchive), so the old free-function
    # srpc::Marshal operators (sharding_policy_marshal.{h,cc}) are gone.
    # sharding_policy.h pulls in the srpc.serializable module directly, so this
    # test just compiles the one source file.
    add_executable(test_sharding_policy
        tests/sharding_policy_test.cc
    )
    target_include_directories(test_sharding_policy PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_sharding_policy PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_sharding_policy cluster ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_sharding_policy COMMAND test_sharding_policy)

    # Sharding Policy Cache Tests (requires txlog bench/core for ConfigClient)
    add_executable(test_sharding_policy_cache tests/sharding_policy_cache_test.cc)
    target_include_directories(test_sharding_policy_cache PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/deptran)
    target_compile_options(test_sharding_policy_cache PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_sharding_policy_cache ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_sharding_policy_cache COMMAND test_sharding_policy_cache)

    # Shard Router Integration Tests (requires txlog bench/core for ShardingPolicyCache)
    add_executable(test_shard_router tests/shard_router_test.cc)
    target_include_directories(test_shard_router PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/deptran src/mako)
    target_compile_options(test_shard_router PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_shard_router ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_shard_router COMMAND test_shard_router)

    # TPC-C Sharding Integration Tests (requires txlog bench/core for ShardingPolicyCache)
    add_executable(test_tpcc_sharding tests/tpcc_sharding_test.cc)
    target_include_directories(test_tpcc_sharding PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/deptran src/mako)
    target_compile_options(test_tpcc_sharding PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_tpcc_sharding ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_tpcc_sharding COMMAND test_tpcc_sharding)


    # ConfigKvService server-handler read-logic tests (Stage 3d). The
    # ConfigKvServiceImpl handler is generated from rcc_rpc.rpc, so this
    # links the txlog stack (srpc); it exercises DoReadConfigKey over an
    # InMemoryKvStore (no socket). The wire transport is CI-verified.
    add_executable(test_config_kv_service tests/config_kv_service_test.cc)
    target_include_directories(test_config_kv_service PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/deptran)
    target_compile_options(test_config_kv_service PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_config_kv_service ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_config_kv_service COMMAND test_config_kv_service)

    # ConfigManager standalone unit tests
    #
    # Truly standalone: links ONLY the three cluster .cc sources + gtest.
    # ConfigManager talks to the cluster-owned KvStore port (get/put/
    # remove, stdlib only), so this binary needs no storage engine, no
    # masstree config, no CONFIG_H, no txlog_core / srpc / RocksDB.
    # The mako binding (OrderedIndexKvStore) is verified separately by
    # test_ordered_index_kv_store below.
    add_executable(test_config_manager
        tests/config_manager_test.cc
    )
    target_include_directories(test_config_manager PRIVATE
        src
        ${GTEST_INCLUDE_DIRS}
    )
    # The brew clang ships its own libc++ but does not add its lib dir
    # to the linker search path automatically; do it explicitly so the
    # gtest archive's `std::__hash_memory` reference resolves against
    # the matching libc++ (system libc++ is older and lacks it).
    get_filename_component(_CXX_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_CXX_COMPILER_ROOT "${_CXX_COMPILER_DIR}" DIRECTORY)
    target_link_options(test_config_manager PRIVATE
        "-L${_CXX_COMPILER_ROOT}/lib"
        "-Wl,-rpath,${_CXX_COMPILER_ROOT}/lib"
    )
    target_link_libraries(test_config_manager
        cluster srpc ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} c++ pthread
    )
    add_test(NAME test_config_manager COMMAND test_config_manager)

    # ShardManager control-plane test — stub Shards (in-memory KV stand-ins
    # for masstree shards) driven through the REAL ConfigManager/ClusterConfig
    # reconfiguration verbs (add/kill/remove). Pure cluster like
    # test_config_manager: no storage engine, no RPC.
    add_executable(test_shard_manager
        tests/shard_manager_test.cc
    )
    target_include_directories(test_shard_manager PRIVATE
        src
        ${GTEST_INCLUDE_DIRS}
    )
    target_link_options(test_shard_manager PRIVATE
        "-L${_CXX_COMPILER_ROOT}/lib"
        "-Wl,-rpath,${_CXX_COMPILER_ROOT}/lib"
    )
    target_link_libraries(test_shard_manager
        cluster srpc ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} c++ pthread
    )
    add_test(NAME test_shard_manager COMMAND test_shard_manager)

    # OrderedIndexKvStore adapter test — the cluster<->mako binding seam.
    # Unlike test_config_manager (pure cluster), this TU pulls the
    # storage interface header + an in-memory FullOrderedIndex fake, so
    # it needs src/mako on the include path plus the masstree-config /
    # CONFIG_H plumbing that abstract_ordered_index.h transitively wants.
    # It still links NO storage library (the interface is pure-virtual;
    # the fake provides the vtable).
    add_executable(test_ordered_index_kv_store
        tests/ordered_index_kv_store_test.cc
    )
    target_include_directories(test_ordered_index_kv_store PRIVATE
        src
        src/mako
        ${MASSTREE_CONFIG_INCLUDE_DIR}
        ${CMAKE_BINARY_DIR}/generated
        ${GTEST_INCLUDE_DIRS}
    )
    target_link_options(test_ordered_index_kv_store PRIVATE
        "-L${_CXX_COMPILER_ROOT}/lib"
        "-Wl,-rpath,${_CXX_COMPILER_ROOT}/lib"
    )
    target_link_libraries(test_ordered_index_kv_store
        cluster ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} c++ pthread
    )
    target_compile_definitions(test_ordered_index_kv_store PRIVATE CONFIG_H=\"${CONFIG_H}\")
    add_test(NAME test_ordered_index_kv_store COMMAND test_ordered_index_kv_store)

    # RPC Tests
    # test_rpc only exercises SRPC RPC paths, so keep its linkage minimal.
    if(ENABLE_BORROW_CHECKING AND TARGET borrow_check_srpc)
        # Ensure regular test builds also gate on SRPC borrow-check status.
    endif()






    # Masstree-specific tests and benchmarks are defined in
    # src/masstree/CMakeLists.txt as a deferred function so they only
    # fire after BUILD_TESTS / TEST_INCLUDE_DIRS / etc. are ready.
    mako_add_masstree_tests()




    # Arc<Mutex<>> Thread Safety Tests
    add_executable(test_arc_mutex_thread tests/test_arc_mutex_thread.cc)
    target_include_directories(test_arc_mutex_thread PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_arc_mutex_thread PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_arc_mutex_thread ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_arc_mutex_thread COMMAND test_arc_mutex_thread)



    # NOTE: test_transport_backend and stress_transport_backend were deleted
    # together with the TransportBackend abstraction. Both exercised only a
    # MockTransportBackend defined inside the test file itself (against a local
    # copy of the TransportType enum), i.e. the interface's own dispatch and no
    # srpc code at all. Real srpc/rpc coverage lives in test_transport_integration


    # ============================================================================
    # RPC Reliability Unit Tests (Phase 7.1)




























    # 4g4: test_rpc_client_channel_switch deleted along with the
    # SRPC_USE_CHANNEL/SRPC_DISABLE_CHANNEL migration switch and its
    # test-only override helpers. Channel mode is unconditional now.

    add_executable(test_raft_messages tests/raft_messages_test.cc)
    target_include_directories(test_raft_messages PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_raft_messages PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_raft_messages ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread srpc)
    add_test(NAME test_raft_messages COMMAND test_raft_messages)

    add_executable(test_raft_application_log tests/raft_application_log_test.cc)
    target_include_directories(test_raft_application_log PRIVATE
        ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_raft_application_log PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_raft_application_log
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread txlog_core)
    add_test(NAME test_raft_application_log COMMAND test_raft_application_log)

    add_executable(test_legacy_raft_log tests/legacy_raft_log_test.cc)
    target_include_directories(test_legacy_raft_log PRIVATE
        ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_legacy_raft_log PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_legacy_raft_log
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread txlog_core)
    add_test(NAME test_legacy_raft_log COMMAND test_legacy_raft_log)

    add_executable(test_raft_transport_facade tests/raft_transport_facade_test.cc)
    target_include_directories(test_raft_transport_facade PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_raft_transport_facade PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_raft_transport_facade ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread srpc)
    add_test(NAME test_raft_transport_facade COMMAND test_raft_transport_facade)

    add_executable(test_raft_srpc_transport_compile tests/raft_srpc_transport_compile_test.cc)
    target_include_directories(test_raft_srpc_transport_compile PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS}
        # raft_srpc_transport_compile_test.cc transitively pulls in
        # deptran headers and `__dep__.h`, which includes
        # `yaml-cpp/yaml.h`. Neither path is propagated through `srpc`,
        # so add both explicitly here (same pattern as
        # `txlog_core_obj`'s PUBLIC include set).
        ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/yaml-cpp/include)
    target_compile_options(test_raft_srpc_transport_compile PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_raft_srpc_transport_compile ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread srpc)
    add_test(NAME test_raft_srpc_transport_compile COMMAND test_raft_srpc_transport_compile)

    # Phase 8.1a — RaftQuorum<Reply> primitive (header-only template).
    # Bootstraps an srpc::Reactor on the test thread to exercise the
    # IntEvent-backed wait_until_quorum path; no heavy raft state needed.
    add_executable(test_raft_quorum tests/raft_quorum_test.cc)
    target_include_directories(test_raft_quorum PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_raft_quorum PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_raft_quorum ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread srpc)
    add_test(NAME test_raft_quorum COMMAND test_raft_quorum)

    add_executable(test_raft_dispatcher_facade tests/raft_dispatcher_facade_test.cc)
    target_include_directories(test_raft_dispatcher_facade PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_raft_dispatcher_facade PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_raft_dispatcher_facade ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread srpc)
    add_test(NAME test_raft_dispatcher_facade COMMAND test_raft_dispatcher_facade)

    add_executable(test_raft_channel_transport tests/raft_channel_transport_test.cc)
    target_include_directories(test_raft_channel_transport PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_raft_channel_transport PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_raft_channel_transport ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread srpc)
    add_test(NAME test_raft_channel_transport COMMAND test_raft_channel_transport)

    add_executable(test_raft_memory_snapshot_manager tests/raft_memory_snapshot_manager_test.cc)
    target_include_directories(test_raft_memory_snapshot_manager PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_raft_memory_snapshot_manager PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_raft_memory_snapshot_manager ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread srpc)
    add_test(NAME test_raft_memory_snapshot_manager COMMAND test_raft_memory_snapshot_manager)

    add_executable(test_raft_test_cluster tests/raft_test_cluster_test.cc)
    target_include_directories(test_raft_test_cluster PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_raft_test_cluster PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_raft_test_cluster ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread srpc)
    add_test(NAME test_raft_test_cluster COMMAND test_raft_test_cluster)

    add_executable(raft_lab_standalone src/deptran/raft/raft_lab_standalone.cc)
    target_include_directories(raft_lab_standalone PRIVATE ${TEST_INCLUDE_DIRS})
    target_compile_options(raft_lab_standalone PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(raft_lab_standalone pthread txlog_core_obj yaml-cpp rocksdb lz4 ${CMAKE_DL_LIBS} Python3::Python)
    add_test(NAME raft_lab_standalone COMMAND raft_lab_standalone)


    # Service proxy facade guard tests (Phase 2 leaf 1)

    find_program(PYTHON3_EXECUTABLE python3 REQUIRED)
    find_program(CLANGXX_EXECUTABLE clang++)
    if(CLANGXX_EXECUTABLE)
        set(RPC_DOCS_SNIPPET_CXX ${CLANGXX_EXECUTABLE})
    else()
        set(RPC_DOCS_SNIPPET_CXX ${CMAKE_CXX_COMPILER})
    endif()



    # Request Queue Tests (Phase 2.1)



    # ============================================================================
    # RPC Reliability Integration Tests (Phase 7.2)













    # Transaction Timeout Tests
    add_executable(test_txn_timeout tests/deptran/txn_timeout_test.cc)
    target_include_directories(test_txn_timeout PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_txn_timeout PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_txn_timeout ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_txn_timeout COMMAND test_txn_timeout)

    # Stress Crash Recovery Tests (Phase 7.3.1)
    # NOTE: Stress tests take too long for regular CI (10+ minutes each).
    # They are labeled "stress" and excluded from ctest by default.


    # Integration tests from CI
    add_test(NAME simpleTransaction COMMAND simpleTransaction)


    # simplePaxos requires config generation and running a shell script that launches multiple processes
    # add_test(NAME simplePaxos
    #     COMMAND bash -c "bash ./src/mako/update_config.sh && bash ./examples/simplePaxos.sh"
    #     WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    # )
    # # Use RESOURCE_LOCK to prevent port conflicts with other Paxos tests (ports 17001-17xxx)

    # shard1ReplicationSimple tests 1-shard replication using simpleTransactionRep
    # add_test(NAME shard1ReplicationSimple
    #     COMMAND bash -c "bash ./examples/test_1shard_replication_simple.sh"
    #     WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    # )
    # # Use RESOURCE_LOCK to prevent port conflicts with other Paxos tests (ports 17001-17xxx)

    # Add a convenience target to run all tests
    # The src/srpc/tests corpus belongs to srpc and is built and run there;
    # mako no longer builds any of it, so this convenience target names only
    # mako's own tests.
    add_custom_target(run_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS test_masstree
                test_silo_varint test_silo_rcu_thread test_silo_allocator
                test_sto_transaction test_sto_transaction_real
                simpleTransaction simplePaxos shard1ReplicationSimple
        COMMENT "Running all tests (Masstree + live Silo support/STO)"
    )
    if(ENABLE_BORROW_CHECKING AND TARGET borrow_check_srpc)
        add_dependencies(run_tests borrow_check_srpc)
    endif()
    add_dependencies(run_tests srpc_goal0_dual_compile)

    # Add borrow checking for test files if enabled
    # NOTE: Test borrow checking is currently disabled due to false positives
    # from recent rusty-cpp updates. The same issues affecting SRPC files also
    # affect test files:
    #   - "Cannot return 'value' because it has been moved"
    #   - "Cannot borrow from 'this': variable is not alive in current scope"
    # Re-enable when these issues are resolved in rusty-cpp.
    if(ENABLE_BORROW_CHECKING)
        # Disabled due to false positives:
        # add_borrow_check_target(test_marshal)
        # add_borrow_check_target(test_rpc)
        # add_borrow_check_target(test_future)
        # add_borrow_check_target(test_reactor)
        # Create an empty convenience target
        add_custom_target(borrow_check_tests
            COMMAND ${CMAKE_COMMAND} -E echo "Test borrow checking disabled - see CMakeLists.txt comments"
            COMMENT "Test borrow checking disabled pending rusty-cpp fixes"
        )
    endif()

    # ============================================================================
    # LIVE SILO SUPPORT + STO TESTS (merged from tests/ directory)
    #
    # "Silo" here names shared support such as allocator, RCU, and
    # SiloRuntime. The original Silo transaction engine is quarantined above.

    # Test: Silo Varint (Utility)
    add_executable(test_silo_varint tests/test_silo_varint.cc)
    target_link_libraries(test_silo_varint
        mako
        srpc
        ${TXLOG_LINK_LIBS}
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(test_silo_varint PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME SiloVarintTests COMMAND test_silo_varint)

    # Test: Silo RCU + Thread
    add_executable(test_silo_rcu_thread tests/test_silo_rcu_thread.cc)
    target_link_libraries(test_silo_rcu_thread
        mako
        srpc
        ${TXLOG_LINK_LIBS}
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(test_silo_rcu_thread PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME SiloRCUThreadTests COMMAND test_silo_rcu_thread)

    # Test: allocator support retained by the STO/MassTrans runtime
    add_executable(test_silo_allocator tests/test_silo_allocator.cc)
    target_link_libraries(test_silo_allocator
        mako
        srpc
        ${TXLOG_LINK_LIBS}
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(test_silo_allocator PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME SiloAllocatorTests COMMAND test_silo_allocator)

    # Test: SiloRuntime per-site isolation (moved out of src/masstree/tests
    # because its subject matter is the SiloRuntime, not Masstree).
    add_executable(test_silo_runtime tests/test_silo_runtime.cc)
    target_include_directories(test_silo_runtime PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS}
    )
    target_compile_options(test_silo_runtime PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_silo_runtime
        mako ${TEST_LINK_LIBS}
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES}
        pthread)
    add_test(NAME test_silo_runtime COMMAND test_silo_runtime)

    # Test: stable C ABI over the native SiloRuntime/Masstree configuration.
    # Linking through mako ensures the ABI implementation is compiled with the
    # exact same generated Masstree config, definitions, and native options as
    # the storage engine it wraps.
    add_executable(test_mtree_abi tests/test_mtree_abi.cc)
    target_include_directories(test_mtree_abi PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS}
    )
    target_compile_options(test_mtree_abi PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_mtree_abi
        mako ${TEST_LINK_LIBS}
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES}
        pthread)
    add_test(NAME test_mtree_abi COMMAND test_mtree_abi)
    set_tests_properties(test_mtree_abi PROPERTIES LABELS "masstree;silo;abi")

    add_test(NAME test_mtree_abi_exports
        COMMAND "${CMAKE_COMMAND}"
            "-DMAKO_NM=${CMAKE_NM}"
            "-DMAKO_ABI_ARTIFACT=$<TARGET_FILE:mako>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckMtreeAbiExports.cmake"
    )
    set_tests_properties(test_mtree_abi_exports PROPERTIES LABELS "masstree;abi;exports")

    add_custom_target(run_mtree_abi_test
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "^test_mtree_abi$"
        DEPENDS test_mtree_abi
        COMMENT "Running the Masstree C ABI test"
    )

    # Test: Multi-site Silo stress.
    add_executable(test_silo_multi_site_stress tests/test_silo_multi_site_stress.cc)
    target_include_directories(test_silo_multi_site_stress PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS}
    )
    target_compile_options(test_silo_multi_site_stress PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_silo_multi_site_stress
        mako ${TEST_LINK_LIBS}
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES}
        pthread)
    add_test(NAME test_silo_multi_site_stress COMMAND test_silo_multi_site_stress)

    # Test: non-transactional (Masstree-shape) API on MassTrans /
    # abstract_ordered_index / mbta_sharded_ordered_index — the gating
    # tests for docs/silo-masstree-api-unification.md.
    add_executable(test_silo_nontxn_api tests/test_silo_nontxn_api.cc)
    target_include_directories(test_silo_nontxn_api PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS}
    )
    target_compile_options(test_silo_nontxn_api PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_silo_nontxn_api
        mako ${TEST_LINK_LIBS}
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES}
        yaml-cpp rocksdb lz4 numa event_pthreads
        pthread)
    add_test(NAME test_silo_nontxn_api COMMAND test_silo_nontxn_api)

    # Test: ConfigManager over a REAL mbta index (integration).
    # Unlike test_config_manager (pure cluster, std::map fake), this
    # drives ConfigManager / ClusterConfig through the OrderedIndexKvStore
    # adapter onto a real Silo/STO mbta index, with the mbta static_init +
    # thread_init bring-up. Validates the storage path the eventual dbtest
    # bootstrap relies on. Links the mako storage lib + the three cluster
    # .cc sources (ConfigManager/ClusterConfig/ConfigWatcher live there,
    # not in the mako lib).
    add_executable(test_config_manager_mbta
        tests/config_manager_mbta_integration_test.cc
    )
    target_include_directories(test_config_manager_mbta PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS}
    )
    target_compile_options(test_config_manager_mbta PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_config_manager_mbta
        cluster mako ${TEST_LINK_LIBS}
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES}
        yaml-cpp rocksdb lz4 numa event_pthreads
        pthread)
    add_test(NAME test_config_manager_mbta COMMAND test_config_manager_mbta)

    # Test: the consolidated three-backend story — one TYPED_TEST body
    # drives masstree_ordered_index / mbta_ordered_index /
    # mbta_sharded_ordered_index through abstract_ordered_index*
    # (docs/mako-nontxn-api-plan.md Phase 5, as revised: no separate
    # KV vocabulary; backends are picked at construction).
    add_executable(test_kv_backends tests/test_kv_backends.cc)
    target_include_directories(test_kv_backends PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS}
    )
    target_compile_options(test_kv_backends PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_kv_backends
        mako ${TEST_LINK_LIBS}
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES}
        yaml-cpp rocksdb lz4 numa event_pthreads
        pthread)
    add_test(NAME test_kv_backends COMMAND test_kv_backends)

    # Test: distributed (two-shards-in-one-process) non-txn API — the
    # gating tests for docs/mako-nontxn-api-plan.md. Exercises the
    # nontxnPut/Insert/Remove RPCs + remoteGet end-to-end over the
    # loopback fake transport.
    add_executable(test_mako_nontxn_distributed tests/test_mako_nontxn_distributed.cc)
    # Out-of-tree build dirs have no ./src or ../src — bake the source root.
    target_compile_definitions(test_mako_nontxn_distributed PRIVATE
        MAKO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_include_directories(test_mako_nontxn_distributed PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS}
    )
    target_compile_options(test_mako_nontxn_distributed PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_mako_nontxn_distributed
        mako ${TEST_LINK_LIBS}
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES}
        yaml-cpp rocksdb lz4 numa event_pthreads
        pthread)
    add_test(NAME test_mako_nontxn_distributed COMMAND test_mako_nontxn_distributed)

    # Reproducer for sanitizer-findings.md Finding 6 (Mako allocator
    # cap). Aborts by design — gated on MAKO_REPRO_FINDING6=1 so it
    # is not picked up by ctest.
    if(DEFINED ENV{MAKO_REPRO_FINDING6} AND "$ENV{MAKO_REPRO_FINDING6}" STREQUAL "1")
        add_executable(repro_finding6 tests/repro_finding6.cc)
        target_include_directories(repro_finding6 PRIVATE ${TEST_INCLUDE_DIRS})
        target_compile_options(repro_finding6 PRIVATE ${TEST_COMPILE_OPTIONS})
        target_link_libraries(repro_finding6 mako ${TEST_LINK_LIBS} pthread)
    endif()

    # Test: STO Transaction (TRcu)
    add_executable(test_sto_transaction tests/test_sto_transaction.cc)
    target_link_libraries(test_sto_transaction
        mako
        srpc
        ${TXLOG_LINK_LIBS}
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(test_sto_transaction PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    mako_force_include_masstree_config(tests/test_sto_transaction.cc)
    add_test(NAME STOTransactionTests COMMAND test_sto_transaction)

    # Test: STO Transaction Real (Integration)
    add_executable(test_sto_transaction_real tests/test_sto_transaction_real.cc)
    target_link_libraries(test_sto_transaction_real
        mako
        srpc
        ${TXLOG_LINK_LIBS}
        ${GTEST_LIBRARIES}
        ${GTEST_MAIN_LIBRARIES}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(test_sto_transaction_real PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME STOTransactionRealTests COMMAND test_sto_transaction_real)

    # Custom target to run live Silo-support and STO tests
    add_custom_target(run_silo_sto_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "Silo|STO"
        DEPENDS test_silo_varint test_silo_rcu_thread test_silo_allocator
                test_sto_transaction test_sto_transaction_real
        COMMENT "Running live Silo-support and STO tests"
    )

    # Keep this summary honest: it must name only targets this file actually
    # defines. It went stale in 4a06ef0e (which stopped building srpc's
    # `src/srpc/tests/` corpus) and then told CI logs that `test_timer`,
    # `test_marshal`, `test_rpc`, `test_rpc_extended`, `test_future`,
    # `test_reactor`, `test_reactor_extended` and `rpcbench` were configured
    # when ninja had no such targets.
    message(STATUS "Test targets configured:")
    message(STATUS "  Masstree Tests:")
    message(STATUS "    - test_masstree: Core Masstree b-tree regression tests")
    message(STATUS "  Live Silo-support/STO Tests:")
    message(STATUS "    - test_silo_varint: Varint encoding/decoding (22 tests)")
    message(STATUS "    - test_silo_rcu_thread: RCU memory management (9 tests)")
    message(STATUS "    - test_silo_allocator: allocator support (9 tests)")
    message(STATUS "    - test_sto_transaction: STO Transaction RCU (13 tests)")
    message(STATUS "    - test_sto_transaction_real: STO Transaction integration tests")
    message(STATUS "Additional perf binaries:")
    message(STATUS "  - masstree_perf: On-demand Masstree throughput benchmark (JSON output)")
    message(STATUS "")
    message(STATUS "Run all tests with: make run_tests")
    message(STATUS "Run live Silo-support/STO tests: make run_silo_sto_tests")
    message(STATUS "Or individually: make test && ctest")
endif()
