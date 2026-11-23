cmake_minimum_required(VERSION 3.30)

set(MAKO_BUILD_PROFILE "MAKO" CACHE STRING "Build profile: MAKO or ROCKSDB_RAFT")
set_property(CACHE MAKO_BUILD_PROFILE PROPERTY STRINGS MAKO ROCKSDB_RAFT)
if(MAKO_BUILD_PROFILE STREQUAL "ROCKSDB_RAFT")
    project(mako_rocksdb_raft LANGUAGES NONE)
    include(cmake/RocksdbRaft.cmake)
    return()
elseif(NOT MAKO_BUILD_PROFILE STREQUAL "MAKO")
    message(FATAL_ERROR "Unknown MAKO_BUILD_PROFILE: ${MAKO_BUILD_PROFILE}")
endif()

# `import std;` is enabled via `src/srpc/std_compat.hpp`. The header
# textually includes every C++ standard header AND then `import std;`s.
# Clang's libc++ rejects `import std; ... #include <vector>` but
# accepts the reverse order, so the include block comes first; that
# also turns later transitive `#include`s from rusty-cpp/yaml-cpp/etc.
# into no-ops via the header guard. The UUID gates CMake's
# experimental `import std;` materialization and must be set BEFORE
# `project()`.
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

# Suppress CMake policy warnings for deprecated modules
# CMP0167: FindBoost module is removed in CMake 3.30+, but we still support older CMake
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)  # Use deprecated FindBoost for compatibility
endif()

# Mako pins its compiler to Clang 22.x. Clang 21 and earlier
# frontend-crash compiling reactor.cpp's C++20-module codegen (the
# `srpc.reactor` named module); Clang 22 fixes it — see the explicit
# version check after project() below.
# The reasons for requiring >= 22:
#   * Clang 21 SIGSEGVs in the frontend on srpc.reactor's module codegen.
#   * Clang 20 and earlier lack alias-template CTAD support that
#     rusty-cpp's platform::threading aliases depend on.
#   * CI standardizes on clang-22 (apt.llvm.org noble repo).
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

# Raft source-provenance gate. The Raft server is canonical Rust built into
# libraft_rt.a; the C++ carriers that still hold inline Rust DSL blocks
# compile the committed C++ generated next to each block. This target proves
# those bytes are a fresh rendering of their blocks, that the shell crate's
# extracted modules and lib.rs match the blocks, and that the crate builds and
# passes clippy with -D warnings.
add_custom_target(raft_goal0_source_gate
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/raft-dsl-cargo
        bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/raft_dsl.sh"
        --check
        --transpiler "${RUSTYCPP_TRANSPILER}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking Raft inline Rust DSL provenance"
    USES_TERMINAL
    VERBATIM
)
add_dependencies(raft_goal0_source_gate build_rusty_cpp_transpiler)

# NOTE: RustyCppSubmodule.cmake already computes CPP_BORROW_CHECKER and creates
# build targets. Do not override checker paths here or Docker CARGO_TARGET_DIR
# support is bypassed.
if(ENABLE_BORROW_CHECKING)
    enable_borrow_checking()
endif()

find_package(PkgConfig REQUIRED)
include(cmake/MakoAllocator.cmake)
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
# Cargo is a configure-time requirement: the native sharding staticlib, the
# Raft runtime and SRPC's crate mode (SRPC_CARGO_EXECUTABLE in
# src/srpc-cmake) are all Cargo builds that every Mako executable links.
# -DMAKO_CARGO_EXECUTABLE=<path> selects a specific Cargo (the Rust STO
# sanitizer lanes pass their toolchain's own); every Cargo archive that
# reaches dbtest is built with this one executable.
find_program(MAKO_CARGO_EXECUTABLE NAMES cargo REQUIRED)
set(_MAKO_RUST_LIB_CARGO_EXECUTABLE "${MAKO_CARGO_EXECUTABLE}")
set(MAKO_RUST_STO_RUSTUP_TOOLCHAIN "" CACHE STRING
    "Rustup toolchain for Cargo targets selected by Rust STO sanitizer builds")
set(MAKO_RUST_STO_TARGET_TRIPLE "" CACHE STRING
    "Explicit Cargo target for Rust STO integration builds")
set(MAKO_RUST_STO_SANITIZER_RUSTFLAGS "" CACHE STRING
    "Rust flags confined to sanitizer-instrumented Rust STO targets")
# One Rust std per executable. A Cargo staticlib carries a copy of the std it
# was compiled against, and an executable that links two copies with
# different crate hashes fails with "multiple definition of
# `rust_eh_personality'". libmako.a brings two such staticlibs (native
# sharding, the Raft runtime), so every Rust archive and Cargo test
# executable that is linked with it must use the same std. Ordinary builds
# get that for free from the toolchain's prebuilt std. The ASan and TSan
# lanes of scripts/ci/run_rust_sto_sanitizer.sh instrument std as well, and
# -Zbuild-std cannot keep one std there: it hashes std by each invocation's
# Cargo profile (opt-level, panic strategy, ...), and those lanes link
# release staticlibs into dev-profile test executables. The gate therefore
# rebuilds the pinned nightly's std ONCE, instrumented, into a private
# sysroot, names it here, and passes `--sysroot=<it>` in
# MAKO_RUST_STO_SANITIZER_RUSTFLAGS, which every Cargo command of such a
# build uses, together with an explicit --target so that build scripts and
# proc-macros keep the toolchain's std. Like the prebuilt std, the sysroot
# holds both panic runtimes, so panic = "abort" crates (sharding, Raft) and
# panic = "unwind" crates link against the same std. A directory configured
# with it is for the gate's own targets.
set(MAKO_RUST_STO_SYSROOT "" CACHE PATH
    "Instrumented Rust sysroot shared by every Cargo command of a Rust STO sanitizer build")
set(_MAKO_RUST_STO_SYSROOT_ENV "")
set(_MAKO_RUST_STO_SYSROOT_TARGET_ARGS "")
if(NOT "${MAKO_RUST_STO_SYSROOT}" STREQUAL "")
    if(NOT IS_ABSOLUTE "${MAKO_RUST_STO_SYSROOT}"
            OR MAKO_RUST_STO_SYSROOT MATCHES "[ \t;]")
        message(FATAL_ERROR
            "MAKO_RUST_STO_SYSROOT must be an absolute path without "
            "whitespace (Cargo splits RUSTFLAGS on spaces): "
            "${MAKO_RUST_STO_SYSROOT}")
    endif()
    if(MAKO_RUST_STO_TARGET_TRIPLE STREQUAL ""
            OR MAKO_RUST_STO_RUSTUP_TOOLCHAIN STREQUAL "")
        message(FATAL_ERROR
            "MAKO_RUST_STO_SYSROOT requires MAKO_RUST_STO_TARGET_TRIPLE and "
            "MAKO_RUST_STO_RUSTUP_TOOLCHAIN")
    endif()
    file(GLOB _MAKO_RUST_STO_SYSROOT_STD
        "${MAKO_RUST_STO_SYSROOT}/lib/rustlib/${MAKO_RUST_STO_TARGET_TRIPLE}/lib/libstd-*.rlib")
    list(LENGTH _MAKO_RUST_STO_SYSROOT_STD _MAKO_RUST_STO_SYSROOT_STD_COUNT)
    if(NOT _MAKO_RUST_STO_SYSROOT_STD_COUNT EQUAL 1)
        message(FATAL_ERROR
            "MAKO_RUST_STO_SYSROOT must hold exactly one std for "
            "${MAKO_RUST_STO_TARGET_TRIPLE}; found "
            "${_MAKO_RUST_STO_SYSROOT_STD_COUNT} under ${MAKO_RUST_STO_SYSROOT}")
    endif()
    separate_arguments(_MAKO_RUST_STO_SYSROOT_FLAGS UNIX_COMMAND
        "${MAKO_RUST_STO_SANITIZER_RUSTFLAGS}")
    list(FILTER _MAKO_RUST_STO_SYSROOT_FLAGS INCLUDE REGEX "^--sysroot")
    if(NOT _MAKO_RUST_STO_SYSROOT_FLAGS STREQUAL
            "--sysroot=${MAKO_RUST_STO_SYSROOT}"
            OR NOT MAKO_RUST_STO_SANITIZER_RUSTFLAGS MATCHES
                "(^|[ \t])-Zsanitizer=(address|thread)($|[ \t])")
        message(FATAL_ERROR
            "MAKO_RUST_STO_SYSROOT requires MAKO_RUST_STO_SANITIZER_RUSTFLAGS "
            "with -Zsanitizer=address or thread and exactly one "
            "--sysroot=${MAKO_RUST_STO_SYSROOT}")
    endif()
    set(_MAKO_RUST_STO_SYSROOT_ENV
        "RUSTFLAGS=${MAKO_RUST_STO_SANITIZER_RUSTFLAGS}")
    set(_MAKO_RUST_STO_SYSROOT_TARGET_ARGS
        --target "${MAKO_RUST_STO_TARGET_TRIPLE}")
    message(STATUS
        "Rust STO sanitizer sysroot for every Cargo staticlib: "
        "${MAKO_RUST_STO_SYSROOT}")
endif()

# Respect CARGO_TARGET_DIR environment variable if set (for Docker builds)
if(DEFINED ENV{CARGO_TARGET_DIR})
    if("$ENV{CARGO_TARGET_DIR}" STREQUAL "")
        message(FATAL_ERROR "CARGO_TARGET_DIR must not be empty")
    endif()
    get_filename_component(RUST_LIB_TARGET_BASE "$ENV{CARGO_TARGET_DIR}"
        ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust-lib")
    message(STATUS "Using CARGO_TARGET_DIR for rust-lib: ${RUST_LIB_TARGET_BASE}")
else()
    set(RUST_LIB_TARGET_BASE "${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/target")
endif()
set(RUST_LIB_DIR "${RUST_LIB_TARGET_BASE}/release")
set(RUST_LIB_NAME "rust_redis")
set(RUST_LIB_PATH "${RUST_LIB_DIR}/lib${RUST_LIB_NAME}.a")
set(_MAKO_RUST_LIB_TOOLCHAIN_ENV "")
if(NOT "${MAKO_RUST_STO_RUSTUP_TOOLCHAIN}" STREQUAL "")
    list(APPEND _MAKO_RUST_LIB_TOOLCHAIN_ENV
        "RUSTUP_TOOLCHAIN=${MAKO_RUST_STO_RUSTUP_TOOLCHAIN}")
endif()
file(GLOB_RECURSE RUST_LIB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/src/*.rs")

if("${MAKO_RUST_STO_SYSROOT}" STREQUAL "")
    set(_MAKO_RUST_LIB_BUILD_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${RUST_LIB_TARGET_BASE}"
            ${_MAKO_RUST_LIB_TOOLCHAIN_ENV}
            "${_MAKO_RUST_LIB_CARGO_EXECUTABLE}" build --release --locked)
else()
    # dbtest links this archive next to libmako.a's Rust staticlibs, so it
    # uses the sanitizer sysroot too (MAKO_RUST_STO_SYSROOT above). --target
    # moves Cargo's output under <triple>/; the copy keeps the one path that
    # rust_lib imports.
    set(_MAKO_RUST_LIB_BUILD_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${RUST_LIB_TARGET_BASE}"
            ${_MAKO_RUST_LIB_TOOLCHAIN_ENV}
            ${_MAKO_RUST_STO_SYSROOT_ENV}
            "${_MAKO_RUST_LIB_CARGO_EXECUTABLE}" build --release --locked
            ${_MAKO_RUST_STO_SYSROOT_TARGET_ARGS}
        COMMAND "${CMAKE_COMMAND}" -E copy
            "${RUST_LIB_TARGET_BASE}/${MAKO_RUST_STO_TARGET_TRIPLE}/release/lib${RUST_LIB_NAME}.a"
            "${RUST_LIB_PATH}")
endif()

# Use add_custom_command instead of add_custom_target to avoid rebuilding every time
add_custom_command(
    OUTPUT ${RUST_LIB_PATH}
    ${_MAKO_RUST_LIB_BUILD_COMMANDS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/Cargo.lock
            ${CMAKE_CURRENT_SOURCE_DIR}/rust-lib/build.rs
            ${RUST_LIB_SOURCES}
    COMMENT "Building Rust library if needed"
    VERBATIM
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

    # Resolve libc++ from the toolchain's own lib directory, ahead of any
    # system copy. Homebrew's LLVM keg puts libc++.so.1 where the driver does
    # not search by default; a stale system libc++ would otherwise disagree
    # with Clang 22 about out-of-line symbols such as __hash_memory.
    get_filename_component(_MAKO_CLANG_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_MAKO_CLANG_ROOT "${_MAKO_CLANG_BIN}" DIRECTORY)
    set(_MAKO_LIBCXX_DIR "${_MAKO_CLANG_ROOT}/lib")
    if (EXISTS "${_MAKO_LIBCXX_DIR}/libc++.so.1")
        message(STATUS "Using toolchain libc++ from ${_MAKO_LIBCXX_DIR}")
        set(CMAKE_EXE_LINKER_FLAGS
            "${CMAKE_EXE_LINKER_FLAGS} -L${_MAKO_LIBCXX_DIR} -Wl,-rpath,${_MAKO_LIBCXX_DIR}")
        set(CMAKE_SHARED_LINKER_FLAGS
            "${CMAKE_SHARED_LINKER_FLAGS} -L${_MAKO_LIBCXX_DIR} -Wl,-rpath,${_MAKO_LIBCXX_DIR}")
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
# Raft disk persistence: a build switch,
# not a run-time one. OFF is the memory-mode binary with the disk code
# compiled out; ON turns on the raft crates' cargo feature `raft_disk`.
option(MAKO_RAFT_DISK "Build the Raft write-ahead log and recovery (disk mode)" OFF)
option(SINGLE_RAFT_INSTANCE "Default runtime Raft group mode (ON=single group, OFF=one group per partition)" ON)
option(MAKO_LOCAL_TEST_HOOKS
       "Enable local test-only commit, cleanup, and timestamp-clock seams" OFF)

if(MAKO_LOCAL_TEST_HOOKS)
  add_compile_definitions(MAKO_LOCAL_TEST_HOOKS=1)
  message(STATUS "Local test-only commit, cleanup, and timestamp-clock seams enabled")
endif()

if(MAKO_USE_RAFT)
  add_compile_definitions(MAKO_USE_RAFT=1)
  message(STATUS "Mako Raft replication enabled")
endif()

if(RAFT_TEST)
  add_compile_definitions(RAFT_TEST_CORO=1)
  message(STATUS "Raft lab testing fiber enabled (note: REUSE_FIBER is always enabled)")
endif()

if(MAKO_RAFT_DISK)
  message(STATUS "Raft disk mode enabled (cargo feature raft_disk)")
endif()

if(SINGLE_RAFT_INSTANCE)
  add_compile_definitions(RAFT_DEFAULT_SINGLE_GROUP=1)
  message(STATUS "Default runtime Raft group mode: single (override with --raft-groups or MAKO_RAFT_GROUP_MODE)")
else()
  message(STATUS "Default runtime Raft group mode: multi (override with --raft-groups or MAKO_RAFT_GROUP_MODE)")
endif()

set(USE_MALLOC_MODE 1 CACHE STRING "Malloc mode: 0=libc, 1=jemalloc, 2=tcmalloc, 3=flow")
set(MODE "perf" CACHE STRING "Build mode: perf, backoff, factor-gc, factor-gc-nowriteinplace, factor-fake-compression, sandbox")
option(STO_RMW "Enable STO read-modify-write" ON)
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

# Preserve sanitizer selection across CMake's automatic regeneration. Legacy
# MAKO_*=0/1 environment toggles override the cached choice when explicitly
# supplied; otherwise the cache is authoritative for the build directory.
function(mako_resolve_build_toggle toggle description)
    option(${toggle} "${description}" OFF)
    if(DEFINED ENV{${toggle}})
        if("$ENV{${toggle}}" STREQUAL "1")
            set(${toggle} ON CACHE BOOL "${description}" FORCE)
        elseif("$ENV{${toggle}}" STREQUAL "0")
            set(${toggle} OFF CACHE BOOL "${description}" FORCE)
        else()
            message(FATAL_ERROR
                "${toggle} environment value must be 0 or 1, got: "
                "$ENV{${toggle}}")
        endif()
    endif()
endfunction()

mako_resolve_build_toggle(MAKO_ASAN "Build native targets with AddressSanitizer")
mako_resolve_build_toggle(MAKO_UBSAN "Build native targets with UndefinedBehaviorSanitizer")
mako_resolve_build_toggle(MAKO_TSAN "Build native targets with ThreadSanitizer")
mako_resolve_build_toggle(MAKO_FUZZER "Build the Masstree libFuzzer target")

# AddressSanitizer (MAKO_ASAN=1 in env or -DMAKO_ASAN=ON). Diagnostic toggle used to localize
# the shard2ReplicationRaft stack-corruption crash. Forces libc malloc — ASan
# replaces malloc/free, so we cannot also link jemalloc/tcmalloc. Adds
# `-fno-omit-frame-pointer` so backtraces are accurate.
set(MAKO_ASAN_ENABLED ${MAKO_ASAN})
if(MAKO_ASAN_ENABLED)
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
set(MAKO_UBSAN_ENABLED ${MAKO_UBSAN})
if(MAKO_UBSAN_ENABLED)
    message(STATUS "MAKO_UBSAN=1: building with -fsanitize=undefined.")
    list(APPEND CXXFLAGS -fsanitize=undefined -fno-omit-frame-pointer)
    list(APPEND LDFLAGS  -fsanitize=undefined)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()

# ThreadSanitizer (MAKO_TSAN=1 in env). Like ASan, TSan replaces malloc and is
# incompatible with jemalloc; force libc malloc. Mutually exclusive with ASan.
set(MAKO_TSAN_ENABLED ${MAKO_TSAN})
if(MAKO_TSAN_ENABLED)
    if(MAKO_ASAN_ENABLED)
        message(FATAL_ERROR "MAKO_TSAN=1 cannot be combined with MAKO_ASAN=1.")
    endif()
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
set(MAKO_FUZZER_ENABLED ${MAKO_FUZZER} CACHE INTERNAL
    "Tier 6 libFuzzer build" FORCE)
if(MAKO_FUZZER_ENABLED)
    if(MAKO_TSAN_ENABLED)
        message(FATAL_ERROR "MAKO_FUZZER=1 cannot be combined with MAKO_TSAN=1.")
    endif()
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

# Native Rust test executables link libmako.a outside CMake. An instrumented
# static archive does not carry its sanitizer runtime with it, so propagate the
# exact CMake sanitizer set to Cargo's final Clang++ link. Keep this separate
# from Rust's own Miri gate: Item 4 instruments the C++ integration boundary,
# while Miri interprets the Rust-only fake-ABI ownership model.
set(MAKO_LOCAL_SANITIZERS_ENABLED)
set(MAKO_LOCAL_SANITIZER_RUNTIME_ENV)
if(MAKO_ASAN_ENABLED)
    list(APPEND MAKO_LOCAL_SANITIZERS_ENABLED address)
    list(APPEND MAKO_LOCAL_SANITIZER_RUNTIME_ENV
         "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1"
         "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/src/mako/mako_local_lsan_suppressions.txt:print_suppressions=1")
endif()
if(MAKO_UBSAN_ENABLED)
    list(APPEND MAKO_LOCAL_SANITIZERS_ENABLED undefined)
    list(APPEND MAKO_LOCAL_SANITIZER_RUNTIME_ENV
         "UBSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/src/masstree/ubsan_suppressions.txt:halt_on_error=1:print_stacktrace=1")
endif()
if(MAKO_TSAN_ENABLED)
    list(APPEND MAKO_LOCAL_SANITIZERS_ENABLED thread)
    list(APPEND MAKO_LOCAL_SANITIZER_RUNTIME_ENV
         "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/src/masstree/tsan_suppressions.txt:halt_on_error=1:print_suppressions=1")
endif()
if(MAKO_LOCAL_SANITIZERS_ENABLED)
    list(JOIN MAKO_LOCAL_SANITIZERS_ENABLED "," MAKO_LOCAL_SANITIZER_LIST)
    set(MAKO_LOCAL_SANITIZER_FLAG "-fsanitize=${MAKO_LOCAL_SANITIZER_LIST}")
    set(MAKO_LOCAL_SANITIZER_RUSTFLAGS "$ENV{RUSTFLAGS}")
    set(MAKO_LOCAL_SANITIZER_CARGO_PREFIX)
    set(MAKO_LOCAL_SANITIZER_CARGO_TARGET_ARGS)
    if(MAKO_TSAN_ENABLED)
        list(LENGTH MAKO_LOCAL_SANITIZERS_ENABLED
             MAKO_LOCAL_SANITIZER_COUNT)
        if(NOT MAKO_LOCAL_SANITIZER_COUNT EQUAL 1)
            message(FATAL_ERROR
                "mako-local Rust TSan must run without another sanitizer")
        endif()
        if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
            message(FATAL_ERROR
                "mako-local Rust TSan currently supports x86_64 Linux only")
        endif()
        execute_process(
            COMMAND bash
                    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_mako_local_miri.sh"
                    --print-toolchain
            OUTPUT_VARIABLE MAKO_LOCAL_RUST_TSAN_TOOLCHAIN
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE MAKO_LOCAL_RUST_TSAN_TOOLCHAIN_STATUS)
        if(NOT MAKO_LOCAL_RUST_TSAN_TOOLCHAIN_STATUS EQUAL 0 OR
           MAKO_LOCAL_RUST_TSAN_TOOLCHAIN STREQUAL "")
            message(FATAL_ERROR
                "failed to resolve the pinned mako-local Rust TSan toolchain")
        endif()

        # Linking Clang TSan into a prebuilt, uninstrumented Rust std/libtest
        # produces false races in Rust's lock-free synchronization internals.
        # Rebuild the complete Rust side under the same dated nightly instead.
        string(APPEND MAKO_LOCAL_SANITIZER_RUSTFLAGS
               " -Zsanitizer=thread -C linker=${CMAKE_CXX_COMPILER}")
        list(APPEND MAKO_LOCAL_SANITIZER_CARGO_PREFIX
             "+${MAKO_LOCAL_RUST_TSAN_TOOLCHAIN}" "-Zbuild-std")
        list(APPEND MAKO_LOCAL_SANITIZER_CARGO_TARGET_ARGS
             "--target" "x86_64-unknown-linux-gnu")
    else()
        string(APPEND MAKO_LOCAL_SANITIZER_RUSTFLAGS
               " -C linker=${CMAKE_CXX_COMPILER}"
               " -C link-arg=${MAKO_LOCAL_SANITIZER_FLAG}")
    endif()
    string(STRIP "${MAKO_LOCAL_SANITIZER_RUSTFLAGS}"
           MAKO_LOCAL_SANITIZER_RUSTFLAGS)
    message(STATUS
        "mako-local sanitizer gate enabled: ${MAKO_LOCAL_SANITIZER_LIST}")
endif()

# libmako.a calls into two Cargo staticlibs, native sharding and the Raft
# runtime, and each carries a copy of the Rust std it was compiled against.
# An executable can hold only one Rust std: when two copies have different
# crate hashes, the linker extracts both and their unmangled symbols collide
# ("multiple definition of `rust_eh_personality'"). Ordinarily one toolchain
# builds everything, so the copies are the same prebuilt std. The mako-local
# TSan gate is the exception: its Cargo test executables use a std that
# -Zbuild-std rebuilds with -Zsanitizer=thread under the pinned nightly, and
# Cargo hashes that std by the Cargo profile it was built with (opt-level,
# panic strategy, ...; RUSTFLAGS do not enter the hash). With this switch the
# two staticlibs are built exactly like those test executables -- same Cargo,
# toolchain, -Zbuild-std, --target and RUSTFLAGS, and the dev profile with
# panic = "unwind" that `cargo test` gives std -- so every executable of the
# gate links one std, instrumented, and the staticlibs' own code is
# instrumented too. In that mode they are unoptimized and keep debug
# assertions and overflow checks.
#
# Only `./ci/ci.sh makoLocalSanitizerGates` (MAKO_LOCAL_SANITIZER=tsan) turns
# this on. A plain MAKO_TSAN=1 build keeps the ordinary release staticlibs, so
# executables that also link other Cargo staticlibs built with the ordinary
# toolchain (dbtest's rust_redis, the Rust STO TPC-C adapter) still link. The
# Rust STO sanitizer lanes select their own toolchain and std for these
# staticlibs (MAKO_RUST_STO_RUSTUP_TOOLCHAIN and MAKO_RUST_STO_SYSROOT above)
# and cannot use this switch.
option(MAKO_LOCAL_TSAN_RUST_STATICLIBS
    "Build the native sharding and Raft staticlibs against the mako-local Rust TSan std (mako-local TSan gate only)"
    OFF)
set(_MAKO_RUST_STATICLIB_TSAN_STD OFF)
if(MAKO_LOCAL_TSAN_RUST_STATICLIBS)
    if(NOT MAKO_TSAN_ENABLED)
        message(FATAL_ERROR
            "MAKO_LOCAL_TSAN_RUST_STATICLIBS=ON requires MAKO_TSAN=1")
    endif()
    if(NOT "${MAKO_RUST_STO_RUSTUP_TOOLCHAIN}" STREQUAL ""
            OR NOT "${MAKO_RUST_STO_SYSROOT}" STREQUAL ""
            OR NOT "${MAKO_RUST_STO_SANITIZER_RUSTFLAGS}" STREQUAL "")
        message(FATAL_ERROR
            "MAKO_LOCAL_TSAN_RUST_STATICLIBS=ON cannot be combined with the "
            "Rust STO sanitizer configuration (MAKO_RUST_STO_*)")
    endif()
    set(_MAKO_RUST_STATICLIB_TSAN_STD ON)
    # The test command's `--target <triple>`; Cargo writes the staticlibs
    # under <target-dir>/<triple>/debug.
    list(GET MAKO_LOCAL_SANITIZER_CARGO_TARGET_ARGS 1
         _MAKO_RUST_STATICLIB_TSAN_TRIPLE)
    message(STATUS
        "Native sharding and Raft staticlibs: mako-local Rust TSan std "
        "(${MAKO_LOCAL_RUST_TSAN_TOOLCHAIN}, -Zbuild-std, dev profile)")
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
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --with-malloc=jemalloc")
elseif(USE_MALLOC_MODE EQUAL 2)
  list(APPEND CXXFLAGS -DUSE_TCMALLOC)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --with-malloc=tcmalloc")
elseif(USE_MALLOC_MODE EQUAL 3)
  list(APPEND CXXFLAGS -DUSE_FLOW)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --with-malloc=flow")
elseif(USE_MALLOC_MODE EQUAL 0)
  set(MASSTREE_CONFIG "${MASSTREE_CONFIG} --with-malloc=malloc")
else()
  message(FATAL_ERROR
    "USE_MALLOC_MODE must be exactly 0, 1, 2, or 3; got '${USE_MALLOC_MODE}'")
endif()

# Resolve the selected process allocator once. Its INTERFACE link contract is
# attached PUBLICly to libmako below, and its generated metadata is consumed by
# Cargo when Rust links the same static archive outside CMake.
mako_configure_allocator()

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
list(APPEND CFLAGS ${LIBEVENT_CFLAGS})
list(APPEND LDFLAGS ${LIBEVENT_LDFLAGS})
list(APPEND CXXFLAGS -fno-omit-frame-pointer)

# The Rust transaction facade must never link against a differently configured
# native archive closure. Generate one identity translation unit from the
# effective compile database and compile it as part of libmako, under the exact
# same target settings as the facade.
find_program(MAKO_LOCAL_PYTHON_EXECUTABLE NAMES python3 REQUIRED)
set(MAKO_LOCAL_FINGERPRINT_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/mako_local_fingerprint.py")
set(MAKO_LOCAL_BUILD_IDENTITY_CPP
    "${CMAKE_BINARY_DIR}/generated/mako_local_build_identity.cc")
set(MAKO_LOCAL_BUILD_MANIFEST
    "${CMAKE_BINARY_DIR}/generated/mako_local_build_manifest.json")
set(MAKO_LOCAL_BUILD_IDENTITY_DEPFILE
    "${CMAKE_BINARY_DIR}/generated/mako_local_build_identity.d")
add_custom_command(
  OUTPUT
    "${MAKO_LOCAL_BUILD_IDENTITY_CPP}"
    "${MAKO_LOCAL_BUILD_MANIFEST}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
  COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}"
          "${MAKO_LOCAL_FINGERPRINT_SCRIPT}"
          generate
          --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
          --build-dir "${CMAKE_BINARY_DIR}"
          --manifest "${MAKO_LOCAL_BUILD_MANIFEST}"
          --cpp-out "${MAKO_LOCAL_BUILD_IDENTITY_CPP}"
          --depfile "${MAKO_LOCAL_BUILD_IDENTITY_DEPFILE}"
  DEPENDS
    "${MAKO_LOCAL_FINGERPRINT_SCRIPT}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/storage/mako_local_abi.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/storage/mako_timestamp.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/storage/mako_local_rust_fast_abi.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/sto/MakoTimestampClock.hh"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/sto/MakoTimestampClock.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/storage/mako_local_abi.cc"
    "${CMAKE_BINARY_DIR}/compile_commands.json"
    "${CMAKE_BINARY_DIR}/CMakeCache.txt"
    "${MAKO_ALLOCATOR_METADATA}"
    "${MASSTREE_GENERATED_CONFIG_H}"
    ${MAKO_ALLOCATOR_LIBRARY}
  DEPFILE "${MAKO_LOCAL_BUILD_IDENTITY_DEPFILE}"
  COMMENT "Generating source-derived mako-local native build identity"
  VERBATIM
)

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
  "${W}/sto/MakoTimestampClock.cc"
  "${W}/sto/MassTrans.cc"
  "${W}/sto/TRcu.cc"
  "${W}/sto/Packer.cc"
  "${W}/sto/ReplayDB.cc"
  "${W}/sto/ThreadPool.cc"
  "${W}/sto/thread_registration.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/lookup3.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/kv_store.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/message.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/memory.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/helper_queue.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/transport.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/fasttransport.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/srpc_rpc_backend.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/sharding_admission.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/multi_transport_manager.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/configuration.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/timestamp.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/promise.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/client.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/shardClient.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/lib/server.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/storage/abstract_db.cc"
  # The mtx_* C ABI over masstree (Rust write-back cache). Deliberately part
  # of the mako target rather than a separate library: a separate target
  # compiled without the same CONFIG_H gets a different CHECK_INVARIANTS,
  # which changes ticker::tick_us (40ms vs 1ms) and the percore/percore_lazy
  # layouts. That is silent ODR corruption, not a compile error.
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/storage/mtx_abi.cc"
  # The mt_* record-directory C ABI over masstree (native Rust STO).
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/storage/mtree_abi.cc"
  # Local STO/MassTrans transaction facade used by the Rust Mako port.
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/storage/mako_local_abi.cc"
  "${MAKO_LOCAL_BUILD_IDENTITY_CPP}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/cluster_bootstrap.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/native_sharding_storage.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/native_sharding_rpc.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/rocks_interface/gateway_receiver.cc"
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
          ${CMAKE_CURRENT_SOURCE_DIR}/bin/rpcgen
          ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc/pylib/simplerpcgen/lang_cpp.py
          ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc/pylib/simplerpcgen/rpcgen.py
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
    src/deptran/raft/frame.cc
    src/deptran/raft/raft_transport.cc
    src/deptran/raft/raft_worker.cc
    src/deptran/raft/server.cc
    src/deptran/replication_log_entry.cc
    src/deptran/scheduler.cc
    src/deptran/tpc_command.cc
    src/deptran/tpcc_sharding.cc
    src/deptran/view_data.cc
)

# Runtime replication switching: compile BOTH Paxos and Raft helpers
# The replication_helper.cc dispatcher routes calls based on runtime configuration
set(TXLOG_HELPER_SRC
    src/deptran/replication_helper.cc
    src/deptran/paxos_main_helper.cc
    src/deptran/raft_main_helper.cc
)

# Native sharding is ordinary Rust, never a rusty-cpp/transpiler input. Keep its
# Cargo output isolated from SRPC's independently pinned crate/toolchain.
find_program(MAKO_SHARDING_CARGO cargo REQUIRED)
set(MAKO_SHARDING_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-sharding")
set(MAKO_SHARDING_ARCHIVE "${MAKO_SHARDING_TARGET_DIR}/release/libmako_sharding.a")
file(GLOB_RECURSE MAKO_SHARDING_RUST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cluster/*.rs"
    "${CMAKE_CURRENT_SOURCE_DIR}/tla/mako/src/*.rs")
# Every Rust staticlib of one executable must come from one toolchain, so
# that they share a single std: without LTO each archive ships the same
# precompiled std objects and the linker takes them from the first archive
# only (see the note in crates/mrx-core/Cargo.toml). The native sharding and
# Raft archives are linked into every Mako executable, so their Cargo
# commands use the same Cargo executable as rust-lib and the crates/
# workspace (MAKO_CARGO_EXECUTABLE) and the same optional rustup selection
# (MAKO_RUST_STO_RUSTUP_TOOLCHAIN, set only by the Rust STO sanitizer lanes;
# see the command audit in scripts/ci/run_rust_sto_sanitizer.sh). They run
# from the source root so that a rustup proxy resolves rust-toolchain.toml
# exactly as it does for those other Cargo edges, wherever the build
# directory is. The ASan and TSan lanes also give them the lanes'
# instrumented sysroot (MAKO_RUST_STO_SYSROOT).
if(NOT _MAKO_RUST_STATICLIB_TSAN_STD AND "${MAKO_RUST_STO_SYSROOT}" STREQUAL "")
add_custom_command(
    OUTPUT "${MAKO_SHARDING_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E env
        ${_MAKO_RUST_LIB_TOOLCHAIN_ENV}
        "${_MAKO_RUST_LIB_CARGO_EXECUTABLE}" build --locked --release --lib
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/src/cluster/Cargo.toml"
        --target-dir "${MAKO_SHARDING_TARGET_DIR}"
    DEPENDS ${MAKO_SHARDING_RUST_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/src/cluster/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/cluster/Cargo.lock"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building native Rust sharding staticlib (locked dependencies)"
    VERBATIM)
elseif(NOT _MAKO_RUST_STATICLIB_TSAN_STD)
# MAKO_RUST_STO_SYSROOT: the same release build (panic = "abort" as declared
# in src/cluster/Cargo.toml) against the Rust STO sanitizer sysroot. --target
# moves Cargo's output under <triple>/, and the copy restores the one path
# that crates/mako-local/native-link-archives.txt and the imported target use.
add_custom_command(
    OUTPUT "${MAKO_SHARDING_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E env
        ${_MAKO_RUST_LIB_TOOLCHAIN_ENV}
        ${_MAKO_RUST_STO_SYSROOT_ENV}
        "${_MAKO_RUST_LIB_CARGO_EXECUTABLE}" build --locked --release --lib
        ${_MAKO_RUST_STO_SYSROOT_TARGET_ARGS}
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/src/cluster/Cargo.toml"
        --target-dir "${MAKO_SHARDING_TARGET_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${MAKO_SHARDING_TARGET_DIR}/${MAKO_RUST_STO_TARGET_TRIPLE}/release/libmako_sharding.a"
        "${MAKO_SHARDING_ARCHIVE}"
    DEPENDS ${MAKO_SHARDING_RUST_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/src/cluster/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/cluster/Cargo.lock"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building native Rust sharding staticlib against the Rust STO sanitizer sysroot"
    VERBATIM)
else()
# MAKO_LOCAL_TSAN_RUST_STATICLIBS: the Cargo command of the mako-local TSan
# test executables (run_mako_local_sanitizer_tests), applied to this crate, in
# its own target directory. The archive is then copied to the one path that
# crates/mako-local/native-link-archives.txt and the imported target both use.
set(_MAKO_SHARDING_TSAN_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-sharding-tsan")
add_custom_command(
    OUTPUT "${MAKO_SHARDING_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E env
        "RUSTFLAGS=${MAKO_LOCAL_SANITIZER_RUSTFLAGS}"
        "CARGO_PROFILE_DEV_PANIC=unwind"
        "${_MAKO_RUST_LIB_CARGO_EXECUTABLE}"
        ${MAKO_LOCAL_SANITIZER_CARGO_PREFIX}
        build --locked --profile dev --lib
        ${MAKO_LOCAL_SANITIZER_CARGO_TARGET_ARGS}
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/src/cluster/Cargo.toml"
        --target-dir "${_MAKO_SHARDING_TSAN_TARGET_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${_MAKO_SHARDING_TSAN_TARGET_DIR}/${_MAKO_RUST_STATICLIB_TSAN_TRIPLE}/debug/libmako_sharding.a"
        "${MAKO_SHARDING_ARCHIVE}"
    DEPENDS ${MAKO_SHARDING_RUST_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/src/cluster/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/cluster/Cargo.lock"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building native Rust sharding staticlib against the mako-local Rust TSan std"
    VERBATIM)
endif()
add_custom_target(mako_sharding_rust_build DEPENDS "${MAKO_SHARDING_ARCHIVE}")
add_library(mako_sharding_native STATIC IMPORTED GLOBAL)
set_target_properties(mako_sharding_native PROPERTIES
    IMPORTED_LOCATION "${MAKO_SHARDING_ARCHIVE}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src"
    INTERFACE_LINK_LIBRARIES "rocksdb;pthread;${CMAKE_DL_LIBS};m;rt;util")
add_dependencies(mako_sharding_native mako_sharding_rust_build)

# Explicit Docker-only proof gate; it verifies the production crate, not a
# separate executable model. The independent model/controls run in addition.
add_custom_target(verify_native_sharding
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_native_sharding.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL VERBATIM)

# ---------------------------------------------------------------------------
# `cluster` retains the configuration KV ports and the disabled/static engine
# table-address bridge. Governed placement/admission/migration live exclusively
# in the native Rust staticlib, not retired C++ policy/cache module partitions.
set(CLUSTER_MODULE_INTERFACES
    ${CMAKE_SOURCE_DIR}/src/cluster/kv_store.h
    ${CMAKE_SOURCE_DIR}/src/cluster/in_memory_kv_store.h
    ${CMAKE_SOURCE_DIR}/src/cluster/remote_kv_store.h
    ${CMAKE_SOURCE_DIR}/src/cluster/config_manager.h
    ${CMAKE_SOURCE_DIR}/src/cluster/shard_router.h
    ${CMAKE_SOURCE_DIR}/src/cluster/cluster.ccm
)
# .h / .ccm module units are not auto-detected as C++ (only .cc/.cpp/.cxx are);
# tell CMake they compile as C++ so the CXX_MODULES fileset schedules them.
set_source_files_properties(${CLUSTER_MODULE_INTERFACES} PROPERTIES LANGUAGE CXX)
# Module implementation units (module cluster;) — regular C++ sources.
set(CLUSTER_MODULE_IMPLS
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
target_link_libraries(cluster PUBLIC srpc pthread btree_port vec_port rc_port std_port mako_sharding_native)

# The Raft server is Rust. src/deptran/raft is a
# cargo staticlib: its C ABI (src/deptran/raft/server_exports.h) is what the C++
# shim `class RaftServer` and the hand-written kernels in server.cc call, and it
# calls those kernels back. cargo builds it into this build tree; the pinned
# rusty-rustc facade (src/rusty-rustc) is its only dependency.
# The Rust side of the Raft RPC service, from the same .rpc file rcc_rpc.h
# comes from. Without this the checked-in rpc.rs is a snapshot that can drift
# from the wire the C++ peers speak, and nothing would say so: the ids are not
# recorded in the .rpc at all (rpcgen assigns them randomly and preserves them
# only by scraping the header it previously wrote), so this reads them out of
# rcc_rpc.h -- which is why it depends on that header and not just the .rpc.
set(RAFT_RPC_RUST "${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/rt/src/rpc.rs")
add_custom_command(
    OUTPUT ${RAFT_RPC_RUST}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/rpcgen_rust.py
            --service Raft
            --rpc src/deptran/rcc_rpc.rpc
            --ids src/deptran/raft/rpc_ids.txt
            --ids-from src/deptran/rcc_rpc.h
            --out src/deptran/raft/rt/src/rpc.rs
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.rpc
            ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/rcc_rpc.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/rpc_ids.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/rpcgen_rust.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "rpcgen_rust: the Rust side of the Raft RPC service"
    VERBATIM)

set(RAFT_RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/raft-cargo")
# The linked artifact is raft-rt (libraft_rt.a), which contains the shell
# crate (src/deptran/raft) and the core as rlibs.
set(RAFT_RUST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/rt/Cargo.toml")
set(RAFT_RUST_LIB "${RAFT_RUST_TARGET_DIR}/release/libraft_rt.a")
# The srpc crate is in here because raft-rt depends on it -- transport.rs
# names srpc::client::Client. Without these, editing a canonical srpc .rs
# leaves libraft_rt.a stale: ninja sees no changed dependency, never re-invokes
# cargo, and cargo therefore never gets the chance to notice. The four module
# directories are the ones src/srpc/rust-modules.toml names, plus the
# generated lib.rs.
file(GLOB RAFT_RUST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/shell/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/core/src/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/rt/src/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/replay/src/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/store/src/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rusty-rustc/src/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rusty-cpp-markers/src/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc/base/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc/misc/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc/reactor/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc/rpc/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc/src/*.rs)
# The Rust half of the lab build. -DRAFT_TEST=ON defines RAFT_TEST_CORO for
# the C++ side; the crate needs the matching cargo feature or the two halves
# disagree about whether they are in lab mode (the leader no-op and the lab's
# initial role both branch on it). One switch, both languages.
# -DMAKO_RAFT_DISK=ON adds the disk-mode feature the same way.
set(RAFT_RUST_FEATURE_LIST)
if(RAFT_TEST)
    list(APPEND RAFT_RUST_FEATURE_LIST raft_test)
endif()
if(MAKO_RAFT_DISK)
    list(APPEND RAFT_RUST_FEATURE_LIST raft_disk)
endif()
set(RAFT_RUST_FEATURES)
if(RAFT_RUST_FEATURE_LIST)
    list(JOIN RAFT_RUST_FEATURE_LIST "," RAFT_RUST_FEATURE_CSV)
    set(RAFT_RUST_FEATURES --features ${RAFT_RUST_FEATURE_CSV})
endif()
# CC and AR are not decoration. src/srpc/build.rs compiles the nine C
# kernels with $CC, falling back to `cc`, while src/srpc-cmake builds the
# same manifest with CMAKE_C_COMPILER -- and srpc_rand.c:17 branches on
# __clang__. Unset, `cc` here is gcc, so the binary carried TWO different
# srpc_rand implementations (clang: a pthread_key_t seed freed at thread
# exit; gcc: a _Thread_local seed with no teardown) and the linker picked
# one by archive order. Passing the compiler through makes the two copies
# the same code; stage 0b removes the second copy.
# --locked here and in the cargo tests below: build what
# src/deptran/raft/Cargo.lock records, and fail on a stale lock rather
# than re-resolve it.
if(NOT _MAKO_RUST_STATICLIB_TSAN_STD AND "${MAKO_RUST_STO_SYSROOT}" STREQUAL "")
    set(_RAFT_RUST_BUILD_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E env
            "CC=${CMAKE_C_COMPILER}"
            "AR=${CMAKE_AR}"
            ${_MAKO_RUST_LIB_TOOLCHAIN_ENV}
            "${_MAKO_RUST_LIB_CARGO_EXECUTABLE}" build --release --locked --quiet
            --manifest-path ${RAFT_RUST_MANIFEST}
            --target-dir ${RAFT_RUST_TARGET_DIR}
            ${RAFT_RUST_FEATURES})
elseif(NOT _MAKO_RUST_STATICLIB_TSAN_STD)
    # MAKO_RUST_STO_SYSROOT: the same release build, panic = "abort" as this
    # workspace declares, against the Rust STO sanitizer sysroot, then copied
    # from <triple>/release to the canonical path. As in the mako-local TSan
    # mode below, the srpc C kernels stay out of this archive.
    set(_RAFT_RUST_BUILD_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E env
            "CC=${CMAKE_C_COMPILER}"
            "AR=${CMAKE_AR}"
            ${_MAKO_RUST_LIB_TOOLCHAIN_ENV}
            ${_MAKO_RUST_STO_SYSROOT_ENV}
            "${_MAKO_RUST_LIB_CARGO_EXECUTABLE}" build --release --locked --quiet
            ${_MAKO_RUST_STO_SYSROOT_TARGET_ARGS}
            --manifest-path ${RAFT_RUST_MANIFEST}
            --target-dir ${RAFT_RUST_TARGET_DIR}
            ${RAFT_RUST_FEATURES}
        COMMAND ${CMAKE_COMMAND} -E copy
            "${RAFT_RUST_TARGET_DIR}/${MAKO_RUST_STO_TARGET_TRIPLE}/release/libraft_rt.a"
            "${RAFT_RUST_LIB}")
else()
    # MAKO_LOCAL_TSAN_RUST_STATICLIBS: the Cargo command of the mako-local
    # TSan test executables, as for native sharding above. This workspace's
    # dev profile says panic = "abort", which would give std a different
    # hash; the override restores the profile `cargo test` builds std with.
    # DECLARED DEVIATION, this lane only: Raft and the srpc code inside
    # libraft_rt.a run with panic = "unwind" here, against the rule in
    # src/deptran/raft/Cargo.toml that a panic must abort before it can cross
    # a fiber switch. srpc's `let _ = catch_unwind(..)` sites (rpc/callbacks.rs,
    # rpc/request_queue.rs) therefore swallow a callback panic that aborts in
    # every other build, and the dev profile adds overflow checks and debug
    # assertions, so this lane can pass where production would abort.
    # The srpc C kernels stay out of this archive (srpc/build.rs links them
    # -bundle), so the executables take them from the instrumented libsrpc.a.
    set(_RAFT_RUST_TSAN_TARGET_DIR "${CMAKE_BINARY_DIR}/raft-cargo-tsan")
    set(_RAFT_RUST_BUILD_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E env
            "CC=${CMAKE_C_COMPILER}"
            "AR=${CMAKE_AR}"
            "RUSTFLAGS=${MAKO_LOCAL_SANITIZER_RUSTFLAGS}"
            "CARGO_PROFILE_DEV_PANIC=unwind"
            "${_MAKO_RUST_LIB_CARGO_EXECUTABLE}"
            ${MAKO_LOCAL_SANITIZER_CARGO_PREFIX}
            build --profile dev --locked --quiet
            ${MAKO_LOCAL_SANITIZER_CARGO_TARGET_ARGS}
            --manifest-path ${RAFT_RUST_MANIFEST}
            --target-dir ${_RAFT_RUST_TSAN_TARGET_DIR}
            ${RAFT_RUST_FEATURES}
        COMMAND ${CMAKE_COMMAND} -E copy
            "${_RAFT_RUST_TSAN_TARGET_DIR}/${_MAKO_RUST_STATICLIB_TSAN_TRIPLE}/debug/libraft_rt.a"
            "${RAFT_RUST_LIB}")
endif()
add_custom_command(
    OUTPUT ${RAFT_RUST_LIB}
    ${_RAFT_RUST_BUILD_COMMANDS}
    DEPENDS ${RAFT_RUST_SOURCES}
            ${RAFT_RPC_RUST}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/rt/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/core/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/replay/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/store/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/src/rusty-rustc/Cargo.toml
    # The source root, so a rustup proxy applies rust-toolchain.toml here as
    # it does for the sharding, rust-lib and crates/ Cargo edges (see the
    # note above the native sharding command). Every path is absolute.
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "cargo build: the Raft core and raft-rt as ${RAFT_RUST_LIB}"
    VERBATIM)
add_custom_target(raft_rust_build DEPENDS ${RAFT_RUST_LIB})
add_library(raft_rust STATIC IMPORTED GLOBAL)
set_target_properties(raft_rust PROPERTIES
    IMPORTED_LOCATION ${RAFT_RUST_LIB}
    # what Rust's std needs from the platform
    INTERFACE_LINK_LIBRARIES "pthread;dl;m;rt;util;gcc_s")

# The Raft core every link site below uses (the rustc artifact), and what must
# be built before anything that links it.
set(RAFT_CORE_TARGET raft_rust)
set(RAFT_CORE_DEPS raft_rust_build)

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
add_dependencies(txlog_core_obj raft_goal0_source_gate ${RAFT_CORE_DEPS})
# Pull in srpc's PUBLIC include dirs + module BMI paths so deptran .cc files
# that `#include "srpc/srpc.hpp"` find both the
# umbrella header and the imports it references.
target_link_libraries(txlog_core_obj PUBLIC srpc cluster ${RAFT_CORE_TARGET})
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
add_dependencies(txlog_core ${RAFT_CORE_DEPS})
target_link_libraries(txlog_core
    PUBLIC
    ${RAFT_CORE_TARGET}
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

# The archive and the Rust library need each other (kernels one way, exports the
# other), so anything that links the archive links both in a rescan group.
set(TXLOG_LINK_LIBS
    "$<LINK_GROUP:RESCAN,txlog_core,${RAFT_CORE_TARGET}>"
)
set(TXLOG_BUILD_DEPS
    txlog_core ${RAFT_CORE_DEPS}
)

# The kernel ABI's exactly-once check (scripts/raft_kernel_check.py), on every
# build: each raft_* symbol the core imports is defined exactly once by the
# host archive, and nothing the core defines (raft-rt's seam) is defined by
# the host as well.
set(RAFT_KERNEL_CHECK_COMMANDS
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/raft_kernel_check.py
            --once "${RAFT_RUST_LIB}" "$<TARGET_FILE:txlog_core>")
set(RAFT_KERNEL_CHECK_DEPS txlog_core ${RAFT_CORE_DEPS})
# raft-rt's own tests (the snapshot store's unit
# tests, the transport round trips, resend suppression, the frame cap) gate
# the build too. A stamp keeps them from rerunning when nothing they cover
# changed; this tree's feature set is passed so the tests see the lab or
# production crate this tree links.
file(GLOB RAFT_RT_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/rt/tests/*.rs)
set(RAFT_RT_TEST_STAMP "${CMAKE_BINARY_DIR}/raft_rt_test.stamp")
add_custom_command(
    OUTPUT ${RAFT_RT_TEST_STAMP}
    COMMAND ${CMAKE_COMMAND} -E env
            "CC=${CMAKE_C_COMPILER}"
            "AR=${CMAKE_AR}"
            ${_MAKO_RUST_LIB_TOOLCHAIN_ENV}
            "${_MAKO_RUST_LIB_CARGO_EXECUTABLE}" test --release --locked --quiet
            --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/rt/Cargo.toml
            --target-dir ${RAFT_RUST_TARGET_DIR}
            ${RAFT_RUST_FEATURES}
    COMMAND ${CMAKE_COMMAND} -E touch ${RAFT_RT_TEST_STAMP}
    DEPENDS ${RAFT_RUST_SOURCES} ${RAFT_RT_TEST_SOURCES} ${RAFT_RUST_LIB}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "cargo test: raft-rt"
    VERBATIM)
add_custom_target(raft_rt_test DEPENDS ${RAFT_RT_TEST_STAMP})
list(APPEND RAFT_KERNEL_CHECK_DEPS raft_rt_test)
# raft-store's tests (the WAL crash sweep, recovery, creation, the flusher,
# the base's ops) gate the build in both modes; a disk build adds the RocksDB
# base's (`--features rocksdb`, which links librocksdb as the binaries do).
set(RAFT_STORE_TEST_FEATURES "")
if(MAKO_RAFT_DISK)
    set(RAFT_STORE_TEST_FEATURES --features rocksdb)
endif()
file(GLOB RAFT_STORE_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/store/src/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/store/tests/*.rs)
set(RAFT_STORE_TEST_STAMP "${CMAKE_BINARY_DIR}/raft_store_test.stamp")
add_custom_command(
    OUTPUT ${RAFT_STORE_TEST_STAMP}
    COMMAND ${CMAKE_COMMAND} -E env
            ${_MAKO_RUST_LIB_TOOLCHAIN_ENV}
            "${_MAKO_RUST_LIB_CARGO_EXECUTABLE}" test --release --locked --quiet ${RAFT_STORE_TEST_FEATURES}
            --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/store/Cargo.toml
            --target-dir ${RAFT_RUST_TARGET_DIR}
    COMMAND ${CMAKE_COMMAND} -E touch ${RAFT_STORE_TEST_STAMP}
    DEPENDS ${RAFT_STORE_TEST_SOURCES}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/store/Cargo.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/store/build.rs
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "cargo test: raft-store"
    VERBATIM)
add_custom_target(raft_store_test DEPENDS ${RAFT_STORE_TEST_STAMP})
list(APPEND RAFT_KERNEL_CHECK_DEPS raft_store_test)
# raft-core's and raft-replay's tests: they check every persist note on
# recorded runs (core/tests/persist_note.rs, the replay's shadow), a runtime
# check beside Verus's proof that each note is exact.
file(GLOB RAFT_CORE_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/core/src/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/core/tests/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/core/tests/common/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/replay/src/*.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/replay/tests/*.rs)
set(RAFT_CORE_TEST_STAMP "${CMAKE_BINARY_DIR}/raft_core_test.stamp")
add_custom_command(
    OUTPUT ${RAFT_CORE_TEST_STAMP}
    COMMAND ${CMAKE_COMMAND} -E env
            ${_MAKO_RUST_LIB_TOOLCHAIN_ENV}
            "${_MAKO_RUST_LIB_CARGO_EXECUTABLE}" test --release --locked --quiet -p raft-core -p raft-replay
            --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran/raft/Cargo.toml
            --target-dir ${RAFT_RUST_TARGET_DIR}
    COMMAND ${CMAKE_COMMAND} -E touch ${RAFT_CORE_TEST_STAMP}
    DEPENDS ${RAFT_CORE_TEST_SOURCES}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "cargo test: raft-core, raft-replay"
    VERBATIM)
add_custom_target(raft_core_test DEPENDS ${RAFT_CORE_TEST_STAMP})
list(APPEND RAFT_KERNEL_CHECK_DEPS raft_core_test)
add_custom_target(raft_kernel_check ALL
    ${RAFT_KERNEL_CHECK_COMMANDS}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/raft_kernel_check.py
    COMMENT "Checking the Raft kernel ABI (exactly-once)"
    VERBATIM)
add_dependencies(raft_kernel_check ${RAFT_KERNEL_CHECK_DEPS})

# Temporary Raft-only target for borrow checking
file(GLOB RAFT_BORROW_SRC
    "src/deptran/raft/*.cc"
    "src/deptran/raft/*.cpp"
)
# Exclude test files from raft borrow checking. The lab suite is Rust
# (src/deptran/raft/shell/lab*.rs, raft-rt's lab_runtime.rs); this filter is
# kept for any C++ test file added here.
# NOTE: raft_main_helper.cc is also excluded because it includes third-party headers
# (YAML, etc.) that generate 1000+ false positive violations from header code.
list(FILTER RAFT_BORROW_SRC EXCLUDE REGEX ".*test(conf)?\\.cc$")

if(RAFT_BORROW_SRC)
    add_library(raft_borrow OBJECT ${RAFT_BORROW_SRC})
    set_target_properties(raft_borrow PROPERTIES EXCLUDE_FROM_ALL YES)
    add_dependencies(raft_borrow raft_goal0_source_gate)

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
        add_dependencies(borrow_check_raft raft_goal0_source_gate)

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
# A static library gets no objects from txlog_core_obj, only usage requirements;
# executables that link mako alone (the gtest binaries) therefore need the
# kernels' archive next to libraft_rt.a, in the same rescan group.
target_link_libraries(mako PUBLIC txlog_core_obj masstree mako_allocator mako_sharding_native
    "$<LINK_GROUP:RESCAN,txlog_core,${RAFT_CORE_TARGET}>")
add_dependencies(mako rcc_rpc_gen)
target_include_directories(mako PUBLIC ${LIBEVENT_INCLUDE_DIRS})
set_target_properties(mako PROPERTIES OUTPUT_NAME "mako")

# The Rust STO adapter uses the Masstree C ABI and runtime, not the C++
# transaction engine or cluster modules. Compile that exact native dependency
# closure separately for focused mixed-language sanitizer tests. These sources
# retain the same generated Masstree configuration and compiler flags as mako.
add_library(mako_mtree_boundary STATIC EXCLUDE_FROM_ALL
    src/mako/storage/mtree_abi.cc
    src/mako/allocator.cc
    src/mako/core.cc
    src/mako/counter.cc
    src/mako/rcu.cc
    src/mako/ticker.cc
    src/mako/silo_runtime.cc)
target_include_directories(mako_mtree_boundary BEFORE PUBLIC
    "${MASSTREE_CONFIG_INCLUDE_DIR}")
target_include_directories(mako_mtree_boundary PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/${MASSTREE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mako"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp/include")
target_compile_options(mako_mtree_boundary PUBLIC ${BENCH_CXXFLAGS})
target_compile_definitions(mako_mtree_boundary PUBLIC CONFIG_H=\"${CONFIG_H}\")
target_link_libraries(mako_mtree_boundary PUBLIC masstree)

# Enforce the public mako-local symbol boundary in every native build, not
# only in profiles that happen to have the vendored GoogleTest checkout.
# libmako contains many unrelated C++ template symbols, so the checker limits
# itself to the exact mako_local_* namespace and binds its one internal anchor
# to the digest recorded by the generated native-build manifest.
add_custom_target(check_mako_local_abi_symbols ALL
    COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/scripts/check_mako_local_symbols.py"
            --nm "${CMAKE_NM}"
            --archive "$<TARGET_FILE:mako>"
            --allowlist
              "${CMAKE_SOURCE_DIR}/tests/mako_local_abi_symbols.txt"
            --manifest "${MAKO_LOCAL_BUILD_MANIFEST}"
    DEPENDS
        mako
        "${MAKO_LOCAL_FINGERPRINT_SCRIPT}"
        "${CMAKE_SOURCE_DIR}/scripts/check_mako_local_symbols.py"
        "${CMAKE_SOURCE_DIR}/tests/mako_local_abi_symbols.txt"
        "${MAKO_LOCAL_BUILD_MANIFEST}"
    COMMENT "Checking exact mako-local exported-symbol surface"
    VERBATIM
)

# Content verification is deliberately always-run when this explicit boundary
# gate is requested: source mtimes are not trusted. Keep it out of the default
# ALL target because a complete compiler-dependency scan is intentionally much
# heavier than the exported-symbol check.
set(MAKO_LOCAL_RUST_LINK_ARCHIVE_MANIFEST
    "${CMAKE_SOURCE_DIR}/crates/mako-local/native-link-archives.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MAKO_LOCAL_RUST_LINK_ARCHIVE_MANIFEST}")
file(STRINGS "${MAKO_LOCAL_RUST_LINK_ARCHIVE_MANIFEST}"
    MAKO_LOCAL_RUST_LINK_ARCHIVE_LINES)
# Dependent Cargo packages (mako-cache, mako-cache-bench, mrx) repeat the
# manifest's trailing link group, and can be built from an isolated copy of
# their own crate directory (the mutation runners), where crates/mako-local
# is not a sibling. Publish the manifest beside the archives it describes;
# the configure dependency above refreshes the copy when the manifest changes.
configure_file("${MAKO_LOCAL_RUST_LINK_ARCHIVE_MANIFEST}"
    "${CMAKE_BINARY_DIR}/generated/mako_local_native_link_archives.txt"
    COPYONLY)
# A `<name> <path>` row names a CMake target that archives compiled objects.
# A `<name> <path> rust-staticlib` row names a Cargo-built Rust staticlib that
# libmako.a calls into: its CMake identity is an IMPORTED target whose name
# differs from the library name, so the rows are bound here to the imported
# target, to the custom target that runs Cargo, and to the archive path this
# file configured. A row whose path drifts from that location fails the
# configure instead of handing Cargo a stale or missing archive.
set(MAKO_LOCAL_RUST_STATICLIB_NAMES mako_sharding raft_rt)
set(MAKO_LOCAL_RUST_STATICLIB_TARGET_mako_sharding mako_sharding_native)
set(MAKO_LOCAL_RUST_STATICLIB_BUILD_mako_sharding mako_sharding_rust_build)
set(MAKO_LOCAL_RUST_STATICLIB_ARCHIVE_mako_sharding "${MAKO_SHARDING_ARCHIVE}")
set(MAKO_LOCAL_RUST_STATICLIB_TARGET_raft_rt raft_rust)
set(MAKO_LOCAL_RUST_STATICLIB_BUILD_raft_rt raft_rust_build)
set(MAKO_LOCAL_RUST_STATICLIB_ARCHIVE_raft_rt "${RAFT_RUST_LIB}")
set(MAKO_LOCAL_RUST_LINK_TARGETS "")
set(MAKO_LOCAL_RUST_LINK_NAMES "")
set(MAKO_LOCAL_RUST_STATICLIB_ROWS "")
foreach(MAKO_LOCAL_RUST_LINK_ARCHIVE_LINE
        IN LISTS MAKO_LOCAL_RUST_LINK_ARCHIVE_LINES)
    if(MAKO_LOCAL_RUST_LINK_ARCHIVE_LINE MATCHES "^[ \t]*(#|$)")
        continue()
    endif()
    string(REGEX MATCH
        "^([A-Za-z0-9_]+)[ \t]+([^ \t]+)([ \t]+rust-staticlib)?[ \t]*$"
        MAKO_LOCAL_RUST_LINK_ARCHIVE_MATCH
        "${MAKO_LOCAL_RUST_LINK_ARCHIVE_LINE}")
    if(NOT MAKO_LOCAL_RUST_LINK_ARCHIVE_MATCH)
        message(FATAL_ERROR
            "Invalid mako-local native archive manifest line: "
            "${MAKO_LOCAL_RUST_LINK_ARCHIVE_LINE}")
    endif()
    set(MAKO_LOCAL_RUST_LINK_NAME "${CMAKE_MATCH_1}")
    set(MAKO_LOCAL_RUST_LINK_RELATIVE "${CMAKE_MATCH_2}")
    set(MAKO_LOCAL_RUST_LINK_KIND "${CMAKE_MATCH_3}")
    list(FIND MAKO_LOCAL_RUST_LINK_NAMES
        "${MAKO_LOCAL_RUST_LINK_NAME}"
        MAKO_LOCAL_RUST_LINK_NAME_INDEX)
    if(NOT MAKO_LOCAL_RUST_LINK_NAME_INDEX EQUAL -1)
        message(FATAL_ERROR
            "Duplicate mako-local native archive target: "
            "${MAKO_LOCAL_RUST_LINK_NAME}")
    endif()
    list(APPEND MAKO_LOCAL_RUST_LINK_NAMES "${MAKO_LOCAL_RUST_LINK_NAME}")
    if(MAKO_LOCAL_RUST_LINK_KIND STREQUAL "")
        if(NOT TARGET "${MAKO_LOCAL_RUST_LINK_NAME}")
            message(FATAL_ERROR
                "Mako-local native archive target does not exist: "
                "${MAKO_LOCAL_RUST_LINK_NAME}")
        endif()
        list(APPEND MAKO_LOCAL_RUST_LINK_TARGETS
            "${MAKO_LOCAL_RUST_LINK_NAME}")
        continue()
    endif()
    if(NOT MAKO_LOCAL_RUST_LINK_NAME IN_LIST MAKO_LOCAL_RUST_STATICLIB_NAMES)
        message(FATAL_ERROR
            "Mako-local native archive manifest names an unknown Rust "
            "staticlib: ${MAKO_LOCAL_RUST_LINK_NAME}")
    endif()
    if(NOT TARGET
            "${MAKO_LOCAL_RUST_STATICLIB_TARGET_${MAKO_LOCAL_RUST_LINK_NAME}}"
            OR NOT TARGET
            "${MAKO_LOCAL_RUST_STATICLIB_BUILD_${MAKO_LOCAL_RUST_LINK_NAME}}")
        message(FATAL_ERROR
            "Mako-local Rust staticlib targets do not exist for: "
            "${MAKO_LOCAL_RUST_LINK_NAME}")
    endif()
    if(NOT "${CMAKE_BINARY_DIR}/${MAKO_LOCAL_RUST_LINK_RELATIVE}" STREQUAL
            "${MAKO_LOCAL_RUST_STATICLIB_ARCHIVE_${MAKO_LOCAL_RUST_LINK_NAME}}")
        message(FATAL_ERROR
            "Mako-local native archive manifest places Rust staticlib "
            "${MAKO_LOCAL_RUST_LINK_NAME} at "
            "${MAKO_LOCAL_RUST_LINK_RELATIVE}, but this build produces "
            "${MAKO_LOCAL_RUST_STATICLIB_ARCHIVE_${MAKO_LOCAL_RUST_LINK_NAME}}")
    endif()
    list(APPEND MAKO_LOCAL_RUST_STATICLIB_ROWS "${MAKO_LOCAL_RUST_LINK_NAME}")
    # The custom target, not the IMPORTED library: a custom target's DEPENDS
    # takes build targets, and this one is what runs Cargo.
    list(APPEND MAKO_LOCAL_RUST_LINK_TARGETS
        "${MAKO_LOCAL_RUST_STATICLIB_BUILD_${MAKO_LOCAL_RUST_LINK_NAME}}")
endforeach()
if(NOT MAKO_LOCAL_RUST_LINK_TARGETS)
    message(FATAL_ERROR "Mako-local native archive manifest is empty")
endif()
# libmako.a cannot be linked without both staticlibs (see
# target_link_libraries(mako ...) above), so a manifest that omits one would
# only fail later, in every Cargo link, with undefined symbols.
foreach(MAKO_LOCAL_RUST_STATICLIB_NAME IN LISTS MAKO_LOCAL_RUST_STATICLIB_NAMES)
    if(NOT MAKO_LOCAL_RUST_STATICLIB_NAME IN_LIST MAKO_LOCAL_RUST_STATICLIB_ROWS)
        message(FATAL_ERROR
            "Mako-local native archive manifest lacks the rust-staticlib row "
            "for ${MAKO_LOCAL_RUST_STATICLIB_NAME}")
    endif()
endforeach()
add_custom_target(check_mako_local_build_fingerprint
    COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}"
            "${MAKO_LOCAL_FINGERPRINT_SCRIPT}"
            verify
            --source-root "${CMAKE_SOURCE_DIR}"
            --build-dir "${CMAKE_BINARY_DIR}"
            --manifest "${MAKO_LOCAL_BUILD_MANIFEST}"
    DEPENDS
        ${MAKO_LOCAL_RUST_LINK_TARGETS}
        "${MAKO_LOCAL_FINGERPRINT_SCRIPT}"
        "${MAKO_LOCAL_RUST_LINK_ARCHIVE_MANIFEST}"
        "${MAKO_LOCAL_BUILD_MANIFEST}"
    COMMENT "Verifying complete mako-local native content identity"
    VERBATIM
)
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
# test against those archive paths. It is not part of ALL.
#
# The list names libmako.a without its Rust staticlibs (native sharding, the
# Raft runtime) on purpose. The mt_* ABI selects only mtree_abi.cc and the
# runtime objects that mako_mtree_boundary also archives (allocator, core,
# counter, rcu, ticker, silo_runtime), none of which reaches sharding or
# Raft. And a Rust staticlib must never appear in a rustc link-lib list: it
# would precede the test executable's own std and replace its unwinding
# panic runtime with the archive's panic=abort one. Links that do reach
# those archives (mako_local_*, mtx_*) add them through the trailing group
# described in crates/mako-local/native-link-archives.txt.
set(MAKO_MTREE_RUST_NATIVE_LIBS
    "static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread"
    CACHE STRING
    "Comma-separated rustc native libraries for the Masstree ABI integration test"
)
option(MAKO_RUST_STO_WORKSPACE_NATIVE_LINK
    "Link sanitizer-instrumented Rust workspace checks to native Masstree" OFF)
set(MAKO_RUST_STO_SANITIZER_RUSTDOCFLAGS "" CACHE STRING
    "Rustdoc flags confined to sanitizer-instrumented Rust STO test targets")
set(MAKO_RUST_STO_CARGO_LINKER_ENV "" CACHE STRING
    "Cargo target-linker environment name for Rust STO sanitizer targets")
set(MAKO_RUST_STO_ASAN_NATIVE_QUARANTINE_TESTS "" CACHE STRING
    "Exact native FFI tests run with ASan leak reporting disabled")
set(MAKO_RUST_STO_ASAN_OPTIONS "" CACHE STRING
    "ASan runtime options confined to Rust STO sanitizer targets")
# Retired: -Zbuild-std rebuilt std inside every Cargo invocation with a crate
# hash taken from that invocation's profile, and two such stds collide in any
# executable that also links libmako.a's Rust staticlibs. An instrumented std
# now comes from one prebuilt sysroot (MAKO_RUST_STO_SYSROOT above).
if(MAKO_RUST_STO_BUILD_STD)
    message(FATAL_ERROR
        "MAKO_RUST_STO_BUILD_STD is retired: per-invocation -Zbuild-std stds "
        "collide with libmako.a's Rust staticlibs. Build one instrumented std "
        "into a sysroot and pass MAKO_RUST_STO_SYSROOT, as "
        "scripts/ci/run_rust_sto_sanitizer.sh does")
endif()
if(MAKO_RUST_STO_WORKSPACE_NATIVE_LINK
        AND (MAKO_RUST_STO_TARGET_TRIPLE STREQUAL ""
             OR MAKO_RUST_STO_SYSROOT STREQUAL ""))
    message(FATAL_ERROR
        "MAKO_RUST_STO_WORKSPACE_NATIVE_LINK is sanitizer-only and requires "
        "MAKO_RUST_STO_TARGET_TRIPLE plus MAKO_RUST_STO_SYSROOT")
endif()
if(NOT MAKO_RUST_STO_SYSROOT STREQUAL "")
    separate_arguments(_MAKO_RUST_STO_SYSROOT_DOC_FLAGS UNIX_COMMAND
        "${MAKO_RUST_STO_SANITIZER_RUSTDOCFLAGS}")
    list(FILTER _MAKO_RUST_STO_SYSROOT_DOC_FLAGS INCLUDE REGEX "^--sysroot")
    if(NOT _MAKO_RUST_STO_SYSROOT_DOC_FLAGS STREQUAL
            "--sysroot=${MAKO_RUST_STO_SYSROOT}")
        message(FATAL_ERROR
            "MAKO_RUST_STO_SYSROOT requires exactly one "
            "--sysroot=${MAKO_RUST_STO_SYSROOT} in "
            "MAKO_RUST_STO_SANITIZER_RUSTDOCFLAGS")
    endif()
endif()
if(MAKO_RUST_STO_WORKSPACE_NATIVE_LINK
        AND (MAKO_RUST_STO_SANITIZER_RUSTFLAGS STREQUAL ""
             OR MAKO_RUST_STO_SANITIZER_RUSTDOCFLAGS STREQUAL ""))
    message(FATAL_ERROR
        "MAKO_RUST_STO_WORKSPACE_NATIVE_LINK requires explicit sanitizer "
        "Rust and rustdoc flags")
endif()
if((MAKO_RUST_STO_SANITIZER_RUSTFLAGS STREQUAL ""
        AND NOT MAKO_RUST_STO_SANITIZER_RUSTDOCFLAGS STREQUAL "")
        OR (NOT MAKO_RUST_STO_SANITIZER_RUSTFLAGS STREQUAL ""
            AND MAKO_RUST_STO_SANITIZER_RUSTDOCFLAGS STREQUAL ""))
    message(FATAL_ERROR
        "Rust STO sanitizer Rust and rustdoc flags must be set together")
endif()
set(_MAKO_RUST_STO_SCOPED_TOOLCHAIN_LINK_ENV "")
set(_MAKO_RUST_STO_SCOPED_SANITIZER_ENV "")
if(NOT MAKO_RUST_STO_SANITIZER_RUSTFLAGS STREQUAL "")
    if(NOT MAKO_RUST_STO_CARGO_LINKER_ENV MATCHES
            "^CARGO_TARGET_[A-Z0-9_]+_LINKER$")
        message(FATAL_ERROR
            "Rust STO sanitizer flags require an exact Cargo target-linker "
            "environment name")
    endif()
    if(NOT CMAKE_C_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
        message(FATAL_ERROR
            "Rust STO sanitizer targets require CMake's Clang C compiler "
            "as their linker")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
        message(FATAL_ERROR
            "Rust STO sanitizer targets require CMake's Clang C++ compiler")
    endif()
    if(NOT CMAKE_C_COMPILER_ID STREQUAL CMAKE_CXX_COMPILER_ID)
        message(FATAL_ERROR
            "Rust STO sanitizer targets require C and C++ compilers from "
            "the same Clang family; found ${CMAKE_C_COMPILER_ID} and "
            "${CMAKE_CXX_COMPILER_ID}")
    endif()
    string(REGEX MATCH "^[0-9]+" _MAKO_RUST_STO_C_COMPILER_MAJOR
        "${CMAKE_C_COMPILER_VERSION}")
    string(REGEX MATCH "^[0-9]+" _MAKO_RUST_STO_CXX_COMPILER_MAJOR
        "${CMAKE_CXX_COMPILER_VERSION}")
    if(_MAKO_RUST_STO_C_COMPILER_MAJOR STREQUAL ""
            OR _MAKO_RUST_STO_CXX_COMPILER_MAJOR STREQUAL ""
            OR NOT _MAKO_RUST_STO_C_COMPILER_MAJOR STREQUAL
                _MAKO_RUST_STO_CXX_COMPILER_MAJOR)
        message(FATAL_ERROR
            "Rust STO sanitizer targets require matching Clang C/C++ major "
            "versions; found ${CMAKE_C_COMPILER_VERSION} and "
            "${CMAKE_CXX_COMPILER_VERSION}")
    endif()
    list(APPEND _MAKO_RUST_STO_SCOPED_TOOLCHAIN_LINK_ENV
        "${MAKO_RUST_STO_CARGO_LINKER_ENV}=${CMAKE_C_COMPILER}")
    if(NOT MAKO_RUST_STO_RUSTUP_TOOLCHAIN STREQUAL "")
        list(APPEND _MAKO_RUST_STO_SCOPED_TOOLCHAIN_LINK_ENV
            "RUSTUP_TOOLCHAIN=${MAKO_RUST_STO_RUSTUP_TOOLCHAIN}")
    endif()
    set(_MAKO_RUST_STO_SCOPED_SANITIZER_ENV
        ${_MAKO_RUST_STO_SCOPED_TOOLCHAIN_LINK_ENV}
        "RUSTFLAGS=${MAKO_RUST_STO_SANITIZER_RUSTFLAGS}"
        "RUSTDOCFLAGS=${MAKO_RUST_STO_SANITIZER_RUSTDOCFLAGS}")
endif()
set(_MAKO_RUST_STO_CARGO_PLATFORM_ARGS "")
if(NOT MAKO_RUST_STO_TARGET_TRIPLE STREQUAL "")
    list(APPEND _MAKO_RUST_STO_CARGO_PLATFORM_ARGS
        --target "${MAKO_RUST_STO_TARGET_TRIPLE}")
endif()
if(MAKO_CARGO_EXECUTABLE)
    get_filename_component(_MAKO_CXX_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_MAKO_CXX_COMPILER_ROOT "${_MAKO_CXX_COMPILER_DIR}" DIRECTORY)

    if(APPLE)
        set(_MAKO_MTREE_LOADER_PATH_VARIABLE "DYLD_LIBRARY_PATH")
    else()
        set(_MAKO_MTREE_LOADER_PATH_VARIABLE "LD_LIBRARY_PATH")
    endif()

    set(_MAKO_MASSTREE_NATIVE_TEST
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/masstree/tests/native_integration.rs")
    set(_MAKO_STO_MASSTREE_NATIVE_TEST
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-masstree/tests/native_integration.rs")
    set(_MAKO_STO_MASSTREE_HISTORY_TEST
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-masstree/tests/history_oracle.rs")
    set(_MAKO_STO_TPCC_NATIVE_TEST
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-tpcc-ffi/tests/native_ffi.rs")
    foreach(_MAKO_REQUIRED_NATIVE_TEST IN ITEMS
            "${_MAKO_MASSTREE_NATIVE_TEST}"
            "${_MAKO_STO_MASSTREE_NATIVE_TEST}"
            "${_MAKO_STO_MASSTREE_HISTORY_TEST}"
            "${_MAKO_STO_TPCC_NATIVE_TEST}")
        if(NOT EXISTS "${_MAKO_REQUIRED_NATIVE_TEST}")
            message(FATAL_ERROR
                "Required Rust STO native integration suite is missing: "
                "${_MAKO_REQUIRED_NATIVE_TEST}")
        endif()
    endforeach()
    unset(_MAKO_REQUIRED_NATIVE_TEST)

    set(_MAKO_RUST_STO_NATIVE_SANITIZER_ARGS "")
    if(NOT MAKO_RUST_STO_SANITIZER_RUSTFLAGS STREQUAL "")
        list(APPEND _MAKO_RUST_STO_NATIVE_SANITIZER_ARGS
            "-DMAKO_RUSTFLAGS=${MAKO_RUST_STO_SANITIZER_RUSTFLAGS}"
            "-DMAKO_RUSTDOCFLAGS=${MAKO_RUST_STO_SANITIZER_RUSTDOCFLAGS}"
            "-DMAKO_RUSTUP_TOOLCHAIN=${MAKO_RUST_STO_RUSTUP_TOOLCHAIN}"
            "-DMAKO_RUST_LINKER_ENV_NAME=${MAKO_RUST_STO_CARGO_LINKER_ENV}"
            "-DMAKO_RUST_LINKER=${CMAKE_C_COMPILER}")
    endif()
    if(NOT MAKO_RUST_STO_ASAN_NATIVE_QUARANTINE_TESTS STREQUAL "")
        list(APPEND _MAKO_RUST_STO_NATIVE_SANITIZER_ARGS
            "-DMAKO_ASAN_NATIVE_QUARANTINE_TESTS=${MAKO_RUST_STO_ASAN_NATIVE_QUARANTINE_TESTS}")
    endif()
    if(NOT MAKO_RUST_STO_ASAN_OPTIONS STREQUAL "")
        list(APPEND _MAKO_RUST_STO_NATIVE_SANITIZER_ARGS
            "-DMAKO_ASAN_OPTIONS=${MAKO_RUST_STO_ASAN_OPTIONS}")
    endif()
    set(_MAKO_RUST_STO_NATIVE_PLATFORM_ARGS "")
    if(NOT MAKO_RUST_STO_TARGET_TRIPLE STREQUAL "")
        list(APPEND _MAKO_RUST_STO_NATIVE_PLATFORM_ARGS
            "-DMAKO_RUST_TARGET_TRIPLE=${MAKO_RUST_STO_TARGET_TRIPLE}")
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
            ${_MAKO_RUST_STO_NATIVE_PLATFORM_ARGS}
            ${_MAKO_RUST_STO_NATIVE_SANITIZER_ARGS}
            "-DMAKO_MASSTREE_NATIVE_TEST=${_MAKO_MASSTREE_NATIVE_TEST}"
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


# ---------------------------------------------------------------
# The Rust cache, and the differential harness that compares it to
# the C++ one.
# ---------------------------------------------------------------
#
# cargo builds crates/mrx-ffi into a staticlib; CMake drives the
# final link, exactly as it does for every other archive here. The
# crate is NOT built by cargo's own test runner in this path -- that
# runs separately via `cargo test` in crates/ -- so this target only
# produces the archive.
#
# ALWAYS out of date on purpose: cargo is the incremental build
# system for its own sources, and teaching CMake to track them would
# be a second, divergent dependency graph. cargo no-ops in a few
# milliseconds when nothing changed.
find_program(CARGO_EXECUTABLE cargo)
if (CARGO_EXECUTABLE)
    set(MRX_FFI_PROFILE "release")
    # Cargo writes the archive under CARGO_TARGET_DIR when the environment
    # sets one (docker_build.sh exports /workspace/target-docker) and under
    # the workspace's own crates/target otherwise. Resolve the directory once,
    # at configure time, and hand the same value to the Cargo command below,
    # so the path CMake links and the path Cargo writes cannot disagree. A
    # relative value is resolved against crates/, Cargo's working directory
    # here.
    if(DEFINED ENV{CARGO_TARGET_DIR}
            AND NOT "$ENV{CARGO_TARGET_DIR}" STREQUAL "")
        get_filename_component(MRX_FFI_TARGET_DIR "$ENV{CARGO_TARGET_DIR}"
            ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}/crates")
        message(STATUS
            "Using CARGO_TARGET_DIR for crates/mrx-ffi: ${MRX_FFI_TARGET_DIR}")
    else()
        set(MRX_FFI_TARGET_DIR "${CMAKE_SOURCE_DIR}/crates/target")
    endif()
    set(MRX_FFI_LIB
        "${MRX_FFI_TARGET_DIR}/${MRX_FFI_PROFILE}/libmrxffi.a")
    # add_custom_target, NOT add_custom_command(OUTPUT ...): a
    # command with an OUTPUT is skipped once the file exists and its
    # declared DEPENDS are unchanged, and CMake cannot see the Rust
    # sources. That is not a slow build, it is a WRONG one -- a
    # deliberate divergence probe injected into the Rust cache was
    # silently not rebuilt, and the differential test passed against
    # the stale archive. A custom target always runs, and cargo
    # no-ops in milliseconds when nothing changed.
    add_custom_target(mrx_ffi_build
        BYPRODUCTS ${MRX_FFI_LIB}
        COMMAND ${CMAKE_COMMAND} -E env
                "MAKO_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "CARGO_TARGET_DIR=${MRX_FFI_TARGET_DIR}"
                ${CARGO_EXECUTABLE} build --release -p mrx-ffi
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/crates
        COMMENT "Building the Rust cache (crates/mrx-ffi -> libmrxffi.a)"
        VERBATIM
    )
    # libmako.a must exist first: mrx-masstree links the mtx_* symbols
    # out of it, and its build script looks for the archive to decide
    # whether masstree is available at all.
    add_dependencies(mrx_ffi_build mako)

    add_library(mrxffi STATIC IMPORTED GLOBAL)
    set_target_properties(mrxffi PROPERTIES
        IMPORTED_LOCATION ${MRX_FFI_LIB}
        INTERFACE_INCLUDE_DIRECTORIES
            ${CMAKE_SOURCE_DIR}/crates/mrx-ffi/include)
    add_dependencies(mrxffi mrx_ffi_build)
else()
    message(STATUS
        "cargo not found: the Rust cache, its differential tests, and the "
        "Rust arm of masstree_rocks_bench are skipped")
endif()

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
  )

  # Add dependencies
  if(ENABLE_BORROW_CHECKING)
    add_dependencies(${exec_name} mako rust_lib ${APP_TXLOG_BUILD_DEPS} build_rusty_cpp_checker)
  else()
    add_dependencies(${exec_name} mako rust_lib ${APP_TXLOG_BUILD_DEPS})
  endif()
endfunction()

add_apps(dbtest src/mako/benchmarks/dbtest.cc)
add_apps(mako_admin src/mako/mako_admin.cc)
add_apps(native_sharding_smoke examples/native_sharding_smoke.cc)
add_apps(simpleTransaction examples/simpleTransaction.cc
    TXLOG_LIBS "$<LINK_GROUP:RESCAN,txlog_core,${RAFT_CORE_TARGET}>"
    TXLOG_DEPS txlog_core ${RAFT_CORE_DEPS}
)
add_apps(simpleTransactionRep examples/simpleTransactionRep.cc)
add_apps(continuousTransactions examples/continuousTransactions.cc)
add_apps(test_rocksdb_persistence examples/test_rocksdb_persistence.cc)
add_apps(rocksdb_replay_app src/mako/benchmarks/rocksdb_replay_app.cc)
# Does the Masstree write-back cache beat plain RocksDB? Reports write
# ack vs ack+flush separately, since the cache's trick is deferral.
add_apps(masstree_rocks_bench src/mako/benchmarks/masstree_rocks_bench.cc)
# The Rust arm runs in the SAME process as the C++ one, so the comparison
# is between implementations rather than between two machine states.
if (TARGET mrxffi)
  # mrxffi is a static archive and selects native objects that reference
  # yaml-cpp and libevent. Keep both after that consumer so one-pass ELF
  # linkers using --as-needed can resolve those references.
  target_link_libraries(masstree_rocks_bench
      mrxffi yaml-cpp ${LIBEVENT_LDFLAGS})
  target_include_directories(masstree_rocks_bench PRIVATE
      ${CMAKE_SOURCE_DIR}/crates/mrx-ffi/include)
  add_dependencies(masstree_rocks_bench mrx_ffi_build)

  # Focused syscall/context-switch probe for the same C++ and Rust cache
  # implementations. Keeping this in CMake makes it inherit the configured
  # compiler, generated module map, STO definitions, and current dependency
  # closure instead of reconstructing those details in a host-specific script.
  add_apps(mrx_kernel_probe src/mako/benchmarks/mrx_kernel_probe.cc)
  target_link_libraries(mrx_kernel_probe
      mrxffi yaml-cpp ${LIBEVENT_LDFLAGS})
  target_include_directories(mrx_kernel_probe PRIVATE
      ${CMAKE_SOURCE_DIR}/crates/mrx-ffi/include)
  add_dependencies(mrx_kernel_probe mrx_ffi_build)
endif()
# Peels the same write path one layer at a time (masstree -> entry CAS ->
# byte accounting -> version draw -> announce floor -> ticket log ->
# flusher), so the C++ and Rust ladders can be subtracted from each other
# instead of compared as whole stacks.
add_apps(mrx_write_ablation src/mako/benchmarks/mrx_write_ablation.cc)
# Single-threaded, under callgrind: instructions and simulated D1/LL
# misses for ONE write on each arm. valgrind serialises threads, so this
# prices WORK PER OP, never contention -- which is exactly the datum the
# throughput ladders cannot supply.
add_apps(mrx_callgrind src/mako/benchmarks/mrx_callgrind.cc)
if (TARGET mrxffi)
  target_link_libraries(mrx_callgrind
      mrxffi yaml-cpp ${LIBEVENT_LDFLAGS})
  target_include_directories(mrx_callgrind PRIVATE
      ${CMAKE_SOURCE_DIR}/crates/mrx-ffi/include)
  add_dependencies(mrx_callgrind mrx_ffi_build)
endif()
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
    # add_apps force-includes Masstree's config for its one source only.
    mako_force_include_masstree_config(src/deptran/server_worker.cc)
  endif()
  add_apps(simpleRaft examples/mako-raft-tests/simpleRaft.cc)
  add_apps(simpleTransactionRepRaft examples/mako-raft-tests/simpleTransactionRepRaft.cc)
  add_apps(testPreferredReplicaStartup examples/mako-raft-tests/testPreferredReplicaStartup.cc)
  add_apps(testPreferredReplicaLogReplication examples/mako-raft-tests/testPreferredReplicaLogReplication.cc)
  add_apps(testNoOps examples/mako-raft-tests/testNoOps.cc)
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
    set(MAKO_RUST_STO_ASAN_QUARANTINE_TESTS "" CACHE STRING
        "Exact intentional-quarantine tests skipped by the ASan workspace sweep")
    set(_MAKO_RUST_STO_CONFIGURED_SANITIZER "")
    if(STO_TPCC_RUST_EXTRA_RUSTFLAGS MATCHES
            "(^|[ \t])-Zsanitizer=([^ \t]+)($|[ \t])")
        set(_MAKO_RUST_STO_CONFIGURED_SANITIZER "${CMAKE_MATCH_2}")
    endif()
    if(NOT _MAKO_RUST_STO_CONFIGURED_SANITIZER STREQUAL ""
            AND NOT MAKO_RUST_STO_WORKSPACE_NATIVE_LINK)
        message(FATAL_ERROR
            "Rust compiler sanitizer instrumentation requires "
            "MAKO_RUST_STO_WORKSPACE_NATIVE_LINK=ON so workspace tests "
            "cannot omit the native Masstree link closure")
    endif()
    if(MAKO_RUST_STO_WORKSPACE_NATIVE_LINK)
        if(NOT _MAKO_RUST_STO_CONFIGURED_SANITIZER MATCHES
                "^(address|thread)$")
            message(FATAL_ERROR
                "MAKO_RUST_STO_WORKSPACE_NATIVE_LINK requires address or "
                "thread in STO_TPCC_RUST_EXTRA_RUSTFLAGS")
        endif()
        if(NOT MAKO_RUST_STO_SANITIZER_RUSTFLAGS MATCHES
                "(^|[ \t])-Zsanitizer=${_MAKO_RUST_STO_CONFIGURED_SANITIZER}($|[ \t])"
                OR NOT MAKO_RUST_STO_SANITIZER_RUSTFLAGS MATCHES
                "(^|[ \t])-Zexternal-clangrt($|[ \t])")
            message(FATAL_ERROR
                "MAKO_RUST_STO_WORKSPACE_NATIVE_LINK requires matching "
                "-Zsanitizer and -Zexternal-clangrt in RUSTFLAGS")
        endif()
    endif()
    if(NOT MAKO_RUST_STO_SYSROOT STREQUAL "")
        # This archive shares its executables with libmako.a's staticlibs,
        # so it must compile against the same sysroot (see
        # MAKO_RUST_STO_SYSROOT).
        separate_arguments(_STO_TPCC_SYSROOT_FLAGS UNIX_COMMAND
            "${STO_TPCC_RUST_EXTRA_RUSTFLAGS}")
        list(FILTER _STO_TPCC_SYSROOT_FLAGS INCLUDE REGEX "^--sysroot")
        if(NOT _STO_TPCC_SYSROOT_FLAGS STREQUAL
                "--sysroot=${MAKO_RUST_STO_SYSROOT}")
            message(FATAL_ERROR
                "MAKO_RUST_STO_SYSROOT requires exactly one "
                "--sysroot=${MAKO_RUST_STO_SYSROOT} in "
                "STO_TPCC_RUST_EXTRA_RUSTFLAGS")
        endif()
    endif()
    set(_MAKO_RUST_STO_WORKSPACE_TEST_ARGS "")
    if(_MAKO_RUST_STO_CONFIGURED_SANITIZER STREQUAL "address")
        if(MAKO_RUST_STO_ASAN_QUARANTINE_TESTS STREQUAL "")
            message(FATAL_ERROR
                "ASan requires an explicit "
                "MAKO_RUST_STO_ASAN_QUARANTINE_TESTS allowlist")
        endif()
        if(MAKO_RUST_STO_ASAN_NATIVE_QUARANTINE_TESTS STREQUAL "")
            message(FATAL_ERROR
                "ASan requires an explicit "
                "MAKO_RUST_STO_ASAN_NATIVE_QUARANTINE_TESTS allowlist")
        endif()
        if(NOT MAKO_RUST_STO_ASAN_OPTIONS MATCHES
                "(^|:)detect_leaks=1(:|$)")
            message(FATAL_ERROR
                "ASan requires MAKO_RUST_STO_ASAN_OPTIONS with "
                "detect_leaks=1")
        endif()
        list(APPEND _MAKO_RUST_STO_WORKSPACE_TEST_ARGS --)
        foreach(_MAKO_RUST_STO_ASAN_QUARANTINE_TEST IN LISTS
                MAKO_RUST_STO_ASAN_QUARANTINE_TESTS)
            if(NOT _MAKO_RUST_STO_ASAN_QUARANTINE_TEST MATCHES
                    "^[A-Za-z0-9_.:-]+$")
                message(FATAL_ERROR
                    "Invalid ASan quarantine test name: "
                    "${_MAKO_RUST_STO_ASAN_QUARANTINE_TEST}")
            endif()
            list(APPEND _MAKO_RUST_STO_WORKSPACE_TEST_ARGS
                --skip "${_MAKO_RUST_STO_ASAN_QUARANTINE_TEST}")
        endforeach()
        foreach(_MAKO_RUST_STO_ASAN_NATIVE_QUARANTINE_TEST IN LISTS
                MAKO_RUST_STO_ASAN_NATIVE_QUARANTINE_TESTS)
            if(NOT _MAKO_RUST_STO_ASAN_NATIVE_QUARANTINE_TEST MATCHES
                    "^[A-Za-z0-9_.:-]+$")
                message(FATAL_ERROR
                    "Invalid native ASan quarantine test name: "
                    "${_MAKO_RUST_STO_ASAN_NATIVE_QUARANTINE_TEST}")
            endif()
        endforeach()
    elseif(NOT MAKO_RUST_STO_ASAN_QUARANTINE_TESTS STREQUAL ""
            OR NOT MAKO_RUST_STO_ASAN_NATIVE_QUARANTINE_TESTS STREQUAL ""
            OR NOT MAKO_RUST_STO_ASAN_OPTIONS STREQUAL "")
        message(FATAL_ERROR
            "Rust STO ASan quarantine allowlists are valid only for ASan")
    endif()
    set(_STO_TPCC_RUSTFLAGS
        "-C target-cpu=native -C force-frame-pointers=yes")
    if(NOT STO_TPCC_RUST_EXTRA_RUSTFLAGS STREQUAL "")
        string(APPEND _STO_TPCC_RUSTFLAGS
            " ${STO_TPCC_RUST_EXTRA_RUSTFLAGS}")
    endif()
    if(MAKO_RUST_STO_TARGET_TRIPLE STREQUAL "")
        set(_STO_TPCC_RUST_PROFILE_DIR
            "${STO_TPCC_RUST_TARGET_DIR}/release")
    else()
        set(_STO_TPCC_RUST_PROFILE_DIR
            "${STO_TPCC_RUST_TARGET_DIR}/${MAKO_RUST_STO_TARGET_TRIPLE}/release")
    endif()
    set(STO_TPCC_RUST_ARCHIVE
        "${_STO_TPCC_RUST_PROFILE_DIR}/libsto_tpcc_ffi.a")
    set(STO_TPCC_RUST_MANIFESTS
        "${CMAKE_CURRENT_SOURCE_DIR}/rust-toolchain.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/mako-clock/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/mako-engine-api/Cargo.toml"
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
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/mako-clock/src/*.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/mako-engine-api/src/*.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-masstree/src/*.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/masstree/src/*.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/crates/mtree-sys/src/*.rs")
    # LTO is off for this archive, and that is a link requirement, not a
    # tuning choice. Every binary that links it also links `mako`, which now
    # carries the native sharding and Raft staticlibs. Rust staticlibs can
    # share one executable only while each ships the same precompiled std
    # objects, so that the linker takes them from the first archive alone.
    # LTO (fat or thin) rewrites std into this crate's own object, and the
    # next archive's std member then collides with it. Measured with this
    # toolchain (rustc 1.97.1, GNU ld 2.42): an lto=fat staticlib next to a
    # plain one fails with "multiple definition of `rust_eh_personality'" and
    # of std::panicking::EMPTY_PANIC; lto=false links. crates/mrx-core/Cargo.toml
    # records the same failure for lto=thin. For this crate as declared
    # (crate-type = ["staticlib", "rlib"]) Cargo runs no LTO at all, so the
    # former lto=fat only embedded bitcode in the same objects; a real fat-LTO
    # build (cargo rustc --crate-type staticlib) fails on the sto_tpcc_bench
    # link line with multiple definitions of rust_eh_personality,
    # std::panicking::EMPTY_PANIC and std::sys::args::unix::imp::ARGV_INIT_ARRAY.
    # The same holds for two different stds, which is why the ASan and TSan
    # lanes build every Rust archive against one sysroot
    # (MAKO_RUST_STO_SYSROOT) instead of -Zbuild-std.
    #
    # The archive must also stay FIRST among the Rust staticlibs on the link
    # lines below: it is built with panic=unwind and catches panics at its C
    # boundary, while the sharding and Raft archives are panic=abort. The
    # first archive that supplies std also supplies the panic runtime for the
    # whole executable, so listing it after `mako` would turn those catches
    # into aborts.
    add_custom_command(
        OUTPUT "${STO_TPCC_RUST_ARCHIVE}"
        COMMAND "${CMAKE_COMMAND}" -E env
            --unset=CARGO_BUILD_TARGET
            --unset=CARGO_ENCODED_RUSTFLAGS
            ${_MAKO_RUST_STO_SCOPED_TOOLCHAIN_LINK_ENV}
            "CARGO_TARGET_DIR=${STO_TPCC_RUST_TARGET_DIR}"
            "CARGO_INCREMENTAL=0"
            "CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1"
            "CARGO_PROFILE_RELEASE_DEBUG=1"
            "CARGO_PROFILE_RELEASE_LTO=false"
            "CARGO_PROFILE_RELEASE_OPT_LEVEL=3"
            "CARGO_PROFILE_RELEASE_PANIC=unwind"
            "RUSTFLAGS=${_STO_TPCC_RUSTFLAGS}"
            "${MAKO_CARGO_EXECUTABLE}" build --release --locked
            ${_MAKO_RUST_STO_CARGO_PLATFORM_ARGS}
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
        $<$<AND:$<NOT:$<BOOL:${MAKO_ASAN_ENABLED}>>,$<NOT:$<BOOL:${MAKO_TSAN_ENABLED}>>>:${JEMALLOC_LDFLAGS}>)
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
        MAKO_RUST_STO_TPCC=1
        MAKO_RUST_STO_TEST_HOOKS=1)
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
        $<$<AND:$<NOT:$<BOOL:${MAKO_ASAN_ENABLED}>>,$<NOT:$<BOOL:${MAKO_TSAN_ENABLED}>>>:${JEMALLOC_LDFLAGS}>)
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
        $<$<AND:$<NOT:$<BOOL:${MAKO_ASAN_ENABLED}>>,$<NOT:$<BOOL:${MAKO_TSAN_ENABLED}>>>:${JEMALLOC_LDFLAGS}>)
    add_dependencies(sto_tpcc_cpp_wrapper_scan_smoke
        mako sto_tpcc_rust_build ${TXLOG_BUILD_DEPS})

    # Rust-engine sharding storage fixture: one scenario per process, driven
    # by examples/rust_sharding_storage.py. It is not a CTest.
    add_executable(rust_sharding_storage_fixture EXCLUDE_FROM_ALL
        examples/rust_sharding_storage_fixture.cc
        src/mako/storage/rust_sto_tpcc_wrapper.cc)
    target_include_directories(rust_sharding_storage_fixture PRIVATE
        ${MASSTREE_CONFIG_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mako
        ${CMAKE_CURRENT_SOURCE_DIR}/${MASSTREE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/deptran
        ${CMAKE_CURRENT_SOURCE_DIR}/src/srpc
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/crates/sto-tpcc-ffi/include)
    target_compile_options(rust_sharding_storage_fixture PRIVATE
        ${BENCH_CXXFLAGS})
    mako_force_include_masstree_config(
        examples/rust_sharding_storage_fixture.cc
        src/mako/storage/rust_sto_tpcc_wrapper.cc)
    target_compile_definitions(rust_sharding_storage_fixture PRIVATE
        CONFIG_H="${CONFIG_H}"
        MAKO_RUST_STO_TPCC=1)
    target_link_libraries(rust_sharding_storage_fixture PRIVATE
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
        $<$<AND:$<NOT:$<BOOL:${MAKO_ASAN_ENABLED}>>,$<NOT:$<BOOL:${MAKO_TSAN_ENABLED}>>>:${JEMALLOC_LDFLAGS}>)
    add_dependencies(rust_sharding_storage_fixture
        mako sto_tpcc_rust_build ${TXLOG_BUILD_DEPS})
else()
    message(STATUS "Cargo not found; sto_tpcc_bench target is unavailable")
endif()
# raft_bench — standalone Raft performance driver,
# beside the scripts that drive it (scripts/raft_perf/).
# Registered like the add_apps programs above: it is an ordinary
# app that links `mako` and drives the replication helper. It is deliberately
# NOT inside the MAKO_USE_RAFT block, because that option only gates the extra
# Raft example executables; the Raft helper itself is always compiled into
# `mako` via TXLOG_HELPER_SRC, so the driver measures the production build.
add_apps(raft_bench scripts/raft_perf/raft_bench.cc)
# raft_kill_node -- one Raft replica for the process-kill tests, beside the
# driver and checker that use it (scripts/raft_kill/).
add_apps(raft_kill_node scripts/raft_kill/raft_kill_node.cc)
# Provenance the record cannot otherwise obtain: neither MODE nor
# CMAKE_BUILD_TYPE is a preprocessor macro, and a number is not interpretable
# without knowing which build produced it.
target_compile_definitions(raft_bench PRIVATE
    RAFT_BENCH_BUILD_FLAVOUR="${MODE}"
    RAFT_BENCH_CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)

# ============================================================================
# Test Configuration
# ============================================================================

# Enable testing
enable_testing()

add_test(NAME test_rocksdb_interface_lifecycle
    COMMAND rocksdbInterfaceTest)
set_tests_properties(test_rocksdb_interface_lifecycle PROPERTIES
    LABELS "rocks;sto;masstree;lifecycle"
    RUN_SERIAL TRUE
    TIMEOUT 120)

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

if(TARGET sto_tpcc_bench)
    add_test(NAME test_sto_tpcc_cpp_slow_exit
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/run_sto_tpcc_ctest.py"
            --expected-engine cpp
            --expected-threads 1
            --expected-warehouses 1
            --expected-seconds 1
            --
            "$<TARGET_FILE:sto_tpcc_bench>"
            --num-threads 1
            --shard-config "${CMAKE_CURRENT_SOURCE_DIR}/config/mako_sto_tpcc_local.yml"
            --site-name local_s0
            --runtime 1
            --storage-engine cpp
            --slow-exit)
    set_tests_properties(test_sto_tpcc_cpp_slow_exit PROPERTIES
        LABELS "sto;masstree;ffi;lifecycle;tpcc;cpp"
        RUN_SERIAL TRUE
        TIMEOUT 180)

    add_test(NAME test_sto_tpcc_cpp_multishard_slow_exit
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/run_sto_tpcc_ctest.py"
            --expected-engine cpp
            --expected-threads 1
            --expected-warehouses 1
            --expected-seconds 1
            --expected-result-count 2
            --
            "$<TARGET_FILE:sto_tpcc_bench>"
            --num-threads 1
            --shard-config "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/local-shards2-warehouses1.yml"
            --paxos-proc-name localhost
            --local-shards 0,1
            --runtime 1
            --storage-engine cpp
            --slow-exit)
    set_tests_properties(test_sto_tpcc_cpp_multishard_slow_exit PROPERTIES
        LABELS "sto;masstree;ffi;lifecycle;tpcc;cpp;multishard"
        # Keep the timed phase on transaction classes that never follow remote
        # table pointers. The inherited C++ MassTrans cross-partition payload
        # race is documented separately in masstree-sanitizer-findings.md.
        ENVIRONMENT "MAKO_TPCC_WORKLOAD_MIX=0,0,34,33,33"
        RUN_SERIAL TRUE
        TIMEOUT 240)

    add_test(NAME test_sto_tpcc_rust_slow_exit
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/run_sto_tpcc_ctest.py"
            --expected-engine rust
            --expected-threads 1
            --expected-warehouses 1
            --expected-seconds 1
            --
            "$<TARGET_FILE:sto_tpcc_bench>"
            --num-threads 1
            --shard-config "${CMAKE_CURRENT_SOURCE_DIR}/config/mako_sto_tpcc_local.yml"
            --site-name local_s0
            --runtime 1
            --storage-engine rust
            --slow-exit)
    set_tests_properties(test_sto_tpcc_rust_slow_exit PROPERTIES
        LABELS "sto;masstree;rust;ffi;lifecycle;tpcc"
        RUN_SERIAL TRUE
        TIMEOUT 180)

    add_test(NAME test_sto_tpcc_rust_concurrent
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/run_sto_tpcc_ctest.py"
            --expected-engine rust
            --expected-threads 4
            --expected-warehouses 4
            --expected-seconds 1
            --
            "$<TARGET_FILE:sto_tpcc_bench>"
            --num-threads 4
            --shard-config "${CMAKE_CURRENT_SOURCE_DIR}/config/mako_sto_tpcc_local.yml"
            --site-name local_s0
            --runtime 1
            --storage-engine rust
            --slow-exit)
    set_tests_properties(test_sto_tpcc_rust_concurrent PROPERTIES
        LABELS "sto;masstree;rust;ffi;lifecycle;tpcc;concurrency"
        RUN_SERIAL TRUE
        TIMEOUT 180)

    # Exercise real process shutdown at each capacity-failure boundary. The
    # runner requires exit 3 and rejects any successful TPCC_BENCH_RESULT.
    foreach(_capacity_phase IN ITEMS startup load run)
        add_test(NAME test_sto_tpcc_rust_${_capacity_phase}_resource_exhausted
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/run_sto_tpcc_ctest.py"
                --expected-failure-text "TPCC_RESOURCE_EXHAUSTED phase=${_capacity_phase}"
                --expected-exit-code 3
                --
                "$<TARGET_FILE:sto_tpcc_bench>"
                --num-threads 1
                --shard-config "${CMAKE_CURRENT_SOURCE_DIR}/config/mako_sto_tpcc_local.yml"
                --site-name local_s0
                --runtime 30
                --storage-engine rust
                --slow-exit)
        set_tests_properties(test_sto_tpcc_rust_${_capacity_phase}_resource_exhausted
            PROPERTIES
            LABELS "sto;masstree;rust;ffi;lifecycle;tpcc;capacity"
            RUN_SERIAL TRUE
            TIMEOUT 240)
    endforeach()
    set_tests_properties(test_sto_tpcc_rust_startup_resource_exhausted PROPERTIES
        ENVIRONMENT "MAKO_STO_TPCC_REGISTRY_MEMORY=1")
    set_tests_properties(test_sto_tpcc_rust_load_resource_exhausted PROPERTIES
        ENVIRONMENT "MAKO_STO_TPCC_MAX_RETAINED_RECORDS=1")
    set_tests_properties(test_sto_tpcc_rust_run_resource_exhausted PROPERTIES
        ENVIRONMENT "MAKO_STO_TPCC_MAX_RETAINED_RECORDS=350000;MAKO_TPCC_WORKLOAD_MIX=100,0,0,0,0")

    add_test(NAME test_sto_tpcc_rust_concurrent_resource_exhausted
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/run_sto_tpcc_ctest.py"
            --expected-failure-text "TPCC_RESOURCE_EXHAUSTED phase=run"
            --expected-exit-code 3
            --
            "$<TARGET_FILE:sto_tpcc_bench>"
            --num-threads 4
            --shard-config "${CMAKE_CURRENT_SOURCE_DIR}/config/mako_sto_tpcc_local.yml"
            --site-name local_s0
            --runtime 30
            --storage-engine rust
            --slow-exit)
    set_tests_properties(test_sto_tpcc_rust_concurrent_resource_exhausted PROPERTIES
        LABELS "sto;masstree;rust;ffi;lifecycle;tpcc;capacity;concurrency"
        ENVIRONMENT "MAKO_STO_TPCC_MAX_RETAINED_RECORDS=350000;MAKO_TPCC_WORKLOAD_MIX=100,0,0,0,0"
        RUN_SERIAL TRUE
        TIMEOUT 240)

    add_test(NAME test_sto_tpcc_rejects_duplicate_local_shards
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/run_sto_tpcc_ctest.py"
            --expected-failure-text "duplicate shard index 0"
            --expected-exit-code 2
            --
            "$<TARGET_FILE:sto_tpcc_bench>"
            --num-threads 1
            --shard-config "${CMAKE_CURRENT_SOURCE_DIR}/config/mako_sto_tpcc_local.yml"
            --local-shards 0,0
            --storage-engine rust
            --slow-exit)
    set_tests_properties(test_sto_tpcc_rejects_duplicate_local_shards PROPERTIES
        LABELS "sto;masstree;rust;ffi;lifecycle;tpcc;configuration"
        RUN_SERIAL TRUE
        TIMEOUT 30)

    add_test(NAME test_sto_tpcc_rejects_partial_local_shards
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/run_sto_tpcc_ctest.py"
            --expected-failure-text "hybrid local/remote multi-shard topology is unsupported"
            --expected-exit-code 2
            --
            "$<TARGET_FILE:sto_tpcc_bench>"
            --num-threads 1
            --shard-config "${CMAKE_CURRENT_SOURCE_DIR}/src/mako/config/local-shards3-warehouses1.yml"
            --local-shards 0,1
            --storage-engine rust
            --slow-exit)
    set_tests_properties(test_sto_tpcc_rejects_partial_local_shards PROPERTIES
        LABELS "sto;masstree;rust;ffi;lifecycle;tpcc;configuration"
        RUN_SERIAL TRUE
        TIMEOUT 30)

    # Replicated scenarios run Raft only. The Paxos ones were removed when
    # Paxos was deprecated (src/deptran/paxos/DEPRECATED.md).
    foreach(_protocol_engine IN ITEMS cpp rust)
        foreach(_protocol_replication IN ITEMS none raft)
            set(_protocol_test
                "test_sto_tpcc_protocol_${_protocol_engine}_${_protocol_replication}")
            add_test(NAME "${_protocol_test}"
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/run_sto_protocol_ctest.py"
                    --binary "$<TARGET_FILE:sto_tpcc_bench>"
                    --engine "${_protocol_engine}"
                    --shards 2
                    --replication "${_protocol_replication}"
                    --output "${CMAKE_CURRENT_BINARY_DIR}/${_protocol_test}")
            set_tests_properties("${_protocol_test}" PROPERTIES
                LABELS "sto;masstree;ffi;tpcc;distributed;${_protocol_engine};${_protocol_replication}"
                RUN_SERIAL TRUE
                TIMEOUT 660)
            if(_protocol_engine STREQUAL "rust" OR
                    NOT _protocol_replication STREQUAL "none")
                set(_colocated_test "${_protocol_test}_colocated")
                add_test(NAME "${_colocated_test}"
                    COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/run_sto_protocol_ctest.py"
                        --binary "$<TARGET_FILE:sto_tpcc_bench>"
                        --engine "${_protocol_engine}"
                        --shards 2
                        --replication "${_protocol_replication}"
                        --colocated-leader
                        --output "${CMAKE_CURRENT_BINARY_DIR}/${_colocated_test}")
                set_tests_properties("${_colocated_test}" PROPERTIES
                    LABELS "sto;masstree;ffi;tpcc;distributed;colocated;${_protocol_engine};${_protocol_replication}"
                    RUN_SERIAL TRUE
                    TIMEOUT 660)
            endif()
        endforeach()
    endforeach()

    add_test(NAME test_sto_tpcc_rejects_rust_worker_limit
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/run_sto_tpcc_ctest.py"
            --expected-failure-text "worker count must be in [1, 111]"
            --expected-exit-code 2
            --
            "$<TARGET_FILE:sto_tpcc_bench>"
            --num-threads 112
            --shard-config "${CMAKE_CURRENT_SOURCE_DIR}/config/mako_sto_tpcc_local.yml"
            --site-name local_s0
            --storage-engine rust
            --slow-exit)
    set_tests_properties(test_sto_tpcc_rejects_rust_worker_limit PROPERTIES
        LABELS "sto;masstree;rust;ffi;lifecycle;tpcc;configuration"
        RUN_SERIAL TRUE
        TIMEOUT 30)
endif()

# Capacity diagnostics must remain intact when shutdown loggers write to the
# same pipe without sharing the benchmark stream lock. Keep this regression
# independent of libmako and native process-lifetime allocations.
add_executable(test_benchmark_output EXCLUDE_FROM_ALL
    tests/test_benchmark_output.cc)
target_include_directories(test_benchmark_output PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mako")
target_link_libraries(test_benchmark_output PRIVATE pthread)
set_target_properties(test_benchmark_output PROPERTIES CXX_MODULE_STD OFF)
target_compile_options(test_benchmark_output PRIVATE
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-Wall;-Wextra;-Wpedantic;-Werror>)
add_test(NAME test_benchmark_output COMMAND test_benchmark_output)
set_tests_properties(test_benchmark_output PROPERTIES
    LABELS "sto;rust;ffi;diagnostics;concurrency"
    RUN_SERIAL TRUE
    TIMEOUT 30)

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
# native archives. The final command covers the rest of the Rust workspace.
# Ordinary builds leave its native environment empty. Sanitizer builds opt in
# to the CMake archive closure because sanitizer module constructors retain
# otherwise dead native references; this also intentionally enables every
# native-gated target in the all-target Clippy and test sweeps. The
# sto-tpcc-ffi unit-test binary still runs case-by-case in the native runner
# because native registries have process-wide lifetimes.
if(MAKO_CARGO_EXECUTABLE)
    # The workspace also holds the Rust cache crates (mako-local, mako-cache,
    # mako-cache-bench, mrx-*), whose build scripts verify this build tree's
    # native archives and refuse to guess another build directory. Point them
    # at exactly the tree that produced $<TARGET_FILE:mako>.
    set(_MAKO_RUST_STO_WORKSPACE_NATIVE_ENV
        "MAKO_BUILD_DIR=${CMAKE_BINARY_DIR}"
        ${_MAKO_RUST_STO_SCOPED_SANITIZER_ENV})
    if(MAKO_RUST_STO_WORKSPACE_NATIVE_LINK)
        set(_MAKO_RUST_STO_WORKSPACE_NATIVE_DIRS
            "$<TARGET_FILE_DIR:mako>:$<TARGET_FILE_DIR:masstree>:${_MAKO_CXX_COMPILER_ROOT}/lib")
        set(_MAKO_RUST_STO_WORKSPACE_LOADER_PATH
            "${_MAKO_CXX_COMPILER_ROOT}/lib")
        if(DEFINED ENV{${_MAKO_MTREE_LOADER_PATH_VARIABLE}}
                AND NOT "$ENV{${_MAKO_MTREE_LOADER_PATH_VARIABLE}}" STREQUAL "")
            string(APPEND _MAKO_RUST_STO_WORKSPACE_LOADER_PATH
                ":$ENV{${_MAKO_MTREE_LOADER_PATH_VARIABLE}}")
        endif()
        list(APPEND _MAKO_RUST_STO_WORKSPACE_NATIVE_ENV
            "MAKO_MTREE_NATIVE_INTEGRATION=1"
            "MAKO_MTREE_NATIVE_LIB_DIRS=${_MAKO_RUST_STO_WORKSPACE_NATIVE_DIRS}"
            "MAKO_MTREE_NATIVE_LIBS=${MAKO_MTREE_RUST_NATIVE_LIBS}"
            "${_MAKO_MTREE_LOADER_PATH_VARIABLE}=${_MAKO_RUST_STO_WORKSPACE_LOADER_PATH}")
        if(_MAKO_RUST_STO_CONFIGURED_SANITIZER STREQUAL "address")
            list(APPEND _MAKO_RUST_STO_WORKSPACE_NATIVE_ENV
                "ASAN_OPTIONS=${MAKO_RUST_STO_ASAN_OPTIONS}")
        elseif(_MAKO_RUST_STO_CONFIGURED_SANITIZER STREQUAL "thread")
            list(APPEND _MAKO_RUST_STO_WORKSPACE_NATIVE_ENV
                "TSAN_OPTIONS=$ENV{TSAN_OPTIONS}")
        endif()
    endif()
    set(_MAKO_RUST_STO_NATIVE_ABI_EXCLUSIONS "")
    if(NOT MAKO_RUST_STO_SANITIZER_RUSTFLAGS STREQUAL "")
        # The sanitizer driver runs this native C++ facade in its own lifetime
        # lane: point-only checks with STO_RMW=OFF, then the full RMW/scan and
        # differential suite with STO_RMW=ON. Neither policy belongs to Rust STO.
        list(APPEND _MAKO_RUST_STO_NATIVE_ABI_EXCLUSIONS --exclude mako-local)
    endif()
    add_custom_target(rust_sto_integration
        COMMAND "${CMAKE_COMMAND}" -E env
            --unset=CARGO_BUILD_TARGET
            --unset=CARGO_ENCODED_RUSTFLAGS
            "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/rust-workspace-target"
            ${_MAKO_RUST_STO_WORKSPACE_NATIVE_ENV}
            "${MAKO_CARGO_EXECUTABLE}" clippy
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/crates/Cargo.toml"
            # The `-D warnings` lint gate covers the native Rust STO crates
            # and the shared history oracle. The Rust cache crates
            # (mako-local, mako-cache, mrx-*) share this workspace but keep
            # their own lint policy; the workspace-wide `cargo test` below
            # still exercises them.
            -p mako-history -p masstree -p mtree-sys -p sto-core
            -p sto-masstree -p sto-test-datatypes -p sto-tpcc-ffi
            --all-targets
            --all-features
            --locked
            ${_MAKO_RUST_STO_CARGO_PLATFORM_ARGS}
            --
            -D warnings
        COMMAND "${CMAKE_COMMAND}" -E env
            --unset=CARGO_BUILD_TARGET
            --unset=CARGO_ENCODED_RUSTFLAGS
            "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/rust-workspace-target"
            ${_MAKO_RUST_STO_WORKSPACE_NATIVE_ENV}
            "${MAKO_CARGO_EXECUTABLE}" test
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/crates/Cargo.toml"
            --workspace
            --exclude sto-tpcc-ffi
            # Backend features are mutually exclusive. Exercise the adapter
            # and cache separately with an explicit engine selection.
            --exclude mako-engine --exclude mako-engine-sto
            --exclude mako-cache --exclude mako-cache-bench
            # The direct RocksDB/Raft profile tests its own crates
            # (cmake/RocksdbRaft.cmake): raft-host's scenarios start real
            # SRPC runtimes and must run one at a time.
            --exclude sto-rocksdb --exclude raft-host
            --exclude rocksdb-raft-service
            ${_MAKO_RUST_STO_NATIVE_ABI_EXCLUSIONS}
            --all-targets
            --all-features
            --locked
            ${_MAKO_RUST_STO_CARGO_PLATFORM_ARGS}
            ${_MAKO_RUST_STO_WORKSPACE_TEST_ARGS}
        DEPENDS
            check_mtree_abi_exports
            rust_masstree_native_integration
            sto_tpcc_bench
            sto_tpcc_cpp_wrapper_smoke
            sto_tpcc_cpp_wrapper_scan_smoke
            rocksdbInterfaceTest
            test_benchmark_output
            test_mtree_abi_c11_header
            test_sto_tpcc_ffi_c11_header
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        USES_TERMINAL
        VERBATIM
        COMMENT "Checking the Rust STO workspace and native integration"
    )
    # Keep the selected engine and its cache in the same instrumented link
    # closure. Passing all workspace features would select both backends.
    add_custom_target(rust_sto_cache_integration
        COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
            -p test_run_rust_sto_cache_tests.py
        COMMAND "${CMAKE_COMMAND}" -E env
            --unset=CARGO_BUILD_TARGET
            --unset=CARGO_ENCODED_RUSTFLAGS
            "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/rust-cache-target"
            ${_MAKO_RUST_STO_WORKSPACE_NATIVE_ENV}
            "MAKO_MTREE_NATIVE_INTEGRATION=1"
            "MAKO_MTREE_NATIVE_LIB_DIRS=$<TARGET_FILE_DIR:mako_mtree_boundary>:$<TARGET_FILE_DIR:masstree>:${_MAKO_CXX_COMPILER_ROOT}/lib"
            "MAKO_MTREE_NATIVE_LIBS=static=mako_mtree_boundary,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread"
            "${_MAKO_MTREE_LOADER_PATH_VARIABLE}=${_MAKO_CXX_COMPILER_ROOT}/lib"
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ci/run_rust_sto_cache_tests.py"
            --cargo "${MAKO_CARGO_EXECUTABLE}"
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/crates/Cargo.toml"
            --report "${CMAKE_CURRENT_BINARY_DIR}/rust-sto-cache-tests.json"
            ${_MAKO_RUST_STO_CARGO_PLATFORM_ARGS}
        DEPENDS mako_mtree_boundary masstree
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        USES_TERMINAL VERBATIM
        COMMENT "Checking the selected Rust STO adapter and transaction cache"
    )
    add_dependencies(rust_sto_integration rust_sto_cache_integration)
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
    if(NOT CARGO_EXECUTABLE)
        message(FATAL_ERROR
            "cargo is required when BUILD_TESTS is enabled: the mandatory "
            "mako-local native Rust link and identity gates must not be skipped")
    endif()

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

    # test_srpc_epoll_platform (tests/srpc_epoll_platform_test.cc) is no longer
    # built: it exercised srpc::epoll_open/epoll_add_impl/epoll_update_impl from
    # the C++ epoll implementation, which the upstream srpc subtree deleted in
    # favour of the token-based C kernel (src/srpc/reactor/srpc_epoll.c).



    # Marshal Microbenchmark (no gtest — standalone main; prints ns/op).
    # Measures the Vec<uint8_t>+read_pos Marshal — same harness used
    # during the chunk-list → Vec rewrite (see

    # MakoClientService Unit Tests
    add_executable(test_client_service tests/test_client_service.cc)
    target_include_directories(test_client_service PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_client_service PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_client_service ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} mako_sharding_native pthread)
    add_test(NAME test_client_service COMMAND test_client_service)

    # HelperQueue carries opaque request tokens between the SRPC event loop
    # and one helper thread. Keep its snapshots, FIFO operations, and wakeup
    # paths under a focused concurrency test that can also run under TSan.
    add_executable(test_helper_queue tests/test_helper_queue.cc)
    target_include_directories(test_helper_queue PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_helper_queue PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_helper_queue
        mako ${TEST_LINK_LIBS}
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES}
        yaml-cpp rocksdb lz4 numa event_pthreads
        pthread)
    add_test(NAME test_helper_queue
        COMMAND test_helper_queue --gtest_fail_if_no_test_selected)
    set_tests_properties(test_helper_queue PROPERTIES
        LABELS "concurrency;srpc;lifecycle"
        TIMEOUT 30)

    # Disabled/static physical table routing and table-registry tests.
    add_executable(test_shard_router tests/shard_router_test.cc)
    target_include_directories(test_shard_router PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS} src/deptran src/mako)
    target_compile_options(test_shard_router PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_shard_router ${TXLOG_LINK_LIBS} ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
    add_test(NAME test_shard_router COMMAND test_shard_router)

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

    # Reactor timeout events remain observable as TIMEOUT to the resumed
    # fiber, but must leave the polling queues once terminal. Raft uses this
    # path for its short submission-progress waits under saturation.
    add_executable(test_srpc_timeout_race
        src/srpc/tests/test_timeout_race.cc)
    target_include_directories(test_srpc_timeout_race PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_srpc_timeout_race PRIVATE
        ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_srpc_timeout_race
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread srpc)
    add_test(NAME test_srpc_timeout_race COMMAND test_srpc_timeout_race)

    # srpc's plain-C rand/timing kernel smoke, which also checks that
    # srpc_rand_destroy() leaves pthread key 0 (in production, jemalloc's) and
    # its own seed key alone. Every build
    # already runs it from srpc_goal0_dual_compile; registering it here lets
    # srpcTests report it by name. (src/srpc-cmake is added before
    # enable_testing(), so an add_test there would never be generated.)
    add_test(NAME srpc_goal0_rand_kernel_smoke
        COMMAND srpc_goal0_rand_kernel_smoke)






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


    # Add a convenience target to run all tests. The full src/srpc/tests corpus
    # belongs to srpc; Mako carries only the focused timeout-queue regression
    # above because Raft exercises that reactor path under saturation.
    add_custom_target(run_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS test_masstree
                test_srpc_timeout_race
                test_silo_varint test_silo_rcu_thread test_silo_allocator
                test_sto_transaction test_sto_transaction_real
                simpleTransaction
        COMMENT "Running all tests (Masstree + live Silo support/STO)"
    )
    if(ENABLE_BORROW_CHECKING AND TARGET borrow_check_srpc)
        add_dependencies(run_tests borrow_check_srpc)
    endif()
    add_dependencies(run_tests raft_goal0_source_gate)
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
    add_test(NAME SiloVarintTests
        COMMAND test_silo_varint --gtest_fail_if_no_test_selected)
    set_tests_properties(SiloVarintTests PROPERTIES
        LABELS "silo;sto;rust;serializer")
    if(TARGET rust_sto_integration)
        add_dependencies(rust_sto_integration test_silo_varint)
    endif()

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

    # Test: masstree-rocks-cache capability
    # (openspec/specs/masstree-rocks-cache/spec.md). Masstree as a
    # write-back cache over RocksDB; needs the mako storage objects for
    # concurrent_btree + RCU, and rocksdb for the system of record.
    add_executable(test_masstree_rocks_cache tests/test_masstree_rocks_cache.cc)
    target_link_libraries(test_masstree_rocks_cache
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
    target_include_directories(test_masstree_rocks_cache PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME MasstreeRocksCacheTests COMMAND test_masstree_rocks_cache)

    # Crash-durability tests. Self-execs a child writer and SIGKILLs it,
    # so it must be its own binary (see the file header for why fork()
    # alone is unsound here).
    add_executable(test_masstree_rocks_crash tests/test_masstree_rocks_crash.cc)
    target_link_libraries(test_masstree_rocks_crash
        mako
        srpc
        ${TXLOG_LINK_LIBS}
        ${GTEST_LIBRARIES}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(test_masstree_rocks_crash PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME MasstreeRocksCrashTests COMMAND test_masstree_rocks_crash)

    # The mtx_* C ABI. WordsRoundTripBitIdentical is the tripwire for a
    # masstree vendor bump that starts interpreting the stored word.
    add_executable(test_mtx_abi tests/test_mtx_abi.cc)
    target_link_libraries(test_mtx_abi
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
    target_include_directories(test_mtx_abi PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME MtxAbiTests COMMAND test_mtx_abi)

    # Stable local STO/MassTrans C boundary for the Rust Mako migration.
    add_executable(test_mako_local_abi tests/test_mako_local_abi.cc)
    target_link_libraries(test_mako_local_abi
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
    target_include_directories(test_mako_local_abi PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
        ${GTEST_INCLUDE_DIRS}
    )
    add_test(NAME MakoLocalAbiTests COMMAND test_mako_local_abi)

    # Worker IDs are process-lifetime, so exhaustion must be tested in a
    # disposable process that does not consume the rest of this test run's
    # native slots.
    add_executable(mako_local_thread_limit_probe
        tests/mako_local_thread_limit_probe.cc)
    target_link_libraries(mako_local_thread_limit_probe
        mako
        srpc
        ${TXLOG_LINK_LIBS}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(mako_local_thread_limit_probe PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
    )
    add_test(NAME MakoLocalThreadLimitProbe
        COMMAND mako_local_thread_limit_probe)

    # Process-isolated differential runner. Direct MassTrans and the local C
    # ABI permanently claim different STO worker-runtime identities, so the
    # Rust orchestrator execs this binary once per mode and feeds both the
    # exact same binary-safe transaction corpus.
    add_executable(mako_local_differential_driver
        tests/mako_local_differential_driver.cc)
    target_link_libraries(mako_local_differential_driver
        mako
        srpc
        ${TXLOG_LINK_LIBS}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(mako_local_differential_driver PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
    )

    # Process-isolated relative-overhead runner. Like the differential driver,
    # direct MassTrans and the raw C ABI must execute in separate processes
    # because they permanently claim different STO worker-runtime identities.
    # The Rust integration test supplies separate public-safe and
    # trusted-fast wrapper processes and validates the complete benchmark
    # protocol before calculating ratios.
    add_executable(mako_local_overhead_driver
        tests/mako_local_overhead_driver.cc)
    target_link_libraries(mako_local_overhead_driver
        mako
        srpc
        ${TXLOG_LINK_LIBS}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    target_include_directories(mako_local_overhead_driver PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
    )

    # The public local-transaction boundary must remain consumable as strict C.
    # Compile the C source in an object target so libmako's public C++ compile
    # options cannot leak into it; the final executable still uses the C++
    # linker because it intentionally references every native ABI export.
    add_library(mako_local_abi_c11_probe OBJECT
        tests/test_mako_local_abi_c11.c)
    set_target_properties(mako_local_abi_c11_probe PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    target_compile_options(mako_local_abi_c11_probe PRIVATE
        -Wall -Wextra -Werror -Wpedantic)
    target_include_directories(mako_local_abi_c11_probe PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
    )
    add_executable(test_mako_local_abi_c11
        $<TARGET_OBJECTS:mako_local_abi_c11_probe>)
    set_target_properties(test_mako_local_abi_c11 PROPERTIES
        LINKER_LANGUAGE CXX)
    target_link_libraries(test_mako_local_abi_c11
        mako
        srpc
        ${TXLOG_LINK_LIBS}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    add_test(NAME MakoLocalAbiC11Probe COMMAND test_mako_local_abi_c11)

    # C++ adds the noexcept/calling-type checks that the C type system cannot
    # express. This is also a complete link probe, independent of GoogleTest.
    add_library(mako_local_abi_cpp_conformance_probe OBJECT
        tests/test_mako_local_abi_cpp_conformance.cc)
    target_compile_options(mako_local_abi_cpp_conformance_probe PRIVATE
        -Wall -Wextra -Werror -Wpedantic)
    target_include_directories(mako_local_abi_cpp_conformance_probe PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/mako
    )
    add_executable(test_mako_local_abi_cpp_conformance
        $<TARGET_OBJECTS:mako_local_abi_cpp_conformance_probe>)
    set_target_properties(test_mako_local_abi_cpp_conformance PROPERTIES
        LINKER_LANGUAGE CXX)
    target_link_libraries(test_mako_local_abi_cpp_conformance
        mako
        srpc
        ${TXLOG_LINK_LIBS}
        pthread
        yaml-cpp
        numa
        event_pthreads
        rocksdb
        ${LIBEVENT_LDFLAGS}
    )
    add_test(NAME MakoLocalAbiCppConformance
        COMMAND test_mako_local_abi_cpp_conformance)

    add_test(NAME MakoLocalAbiSymbols
        COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/scripts/check_mako_local_symbols.py"
                --nm "${CMAKE_NM}"
                --archive "$<TARGET_FILE:mako>"
                --allowlist
                  "${CMAKE_SOURCE_DIR}/tests/mako_local_abi_symbols.txt"
                --manifest "${MAKO_LOCAL_BUILD_MANIFEST}"
    )
    add_test(NAME MakoLocalAbiSymbolCheckerUnit
        COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/tests/test_mako_local_symbol_checker.py"
    )
    add_test(NAME MakoLocalBuildFingerprintUnit
        COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/scripts/tests/test_mako_local_fingerprint.py"
    )

    if (TARGET mrxffi)
        # Differential: drive the C++ cache and the Rust cache with the
        # same operation stream and require identical observable
        # behaviour. Two independent implementations of one specification
        # disagreeing is a signal neither one's own tests can produce.
        add_executable(test_mrx_differential tests/test_mrx_differential.cc)
        add_dependencies(test_mrx_differential mrx_ffi_build)
        target_link_libraries(test_mrx_differential
            mrxffi
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
        target_include_directories(test_mrx_differential PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src/mako
            ${CMAKE_SOURCE_DIR}/crates/mrx-ffi/include
            ${GTEST_INCLUDE_DIRS}
        )
        add_test(NAME MrxDifferentialTests COMMAND test_mrx_differential)
    endif()

    # Test: SiloRuntime per-site isolation (moved out of src/masstree/tests
    # because its subject matter is the SiloRuntime, not Masstree).
    add_executable(test_silo_runtime tests/test_silo_runtime.cc)
    target_include_directories(test_silo_runtime PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS}
    )
    target_compile_options(test_silo_runtime PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_silo_runtime
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
    add_test(NAME test_silo_runtime COMMAND test_silo_runtime)
    add_test(NAME test_silo_runtime_concurrency_lifecycle
        COMMAND test_silo_runtime
            --gtest_filter=SiloRuntimeTest.ConcurrentGlobalDefaultInitialization:SiloRuntimeLifetimeTest.LazyServicesHaveBoundedTeardown:CoreStorageTest.*:SpinBarrierTest.*:MasstreeContextEpochTest.*:MassTransEpochIntegrationTest.*:StoThreadIdLifecycleTest.*:StoEpochAdvancerLifecycleTest.*:MakoTimestampTest.*
            --gtest_fail_if_no_test_selected)
    set_tests_properties(test_silo_runtime_concurrency_lifecycle PROPERTIES
        LABELS "silo;masstree;concurrency"
        RUN_SERIAL TRUE
        TIMEOUT 60)
    add_test(NAME test_silo_transitem_lifetime
        COMMAND test_silo_runtime
            --gtest_filter=TransItemLifetimeTest.ReusedSlotRetainsExactlyOneOwnedExtraString
            --gtest_fail_if_no_test_selected)
    add_test(NAME test_silo_transaction_set_exact_chunk_boundary
        COMMAND test_silo_runtime
            --gtest_filter=TransactionSetBoundaryTest.ParticipantAbortCleansExactChunkBoundaries
            --gtest_fail_if_no_test_selected)
    add_test(NAME test_silo_transaction_set_capacity_exhaustion
        COMMAND test_silo_runtime
            --gtest_filter=TransactionSetBoundaryTest.CapacityExhaustionFailsClosedInRelease
            --gtest_fail_if_no_test_selected)
    add_test(NAME test_silo_transaction_install_exception_fail_stop
        COMMAND test_silo_runtime
            --gtest_filter=TransactionCommitFailureTest.InstallExceptionIsFailStop
            --gtest_fail_if_no_test_selected)
    set_tests_properties(
        test_silo_transitem_lifetime
        test_silo_transaction_set_exact_chunk_boundary
        test_silo_transaction_set_capacity_exhaustion
        test_silo_transaction_install_exception_fail_stop
        PROPERTIES
        LABELS "silo;transaction;lifecycle"
        RUN_SERIAL TRUE
        TIMEOUT 120)

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
    add_test(NAME test_mtree_abi
        COMMAND test_mtree_abi --gtest_fail_if_no_test_selected)
    set_tests_properties(test_mtree_abi PROPERTIES
        LABELS "masstree;silo;abi;rust;ffi")
    if(TARGET rust_sto_integration)
        add_dependencies(rust_sto_integration test_mtree_abi)
    endif()

    # The Mako value suffix is a packed byte-stream contract shared by the
    # native STO/Masstree path and the Rust bridge. Exercise every possible
    # trailer alignment plus the production replay parser/materializer under
    # sanitizers, including odd scalar offsets and thread-local value reuse.
    add_executable(test_mako_value_metadata
        tests/test_mako_value_metadata.cc)
    target_include_directories(test_mako_value_metadata PRIVATE
        ${TEST_INCLUDE_DIRS}
        ${GTEST_INCLUDE_DIRS}
    )
    target_compile_options(test_mako_value_metadata PRIVATE
        ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_mako_value_metadata
        mako ${TEST_LINK_LIBS}
        ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES}
        pthread)
    add_test(NAME test_mako_value_metadata
        COMMAND test_mako_value_metadata --gtest_fail_if_no_test_selected)
    set_tests_properties(test_mako_value_metadata PROPERTIES
        LABELS "sto;masstree;rust;metadata;replay")
    if(TARGET rust_sto_integration)
        add_dependencies(rust_sto_integration test_mako_value_metadata)
    endif()

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
    add_test(NAME test_silo_nontxn_api
        COMMAND test_silo_nontxn_api --gtest_fail_if_no_test_selected)
    set_tests_properties(test_silo_nontxn_api PROPERTIES
        LABELS "silo;masstree;nontransactional"
        RUN_SERIAL TRUE
        TIMEOUT 60)
    add_test(NAME test_silo_nontxn_api_mv_delete_delete_conflict
        COMMAND test_silo_nontxn_api
            --gtest_filter=SiloNonTxnApi.ConcurrentMvDeletesOfPresentRowConflict
            --gtest_fail_if_no_test_selected)
    add_test(NAME test_silo_nontxn_api_mv_delete_update_conflict
        COMMAND test_silo_nontxn_api
            --gtest_filter=SiloNonTxnApi.ConcurrentMvDeleteInvalidatesStagedUpdate
            --gtest_fail_if_no_test_selected)
    set_tests_properties(
        test_silo_nontxn_api_mv_delete_delete_conflict
        test_silo_nontxn_api_mv_delete_update_conflict
        PROPERTIES
        LABELS "silo;masstree;nontransactional;mv;concurrency"
        RUN_SERIAL TRUE
        TIMEOUT 60)
    if(STO_RMW)
        # Keep the read-my-writes-only state transitions visible as their own
        # CTests. One exact filter per case plus fail-if-empty prevents either
        # preprocessor/test-name regression from becoming a partial false pass.
        add_test(NAME test_silo_nontxn_api_sto_rmw_insert_delete
            COMMAND test_silo_nontxn_api
                "--gtest_filter=SiloNonTxnApi.MassTransInsertThenDeleteCancelsInMvMode"
                --gtest_fail_if_no_test_selected)
        add_test(NAME test_silo_nontxn_api_sto_rmw_resurrection_delete
            COMMAND test_silo_nontxn_api
                "--gtest_filter=SiloNonTxnApi.MassTransResurrectionThenDeleteIsNetZeroInMvMode"
                --gtest_fail_if_no_test_selected)
        set_tests_properties(
            test_silo_nontxn_api_sto_rmw_insert_delete
            test_silo_nontxn_api_sto_rmw_resurrection_delete
            PROPERTIES
            LABELS "silo;masstree;nontransactional;mv;sto-rmw"
            RUN_SERIAL TRUE
            TIMEOUT 60)
    endif()
    if(TARGET rust_sto_integration)
        add_dependencies(rust_sto_integration test_silo_nontxn_api)
    endif()

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

    # Participant RPC, retained-admission and poll-loop liveness regressions.
    # Admission cases use real loopback/socketpair TCP channels.
    add_executable(test_mako_nontxn_distributed
        tests/test_mako_nontxn_distributed.cc
        tests/test_replication_admission.cc)
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
    add_test(NAME test_mako_nontxn_distributed COMMAND test_mako_nontxn_distributed
        --gtest_filter=-NativeMixedScan.*)
    # Native actors are process-global; run mixed local/paged scans separately
    # from the same executable's static-routing suite.
    add_test(NAME test_mako_native_mixed_scan
        COMMAND ${CMAKE_COMMAND} -E env
            --unset=MAKO_SHARD_METADATA_DIR --unset=MAKO_SHARD_METADATA_INIT
            MAKO_NATIVE_MIXED_SCAN=1 MAKO_CLUSTER_CONFIG=1
            $<TARGET_FILE:test_mako_nontxn_distributed>
            --gtest_filter=NativeMixedScan.LocalAndRemoteSegments)
    set_tests_properties(test_mako_nontxn_distributed test_mako_native_mixed_scan
        PROPERTIES RESOURCE_LOCK mako_distributed_loopback)
    set_tests_properties(test_mako_native_mixed_scan PROPERTIES TIMEOUT 300)
    set_tests_properties(test_mako_nontxn_distributed PROPERTIES
        LABELS "silo;distributed"
        RUN_SERIAL TRUE
        TIMEOUT 60)

    # Keep the existing explicit/manual reactor contract covered alongside the
    # poll-worker fairness regression. Do not register the standalone load test.
    add_executable(test_srpc_reactor_extended
        src/srpc/tests/test_reactor_extended.cc)
    target_include_directories(test_srpc_reactor_extended PRIVATE
        ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
    target_compile_options(test_srpc_reactor_extended PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_libraries(test_srpc_reactor_extended
        srpc ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} pthread)
    add_test(NAME test_srpc_reactor_manual_compatibility
        COMMAND test_srpc_reactor_extended
            --gtest_filter=ExtendedReactorTest.EventChain:ExtendedReactorTest.NestedFibers
            --gtest_fail_if_no_test_selected)
    set_tests_properties(test_srpc_reactor_manual_compatibility PROPERTIES
        LABELS "srpc;reactor;lifecycle"
        RUN_SERIAL TRUE
        TIMEOUT 30)

    # Focused regression for an idle mode-1 helper pinning Masstree RCU while
    # a short-lived ClientTcpServer worker drains its callback queue at exit.
    # thread_end() no longer waits for an older reader, so the L7 run alone
    # cannot hang on that regression; ParkedIdleHelperLeavesMasstreeRcu
    # checks the suspended helper's epochs directly.
    add_test(NAME test_mako_nontxn_distributed_rcu_shutdown
        COMMAND test_mako_nontxn_distributed
            --gtest_filter=MakoNontxnDistributed.L7RemoteTableNontxn:MakoNontxnDistributed.ParkedIdleHelperLeavesMasstreeRcu
            --gtest_fail_if_no_test_selected)
    set_tests_properties(test_mako_nontxn_distributed_rcu_shutdown PROPERTIES
        LABELS "silo;distributed;rcu;lifecycle"
        RUN_SERIAL TRUE
        TIMEOUT 30)

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

    # Exercise the real process-wide clock/slots, nested retained guards, lazy
    # helper activation, and reclamation callbacks through terminal cleanup.
    add_executable(test_sto_epoch_rcu
        src/mako/sto/unit-rcu.cc src/mako/sto/clp.c)
    # CLP supports C++, and it is compiled as C++ because both sources take
    # Masstree's generated C++ config header as a forced include. `mako` no
    # longer propagates that include as a target option (it would reach the
    # synthetic std module unit), so attach it to the two sources directly.
    set_source_files_properties(src/mako/sto/clp.c PROPERTIES LANGUAGE CXX)
    mako_force_include_masstree_config(
        src/mako/sto/unit-rcu.cc src/mako/sto/clp.c)
    target_include_directories(test_sto_epoch_rcu PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(test_sto_epoch_rcu
        mako ${TEST_LINK_LIBS} yaml-cpp rocksdb lz4 numa event_pthreads pthread)
    add_test(NAME STOEpochRCUTests COMMAND test_sto_epoch_rcu)
    set_tests_properties(STOEpochRCUTests PROPERTIES
        ENVIRONMENT "MAKO_CLUSTER_CONFIG=0" TIMEOUT 120)

    if(CARGO_EXECUTABLE)
        set(MAKO_CACHE_NATIVE_CRASH_TEST_ENV)
        if(MAKO_LOCAL_TEST_HOOKS)
            list(APPEND MAKO_CACHE_NATIVE_CRASH_TEST_ENV
                 "MAKO_CACHE_REQUIRE_NATIVE_CRASH_HOOKS=1")
        endif()
        set(MAKO_RUST_TOOLCHAIN_ENV
            "PYTHON=${MAKO_LOCAL_PYTHON_EXECUTABLE}")
        if(DEFINED ENV{LIBCLANG_PATH})
            list(APPEND MAKO_RUST_TOOLCHAIN_ENV
                 "LIBCLANG_PATH=$ENV{LIBCLANG_PATH}")
        endif()
        set(MAKO_LOCAL_CARGO_ENV
            ${MAKO_RUST_TOOLCHAIN_ENV}
            "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/cargo-target-mako-local")
        set(MAKO_CACHE_CARGO_ENV
            ${MAKO_RUST_TOOLCHAIN_ENV}
            "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/cargo-target-mako-cache")

        # Miri is intentionally a Rust-only fake-ABI gate. The runner owns a
        # dated nightly pin and fails loudly when that exact toolchain or Miri
        # component is unavailable; this target must never degrade to `cargo
        # test` or cross into the native C++ archive.
        add_custom_target(run_mako_local_miri_tests
            COMMAND ${CMAKE_COMMAND} -E env
                    "CARGO=${CARGO_EXECUTABLE}"
                    "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/cargo-target-mako-local-miri"
                    bash "${CMAKE_SOURCE_DIR}/scripts/run_mako_local_miri.sh"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running pinned Miri safe-wrapper ownership gate"
            VERBATIM
        )

        add_custom_target(run_mako_local_rust_tests
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_LOCAL_CARGO_ENV}
                    ${CARGO_EXECUTABLE} test --locked
                    -p mako-history --all-targets
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_LOCAL_CARGO_ENV}
                    "MAKO_LOCAL_FAKE_ABI=1"
                    "MAKO_LOCAL_REQUIRE_NATIVE=0"
                    ${CARGO_EXECUTABLE} test --locked -p mako-engine-api --all-targets
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_LOCAL_CARGO_ENV}
                    "MAKO_LOCAL_FAKE_ABI=1"
                    "MAKO_LOCAL_REQUIRE_NATIVE=0"
                    ${CARGO_EXECUTABLE} test --locked -p mako-clock --all-features --all-targets
        COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_LOCAL_CARGO_ENV}
                    "MAKO_LOCAL_FAKE_ABI=1"
                    "MAKO_LOCAL_REQUIRE_NATIVE=0"
                    ${CARGO_EXECUTABLE} test --locked -p mako-local --lib
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_LOCAL_CARGO_ENV}
                    "MAKO_BUILD_DIR=${CMAKE_BINARY_DIR}"
                    "MAKO_LOCAL_FAKE_ABI=0"
                    "MAKO_LOCAL_REQUIRE_NATIVE=1"
                    "MAKO_LOCAL_REQUIRE_DIFFERENTIAL=1"
                    "MAKO_LOCAL_DIFFERENTIAL_DRIVER=$<TARGET_FILE:mako_local_differential_driver>"
                    ${CARGO_EXECUTABLE} test --locked
                    -p mako-local -p mako-local-sys --all-targets
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_LOCAL_CARGO_ENV}
                    "MAKO_BUILD_DIR=${CMAKE_BINARY_DIR}"
                    "MAKO_LOCAL_FAKE_ABI=0"
                    "MAKO_LOCAL_REQUIRE_NATIVE=1"
                    ${CARGO_EXECUTABLE} test --locked -p mako-local --doc
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/crates
            DEPENDS mako mako_local_differential_driver
            COMMENT "Running safe Rust transaction-wrapper tests"
            VERBATIM
        )

        # The native Rust STO engine behind the cache (mako-engine-sto) links
        # the CMake Masstree archives directly. Run its end-to-end tests, the
        # fourth executor of the local-engine differential against the C ABI
        # driver, and the complete cache suites compiled against that engine.
        set(_MAKO_ENGINE_STO_NATIVE_ENV
            "MAKO_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "MAKO_MTREE_NATIVE_INTEGRATION=1"
            "MAKO_MTREE_NATIVE_LIB_DIRS=$<TARGET_FILE_DIR:mako>:$<TARGET_FILE_DIR:masstree>:${_MAKO_CXX_COMPILER_ROOT}/lib"
            "MAKO_MTREE_NATIVE_LIBS=${MAKO_MTREE_RUST_NATIVE_LIBS}"
            "${_MAKO_MTREE_LOADER_PATH_VARIABLE}=${_MAKO_CXX_COMPILER_ROOT}/lib")
        add_custom_target(run_mako_engine_sto_tests
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_LOCAL_CARGO_ENV}
                    ${CARGO_EXECUTABLE} test --locked
                    -p mako-differential --all-targets
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_LOCAL_CARGO_ENV}
                    ${_MAKO_ENGINE_STO_NATIVE_ENV}
                    "MAKO_LOCAL_REQUIRE_DIFFERENTIAL=1"
                    "MAKO_LOCAL_DIFFERENTIAL_DRIVER=$<TARGET_FILE:mako_local_differential_driver>"
                    ${CARGO_EXECUTABLE} test --locked
                    -p mako-engine-sto --all-features --all-targets
                    -- --test-threads=1
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_LOCAL_CARGO_ENV}
                    ${_MAKO_ENGINE_STO_NATIVE_ENV}
                    ${CARGO_EXECUTABLE} test --locked -p mako-cache
                    --no-default-features
                    --features engine-sto-test-hooks,test-support
                    --lib
                    -- --test-threads=1
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_LOCAL_CARGO_ENV}
                    ${_MAKO_ENGINE_STO_NATIVE_ENV}
                    ${CARGO_EXECUTABLE} test --locked -p mako-cache
                    --no-default-features
                    --features engine-sto-test-hooks,test-support
                    --test "*"
                    -- --test-threads=1
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_LOCAL_CARGO_ENV}
                    ${_MAKO_ENGINE_STO_NATIVE_ENV}
                    ${CARGO_EXECUTABLE} test --locked -p mako-cache --doc
                    --no-default-features --features engine-sto
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/crates
            DEPENDS mako masstree mako_local_differential_driver
                    check_mako_local_build_fingerprint
            COMMENT "Running the Rust STO engine, its differential, and the cache on it"
            VERBATIM
        )

        add_custom_target(run_mako_engine_integration_driver_tests
            COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}" -B -m unittest discover
                    -s "${CMAKE_SOURCE_DIR}/scripts"
                    -p test_mako_cache_cross_engine_reopen.py
            COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}" -B -m unittest discover
                    -s "${CMAKE_SOURCE_DIR}/scripts"
                    -p test_mako_engine_sto_mutations.py
            COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}" -B -m unittest discover
                    -s "${CMAKE_SOURCE_DIR}/scripts"
                    -p test_run_rust_sto_cache_tests.py
            COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}" -B -m unittest discover
                    -s "${CMAKE_SOURCE_DIR}/crates/mako-cache/mutations"
                    -p test_runner.py
            COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}" -B -m unittest discover
                    -s "${CMAKE_SOURCE_DIR}/scripts"
                    -p test_mako_engine_benchmark_drivers.py
            COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}" -B -m unittest discover
                    -s "${CMAKE_SOURCE_DIR}/scripts"
                    -p test_run_rust_sto_miri.py
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Checking engine integration acceptance drivers"
            VERBATIM
        )

        # The mako-engine-sto mutation runner compiles each mutant of the
        # Rust STO engine's record, one-put, publication, and budget logic in
        # an isolated workspace and accepts a kill only from its designated
        # exact test. It links the same native archives as the engine tests.
        add_custom_target(run_mako_engine_sto_mutation_tests
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_RUST_TOOLCHAIN_ENV}
                    ${CARGO_EXECUTABLE} fetch --locked
                    --manifest-path "${CMAKE_SOURCE_DIR}/crates/Cargo.toml"
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_RUST_TOOLCHAIN_ENV}
                    ${_MAKO_ENGINE_STO_NATIVE_ENV}
                    "${MAKO_LOCAL_PYTHON_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/crates/mako-engine-sto/mutations/run.py"
                    --cargo "${CARGO_EXECUTABLE}"
                    --temp-root "${CMAKE_BINARY_DIR}/mako-engine-sto-mutation-work"
                    --report "${CMAKE_BINARY_DIR}/mako-engine-sto-mutations.json"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            DEPENDS mako masstree check_mako_local_build_fingerprint
            COMMENT "Running isolated mako-engine-sto mutation gate"
            VERBATIM
        )

        # Both engines must write one durable format. Build the benchmark
        # against each engine in its own target directory, then write a
        # database with one build and recover it with the other, in both
        # directions, through the benchmark's own sample/recover roles. Run
        # the benchmark's protocol and expected-state unit tests for each
        # engine too; a successful process exit must not mask a weak oracle.
        add_custom_target(run_mako_engine_cross_reopen_tests
            COMMAND "${MAKO_LOCAL_PYTHON_EXECUTABLE}" -B -m unittest discover
                    -s "${CMAKE_SOURCE_DIR}/scripts"
                    -p test_mako_cache_cross_engine_reopen.py
            COMMAND ${CMAKE_COMMAND} -E env
                    --unset=CARGO_BUILD_TARGET
                    --unset=CARGO_ENCODED_RUSTFLAGS
                    ${MAKO_RUST_TOOLCHAIN_ENV}
                    "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/cargo-target-mako-cache-bench-c-abi"
                    "MAKO_BUILD_DIR=${CMAKE_BINARY_DIR}"
                    "MAKO_LOCAL_FAKE_ABI=0"
                    "MAKO_LOCAL_REQUIRE_NATIVE=1"
                    ${CARGO_EXECUTABLE} build --locked
                    -p mako-cache-bench --bin mako-cache-bench
                    --no-default-features --features engine-c-abi
            COMMAND ${CMAKE_COMMAND} -E env
                    --unset=CARGO_BUILD_TARGET
                    --unset=CARGO_ENCODED_RUSTFLAGS
                    ${MAKO_RUST_TOOLCHAIN_ENV}
                    "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/cargo-target-mako-cache-bench-c-abi"
                    "MAKO_BUILD_DIR=${CMAKE_BINARY_DIR}"
                    "MAKO_LOCAL_FAKE_ABI=0"
                    "MAKO_LOCAL_REQUIRE_NATIVE=1"
                    ${CARGO_EXECUTABLE} test --locked
                    -p mako-cache-bench --bin mako-cache-bench
                    --no-default-features --features engine-c-abi
                    -- --test-threads=1
            COMMAND ${CMAKE_COMMAND} -E env
                    --unset=CARGO_BUILD_TARGET
                    --unset=CARGO_ENCODED_RUSTFLAGS
                    ${MAKO_RUST_TOOLCHAIN_ENV}
                    "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/cargo-target-mako-cache-bench-sto"
                    ${_MAKO_ENGINE_STO_NATIVE_ENV}
                    ${CARGO_EXECUTABLE} build --locked
                    -p mako-cache-bench --bin mako-cache-bench
                    --no-default-features --features engine-sto
            COMMAND ${CMAKE_COMMAND} -E env
                    --unset=CARGO_BUILD_TARGET
                    --unset=CARGO_ENCODED_RUSTFLAGS
                    ${MAKO_RUST_TOOLCHAIN_ENV}
                    "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/cargo-target-mako-cache-bench-sto"
                    ${_MAKO_ENGINE_STO_NATIVE_ENV}
                    ${CARGO_EXECUTABLE} test --locked
                    -p mako-cache-bench --bin mako-cache-bench
                    --no-default-features --features engine-sto
                    -- --test-threads=1
            COMMAND ${CMAKE_COMMAND} -E env
                    ${_MAKO_ENGINE_STO_NATIVE_ENV}
                    "${MAKO_LOCAL_PYTHON_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/scripts/mako_cache_cross_engine_reopen.py"
                    --binary "c-abi=${CMAKE_BINARY_DIR}/cargo-target-mako-cache-bench-c-abi/debug/mako-cache-bench"
                    --binary "sto=${CMAKE_BINARY_DIR}/cargo-target-mako-cache-bench-sto/debug/mako-cache-bench"
                    --data-root "${CMAKE_BINARY_DIR}/mako-cross-reopen-data"
                    --report "${CMAKE_BINARY_DIR}/mako-engine-cross-reopen.json"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/crates
            DEPENDS mako masstree check_mako_local_build_fingerprint
            COMMENT "Reopening cache databases across the C ABI and Rust STO engines"
            VERBATIM
        )

        # Cleanup-failure seams deliberately quarantine their sacrificial
        # worker and retain uncertain native state. Keep this as a distinct
        # hook-enabled functional profile rather than weakening the production
        # ASan/LSan ownership baseline. Conditional target creation makes a
        # missing -DMAKO_LOCAL_TEST_HOOKS=ON fail at target selection.
        if(MAKO_LOCAL_TEST_HOOKS)
            add_custom_target(run_mako_local_hook_tests
                COMMAND ${CMAKE_CTEST_COMMAND}
                        --test-dir ${CMAKE_BINARY_DIR} --verbose
                        --output-on-failure --no-tests=error
                        -R "^(MakoLocalAbiTests|MakoLocalAbiC11Probe|MakoLocalAbiCppConformance|MakoLocalThreadLimitProbe)$"
                DEPENDS test_mako_local_abi test_mako_local_abi_c11
                        test_mako_local_abi_cpp_conformance
                        mako_local_thread_limit_probe
                        run_mako_local_rust_tests
                        check_mako_local_build_fingerprint
                        check_mako_local_abi_symbols
                COMMENT
                    "Running hook-enabled cleanup/quarantine boundary gate"
                VERBATIM
            )
        endif()

        # Item 4's benchmark is intentionally opt-in: it requires optimized
        # C++ and Rust code, performs warmup plus repeated samples over the
        # complete workload matrix, and records same-host artifacts. Ordinary
        # correctness targets still compile its protocol tests but do not make
        # noisy performance measurements.
        add_custom_target(run_mako_local_overhead_benchmarks
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_BINARY_DIR}/mako-local-overhead-artifacts"
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_RUST_TOOLCHAIN_ENV}
                    "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/cargo-target-mako-local-overhead"
                    "MAKO_BUILD_DIR=${CMAKE_BINARY_DIR}"
                    "MAKO_LOCAL_FAKE_ABI=0"
                    "MAKO_LOCAL_REQUIRE_NATIVE=1"
                    "MAKO_LOCAL_REQUIRE_OVERHEAD=1"
                    "MAKO_LOCAL_OVERHEAD_DRIVER=$<TARGET_FILE:mako_local_overhead_driver>"
                    "MAKO_LOCAL_OVERHEAD_ARTIFACT_DIR=${CMAKE_BINARY_DIR}/mako-local-overhead-artifacts"
                    ${CARGO_EXECUTABLE} test --locked --release
                    -p mako-local --test overhead --
                    --exact wrapper_overhead_gate --nocapture
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/crates
            DEPENDS mako mako_local_overhead_driver
                    check_mako_local_build_fingerprint
                    check_mako_local_abi_symbols
            COMMENT "Running optimized five-surface wrapper-overhead gate"
            VERBATIM
        )

        if(MAKO_LOCAL_SANITIZERS_ENABLED AND MAKO_TSAN_ENABLED
                AND NOT _MAKO_RUST_STATICLIB_TSAN_STD)
            # The Rust half of the TSan gate cannot link against release
            # staticlibs built with the ordinary toolchain (see
            # MAKO_LOCAL_TSAN_RUST_STATICLIBS). Fail at once instead of after
            # the native build.
            add_custom_target(run_mako_local_sanitizer_tests
                COMMAND ${CMAKE_COMMAND} -E echo
                        "run_mako_local_sanitizer_tests under MAKO_TSAN=1 needs -DMAKO_LOCAL_TSAN_RUST_STATICLIBS=ON (./ci/ci.sh makoLocalSanitizerGates passes it)"
                COMMAND ${CMAKE_COMMAND} -E false
                VERBATIM
            )
        elseif(MAKO_LOCAL_SANITIZERS_ENABLED)
            if(_MAKO_RUST_STATICLIB_TSAN_STD
                    AND NOT CARGO_EXECUTABLE STREQUAL MAKO_CARGO_EXECUTABLE)
                # The staticlibs must resolve the same +toolchain through the
                # same Cargo as the test executables below.
                message(FATAL_ERROR
                    "MAKO_LOCAL_TSAN_RUST_STATICLIBS=ON needs one Cargo: "
                    "CARGO_EXECUTABLE=${CARGO_EXECUTABLE}, "
                    "MAKO_CARGO_EXECUTABLE=${MAKO_CARGO_EXECUTABLE}")
            endif()
            set(MAKO_LOCAL_SANITIZER_CARGO_ENV
                ${MAKO_RUST_TOOLCHAIN_ENV}
                ${MAKO_LOCAL_SANITIZER_RUNTIME_ENV}
                "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/cargo-target-mako-local-sanitizer"
                "RUSTFLAGS=${MAKO_LOCAL_SANITIZER_RUSTFLAGS}")

            # Required-native Item 4 gate. The gtest exercises the C ABI
            # directly. The Rust integration suite then drives the same
            # instrumented archive through safe Rust and execs the direct-C++
            # and raw-C-ABI differential children. Sanitizer suppressions are
            # restricted to the reviewed engine/process-lifetime lists above,
            # and any other report terminates the gate.
            add_custom_target(run_mako_local_sanitizer_tests
                COMMAND ${CMAKE_COMMAND} -E env
                        ${MAKO_LOCAL_SANITIZER_RUNTIME_ENV}
                        ${CMAKE_CTEST_COMMAND}
                        --test-dir ${CMAKE_BINARY_DIR} --verbose
                        --output-on-failure --no-tests=error
                        -R "^(MakoLocalAbiTests|MakoLocalAbiC11Probe|MakoLocalAbiCppConformance|MakoLocalThreadLimitProbe)$"
                COMMAND ${CMAKE_COMMAND} -E env
                        ${MAKO_LOCAL_SANITIZER_CARGO_ENV}
                        "MAKO_BUILD_DIR=${CMAKE_BINARY_DIR}"
                        "MAKO_LOCAL_FAKE_ABI=0"
                        "MAKO_LOCAL_REQUIRE_NATIVE=1"
                        "MAKO_LOCAL_REQUIRE_DIFFERENTIAL=1"
                        "MAKO_LOCAL_DIFFERENTIAL_DRIVER=$<TARGET_FILE:mako_local_differential_driver>"
                        ${CARGO_EXECUTABLE}
                        ${MAKO_LOCAL_SANITIZER_CARGO_PREFIX}
                        test --locked
                        ${MAKO_LOCAL_SANITIZER_CARGO_TARGET_ARGS}
                        -p mako-local -p mako-local-sys --all-targets --
                        --test-threads=1 --nocapture
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/crates
                DEPENDS test_mako_local_abi test_mako_local_abi_c11
                        test_mako_local_abi_cpp_conformance
                        mako_local_thread_limit_probe
                        mako_local_differential_driver
                        check_mako_local_build_fingerprint
                        check_mako_local_abi_symbols
                COMMENT
                    "Running required-native mako-local sanitizer boundary gate"
                VERBATIM
            )
        endif()

        # A process has one native cache-order namespace. Individual tests
        # still exercise concurrent workers, but separate cache cases cannot
        # claim that namespace at the same time inside one Rust test binary.
        add_custom_target(run_mako_cache_rust_tests
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_CACHE_CARGO_ENV}
                    "MAKO_BUILD_DIR=${CMAKE_BINARY_DIR}"
                    "MAKO_LOCAL_FAKE_ABI=0"
                    "MAKO_LOCAL_REQUIRE_NATIVE=1"
                    ${MAKO_CACHE_NATIVE_CRASH_TEST_ENV}
                    ${CARGO_EXECUTABLE} test --locked --no-default-features
                    --features engine-c-abi,test-support
                    -p mako-cache --all-targets -- --test-threads=1
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_CACHE_CARGO_ENV}
                    "MAKO_BUILD_DIR=${CMAKE_BINARY_DIR}"
                    "MAKO_LOCAL_FAKE_ABI=0"
                    "MAKO_LOCAL_REQUIRE_NATIVE=1"
                    ${CARGO_EXECUTABLE} test --locked -p mako-cache --doc
                    --no-default-features --features engine-c-abi
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/crates
            DEPENDS mako
            COMMENT "Running Rust transaction-cache tests"
            VERBATIM
        )

        # The mako-clock mutation runner needs no native archive: every
        # mutant is a safe-Rust clock rule and its designated unit test
        # kills it in an isolated workspace. The report lands in the build
        # tree, never the source worktree.
        add_custom_target(run_mako_clock_mutation_tests
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_RUST_TOOLCHAIN_ENV}
                    ${CARGO_EXECUTABLE} fetch --locked
                    --manifest-path "${CMAKE_SOURCE_DIR}/crates/Cargo.toml"
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_RUST_TOOLCHAIN_ENV}
                    "${MAKO_LOCAL_PYTHON_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/crates/mako-clock/mutations/run.py"
                    --cargo "${CARGO_EXECUTABLE}"
                    --temp-root "${CMAKE_BINARY_DIR}/mako-clock-mutation-work"
                    --report "${CMAKE_BINARY_DIR}/mako-clock-mutations.json"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running isolated mako-clock HLC mutation gate"
            VERBATIM
        )

        # The cache protocol must reject the same mutations on the Rust
        # engine. Its observer hooks live in Rust, so this gate also runs
        # against the production hook-off Masstree archive.
        add_custom_target(run_mako_cache_sto_mutation_tests
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_RUST_TOOLCHAIN_ENV}
                    ${CARGO_EXECUTABLE} fetch --locked
                    --manifest-path "${CMAKE_SOURCE_DIR}/crates/Cargo.toml"
            COMMAND ${CMAKE_COMMAND} -E env
                    ${MAKO_RUST_TOOLCHAIN_ENV}
                    ${_MAKO_ENGINE_STO_NATIVE_ENV}
                    "${MAKO_LOCAL_PYTHON_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/crates/mako-cache/mutations/run.py"
                    --engine sto --cargo "${CARGO_EXECUTABLE}"
                    --mako-build-dir "${CMAKE_BINARY_DIR}"
                    --temp-root "${CMAKE_BINARY_DIR}/mako-cache-sto-mutation-work"
                    --report "${CMAKE_BINARY_DIR}/mako-cache-sto-mutations.json"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            DEPENDS mako masstree check_mako_local_build_fingerprint
            COMMENT "Running cache protocol mutations against Rust STO"
            VERBATIM
        )

        # Phase 1F's mutation runner compiles every mutant in an isolated
        # workspace and accepts a kill only from its one designated exact
        # test. It requires the hook-enabled archive for timestamp carriage
        # and native recovery mutants, and writes the authoritative report
        # under the CMake build tree rather than the source worktree.
        if(MAKO_LOCAL_TEST_HOOKS)
            add_custom_target(run_mako_cache_mutation_tests
                # The isolated lockfile normalization is deliberately
                # offline. Prime every locked target dependency first so a
                # clean host also has archives such as windows-link that the
                # Linux build itself never compiles.
                COMMAND ${CMAKE_COMMAND} -E env
                        ${MAKO_RUST_TOOLCHAIN_ENV}
                        ${CARGO_EXECUTABLE} fetch --locked
                        --manifest-path "${CMAKE_SOURCE_DIR}/crates/Cargo.toml"
                COMMAND ${CMAKE_COMMAND} -E env
                        ${MAKO_RUST_TOOLCHAIN_ENV}
                        "MAKO_CACHE_MUTATION_TEMP_ROOT=${CMAKE_BINARY_DIR}/mako-cache-mutation-work"
                        "${MAKO_LOCAL_PYTHON_EXECUTABLE}"
                        "${CMAKE_SOURCE_DIR}/crates/mako-cache/mutations/run.py"
                        --cargo "${CARGO_EXECUTABLE}"
                        --hook-build-dir "${CMAKE_BINARY_DIR}"
                        --report "${CMAKE_BINARY_DIR}/mako-cache-phase1f-mutations.json"
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                DEPENDS mako
                COMMENT "Running isolated Phase 1F transaction-cache mutation gate"
                VERBATIM
            )
        endif()
    endif()

    # Custom target to run all SILO/STO tests
    add_custom_target(run_silo_sto_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                -R "Silo|STO|silo|MakoLocal|kv_backends"
        DEPENDS test_silo_varint test_silo_rcu_thread test_silo_allocator
                test_silo_runtime test_silo_multi_site_stress
                test_silo_nontxn_api test_kv_backends test_mako_local_abi
                test_mako_local_abi_c11
                test_mako_local_abi_cpp_conformance
                mako_local_thread_limit_probe
                check_mako_local_build_fingerprint
                check_mako_local_abi_symbols
                test_sto_transaction test_sto_transaction_real test_sto_epoch_rcu
        COMMENT "Running all Silo/STO tests"
        VERBATIM
    )
    if(TARGET run_mako_local_rust_tests)
        add_dependencies(run_mako_local_rust_tests
            check_mako_local_build_fingerprint
            check_mako_local_abi_symbols)
        add_dependencies(run_silo_sto_tests run_mako_local_rust_tests)
    endif()
    if(TARGET run_mako_cache_rust_tests)
        add_dependencies(run_mako_cache_rust_tests
            check_mako_local_build_fingerprint
            check_mako_local_abi_symbols)
        # The hook profile is the mandatory cache cleanup/crash seam gate too.
        # Keep the dependency one-way: the cache suite requires the hook
        # archive, while its standalone target must remain usable in ordinary
        # hook-off builds.
        if(TARGET run_mako_local_hook_tests)
            add_dependencies(run_mako_local_hook_tests
                run_mako_cache_rust_tests)
        endif()
        add_dependencies(run_silo_sto_tests run_mako_cache_rust_tests)
    endif()
    if(TARGET run_mako_engine_sto_tests)
        # These targets share the native archives and are all required before
        # changing the cache's default engine. Keep both backends in CI.
        add_dependencies(run_silo_sto_tests
            run_mako_engine_integration_driver_tests
            run_mako_engine_sto_tests
            run_mako_engine_sto_mutation_tests
            run_mako_cache_sto_mutation_tests
            run_mako_engine_cross_reopen_tests
            run_mako_clock_mutation_tests)
    endif()
    if(TARGET run_mako_cache_mutation_tests)
        # Run the ordinary baseline before spending time on isolated mutants.
        # The aggregate hook gate is the mandatory CI entry point.
        add_dependencies(run_mako_cache_mutation_tests
            run_mako_cache_rust_tests)
        if(TARGET run_mako_local_hook_tests)
            add_dependencies(run_mako_local_hook_tests
                run_mako_cache_mutation_tests)
        endif()
    endif()

    # Keep this summary honest: it must name only targets this file actually
    # defines. It went stale in 4a06ef0e (which stopped building the complete
    # `src/srpc/tests/` corpus) and then told CI logs that `test_timer`,
    # `test_marshal`, `test_rpc`, `test_rpc_extended`, `test_future`,
    # `test_reactor`, `test_reactor_extended` and `rpcbench` were configured
    # when ninja had no such targets.
    message(STATUS "Test targets configured:")
    message(STATUS "  Reactor Tests:")
    message(STATUS "    - test_srpc_timeout_race: terminal timeout queue cleanup")
    message(STATUS "  Masstree Tests:")
    message(STATUS "    - test_masstree: Core Masstree b-tree regression tests")
    message(STATUS "  Live Silo-support/STO Tests:")
    message(STATUS "    - test_silo_varint: Varint/serializer encoding (29 tests)")
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
