cmake_minimum_required(VERSION 3.30)

# CMake's import-std support is experimental and must be enabled before the
# project command. SRPC requires the same Clang/libc++ module configuration as
# the historical Mako build.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.4)
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
        "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
        "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
endif()

set(SRPC_CLANG22_HINTS
    /home/linuxbrew/.linuxbrew/opt/llvm@22/bin
    $ENV{HOMEBREW_PREFIX}/opt/llvm@22/bin
    /opt/homebrew/opt/llvm@22/bin
    /usr/local/opt/llvm@22/bin
)
if(DEFINED ENV{HOME})
    list(APPEND SRPC_CLANG22_HINTS $ENV{HOME}/.linuxbrew/opt/llvm@22/bin)
endif()
if(NOT DEFINED CMAKE_C_COMPILER)
    find_program(SRPC_CLANG NAMES clang-22 clang
        HINTS ${SRPC_CLANG22_HINTS} NO_DEFAULT_PATH)
    if(NOT SRPC_CLANG)
        find_program(SRPC_CLANG NAMES clang-22)
    endif()
    if(NOT SRPC_CLANG)
        message(FATAL_ERROR "SRPC requires Clang 22 or newer")
    endif()
    set(CMAKE_C_COMPILER "${SRPC_CLANG}" CACHE FILEPATH "SRPC C compiler")
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(SRPC_CLANGXX NAMES clang++-22 clang++
        HINTS ${SRPC_CLANG22_HINTS} NO_DEFAULT_PATH)
    if(NOT SRPC_CLANGXX)
        find_program(SRPC_CLANGXX NAMES clang++-22)
    endif()
    if(NOT SRPC_CLANGXX)
        message(FATAL_ERROR "SRPC requires Clang 22 or newer")
    endif()
    set(CMAKE_CXX_COMPILER "${SRPC_CLANGXX}" CACHE FILEPATH "SRPC C++ compiler")
endif()

get_filename_component(SRPC_CLANG_BINDIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(SRPC_CLANG_ROOT "${SRPC_CLANG_BINDIR}" DIRECTORY)
set(SRPC_BREW_LIBCXX_FLAGS "")
if(EXISTS "${SRPC_CLANG_ROOT}/lib/libc++.so" AND
   SRPC_CLANG_ROOT MATCHES "(Cellar|opt)/llvm")
    set(SRPC_BREW_LIBCXX_FLAGS
        "-L${SRPC_CLANG_ROOT}/lib -Wl,-rpath,${SRPC_CLANG_ROOT}/lib")
endif()
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-stdlib=libc++ ${SRPC_BREW_LIBCXX_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "-stdlib=libc++ ${SRPC_BREW_LIBCXX_FLAGS}")

project(srpc LANGUAGES C CXX ASM)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 22.0)
    message(FATAL_ERROR
        "SRPC requires Clang 22 or newer; found "
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_MODULE_STD ON)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
add_link_options(-stdlib=libc++ -lc++abi)

# Sanitizer configuration for the Goal-0 runtime test battery.
#
# reactor/CANONICAL_CHECKPOINT.md makes ASan and TSan runtime coverage a
# promotion gate.  A sanitized reactor cannot be produced by adding flags to
# the test executables alone: the reactor lives in libsrpc.a and in the BMIs
# that the tests import, so the whole tree (srpc, the generated crate providers
# and the rusty runtime ports) has to be instrumented with the same flags.
# This is therefore a whole-configuration switch; configure a *separate* build
# directory per sanitizer, e.g.
#
#   cmake -S . -B build-asan -G Ninja -DSRPC_SANITIZER=address
#   cmake -S . -B build-tsan -G Ninja -DSRPC_SANITIZER=thread
#
# and build the battery targets there.  The ABI/symbol oracles deliberately
# stay on the un-sanitized production configuration: sanitizer runtimes add
# their own strong symbols and interceptors, so a sanitized archive is not the
# artifact those oracles are about.
set(SRPC_SANITIZER "none" CACHE STRING
    "Sanitizer for the whole build: none|address|thread|undefined")
set_property(CACHE SRPC_SANITIZER PROPERTY STRINGS
    none address thread undefined)
if(NOT SRPC_SANITIZER STREQUAL "none")
    if(SRPC_SANITIZER STREQUAL "address")
        set(SRPC_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
    elseif(SRPC_SANITIZER STREQUAL "thread")
        set(SRPC_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
    elseif(SRPC_SANITIZER STREQUAL "undefined")
        set(SRPC_SANITIZER_FLAGS
            -fsanitize=undefined -fno-sanitize-recover=all
            -fno-omit-frame-pointer)
    else()
        message(FATAL_ERROR
            "Unknown SRPC_SANITIZER '${SRPC_SANITIZER}'; expected "
            "none|address|thread|undefined")
    endif()
    add_compile_options(${SRPC_SANITIZER_FLAGS})
    add_link_options(${SRPC_SANITIZER_FLAGS})
    message(STATUS "SRPC sanitizer build: ${SRPC_SANITIZER}")
endif()

include(CTest)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(CARGO_EXECUTABLE NAMES cargo REQUIRED)

set(ENABLE_BORROW_CHECKING OFF CACHE BOOL "Enable RustyCpp borrow checking")
set(RUSTYCPP_BUILD_TYPE "release" CACHE STRING "rusty-cpp build type")
set(SRPC_RUSTY_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/rusty-cpp")
if(NOT EXISTS "${SRPC_RUSTY_CPP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "rusty-cpp is missing; run `git submodule update --init --recursive`")
endif()
include("${SRPC_RUSTY_CPP_DIR}/cmake/RustyCppSubmodule.cmake")
include("${SRPC_RUSTY_CPP_DIR}/cmake/RustyCppTranspiler.cmake")
create_rustycpp_transpiler_build_target()
add_subdirectory(third-party/rusty-cpp EXCLUDE_FROM_ALL)
foreach(_rusty_port_target IN ITEMS
    vec_port btree_port rc_port std_port std_port_hashbrown rusty_async rusty
    binary_heap_port vec_deque_port cell_port string_port arc_port
    linked_list_port)
    if(TARGET ${_rusty_port_target})
        set_target_properties(${_rusty_port_target} PROPERTIES CXX_EXTENSIONS ON)
    endif()
endforeach()

set(BENCH_CXXFLAGS
    -w
    -Wreturn-type
    -MD
    -MP
    -DRUSTYCPP_DISABLE_ARC_LOG
    -DREUSE_FIBER
    -O2
    -g
    -fno-omit-frame-pointer
)

# srpc: STATIC library compiled from the remaining NATIVE units (C++, C, ASM).
# The canonical Rust providers are not native sources and are now named .rs,
# so these patterns no longer match them at all; the explicit REMOVE_ITEM
# below is kept as a belt-and-braces guard in case a canonical path ever
# re-enters the glob. The .cpp/.cc patterns stay so that genuine C++ added to
# these directories later is still compiled. Migration is measurement-gated —
# see the Goal-0 migration notes in this repository.
file(GLOB SRPC_BENCH_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/base/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/misc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/reactor/*.cc"
    # fiber context-switch trampolines, real assembly (see top-level
    # CMakeLists enable_language(ASM))
    "${CMAKE_CURRENT_SOURCE_DIR}/reactor/*.S"
)

# Remaining C++23 named-module interface units whose Rust is still embedded in
# an inline carrier. Keep this census complete so none is also compiled through
# the regular source glob.
set(SRPC_INLINE_MODULE_SRC
)
set(SRPC_EXPECTED_INLINE_MODULES
)

# Fail closed on both paths and declared provider names. A mere length check
# lets one carrier be replaced by a duplicate while still reporting 37 module
# inputs, which can silently remove a production provider.
set(_SRPC_INLINE_UNIQUE_SRC ${SRPC_INLINE_MODULE_SRC})
list(REMOVE_DUPLICATES _SRPC_INLINE_UNIQUE_SRC)
list(LENGTH SRPC_INLINE_MODULE_SRC _SRPC_INLINE_SRC_COUNT)
list(LENGTH _SRPC_INLINE_UNIQUE_SRC _SRPC_INLINE_UNIQUE_SRC_COUNT)
if(NOT _SRPC_INLINE_SRC_COUNT EQUAL _SRPC_INLINE_UNIQUE_SRC_COUNT)
    message(FATAL_ERROR
        "Goal-0 inline module carrier inventory contains duplicate paths")
endif()
set(_SRPC_ACTUAL_INLINE_MODULES)
foreach(_SRPC_INLINE_SOURCE IN LISTS SRPC_INLINE_MODULE_SRC)
    file(READ "${_SRPC_INLINE_SOURCE}" _SRPC_INLINE_TEXT)
    string(REGEX MATCHALL
        "export[ \t\r\n]+module[ \t\r\n]+srpc\\.[A-Za-z_][A-Za-z0-9_]*"
        _SRPC_INLINE_DECLARATIONS "${_SRPC_INLINE_TEXT}")
    list(LENGTH _SRPC_INLINE_DECLARATIONS _SRPC_INLINE_DECLARATION_COUNT)
    if(NOT _SRPC_INLINE_DECLARATION_COUNT EQUAL 1)
        message(FATAL_ERROR
            "Inline carrier must declare exactly one srpc module: "
            "${_SRPC_INLINE_SOURCE}")
    endif()
    list(GET _SRPC_INLINE_DECLARATIONS 0 _SRPC_INLINE_DECLARATION)
    string(REGEX REPLACE
        ".*srpc\\.([A-Za-z_][A-Za-z0-9_]*)" "\\1"
        _SRPC_INLINE_MODULE "${_SRPC_INLINE_DECLARATION}")
    list(APPEND _SRPC_ACTUAL_INLINE_MODULES "${_SRPC_INLINE_MODULE}")
endforeach()
set(_SRPC_EXPECTED_INLINE_MODULES_SORTED ${SRPC_EXPECTED_INLINE_MODULES})
set(_SRPC_ACTUAL_INLINE_MODULES_SORTED ${_SRPC_ACTUAL_INLINE_MODULES})
list(SORT _SRPC_EXPECTED_INLINE_MODULES_SORTED)
list(SORT _SRPC_ACTUAL_INLINE_MODULES_SORTED)
# Quote both operands.  `set(X)` UNSETS X in CMake, so once every carrier is
# canonical and both inventories are empty, the unquoted form compares the
# literal variable NAMES -- which always differ -- and this guard fires on a
# correct tree.  Quoting compares the (empty) values, which is what is meant.
if(NOT "${_SRPC_ACTUAL_INLINE_MODULES_SORTED}" STREQUAL
       "${_SRPC_EXPECTED_INLINE_MODULES_SORTED}")
    message(FATAL_ERROR
        "Goal-0 inline module declarations differ from the exact retained "
        "inventory; expected=${_SRPC_EXPECTED_INLINE_MODULES_SORTED}; "
        "actual=${_SRPC_ACTUAL_INLINE_MODULES_SORTED}")
endif()
if(SRPC_INLINE_MODULE_SRC)
    list(REMOVE_ITEM SRPC_BENCH_SRC ${SRPC_INLINE_MODULE_SRC})
endif()

# These modules are owned by canonical Rust sources. rusty-cpp generates
# their complete C++ module interfaces; no hand-authored carrier remains.
set(SRPC_GOAL0_CANONICAL_MODULES
    basetypes
    callback_wrapper
    internal_protocol
    stat
    errors
    connection_metrics
    completion_tracker
    rand
    request_options
    reconnect_policy
    circuit_breaker
    connection_state
    heartbeat
    request_queue
    load_balancer
    utils
    frame_codec
    serializable
    serializable_envelope
    future
    logging
    idempotency
    fiber
    misc
    channel
    epoll_wrapper
    pollable_proxy
    callbacks
    inmemory_channel
    fiber_channel
    threading
    debugging
    any_message
    tcp_channel
    reactor
    server
    client
)
set(SRPC_GOAL0_SOURCE_basetypes ${CMAKE_CURRENT_SOURCE_DIR}/base/basetypes.rs)
set(SRPC_GOAL0_SOURCE_callback_wrapper ${CMAKE_CURRENT_SOURCE_DIR}/base/callback_wrapper.rs)
set(SRPC_GOAL0_SOURCE_internal_protocol ${CMAKE_CURRENT_SOURCE_DIR}/rpc/internal_protocol.rs)
set(SRPC_GOAL0_SOURCE_stat ${CMAKE_CURRENT_SOURCE_DIR}/misc/stat.rs)
set(SRPC_GOAL0_SOURCE_errors ${CMAKE_CURRENT_SOURCE_DIR}/rpc/errors.rs)
set(SRPC_GOAL0_SOURCE_connection_metrics ${CMAKE_CURRENT_SOURCE_DIR}/rpc/connection_metrics.rs)
set(SRPC_GOAL0_SOURCE_completion_tracker ${CMAKE_CURRENT_SOURCE_DIR}/rpc/completion_tracker.rs)
set(SRPC_GOAL0_SOURCE_rand ${CMAKE_CURRENT_SOURCE_DIR}/misc/rand.rs)
set(SRPC_GOAL0_SOURCE_request_options ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_options.rs)
set(SRPC_GOAL0_SOURCE_reconnect_policy ${CMAKE_CURRENT_SOURCE_DIR}/rpc/reconnect_policy.rs)
set(SRPC_GOAL0_SOURCE_circuit_breaker ${CMAKE_CURRENT_SOURCE_DIR}/rpc/circuit_breaker.rs)
set(SRPC_GOAL0_SOURCE_connection_state ${CMAKE_CURRENT_SOURCE_DIR}/rpc/connection_state.rs)
set(SRPC_GOAL0_SOURCE_heartbeat ${CMAKE_CURRENT_SOURCE_DIR}/rpc/heartbeat.rs)
set(SRPC_GOAL0_SOURCE_request_queue ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_queue.rs)
set(SRPC_GOAL0_SOURCE_load_balancer ${CMAKE_CURRENT_SOURCE_DIR}/rpc/load_balancer.rs)
set(SRPC_GOAL0_SOURCE_utils ${CMAKE_CURRENT_SOURCE_DIR}/rpc/utils.rs)
set(SRPC_GOAL0_SOURCE_frame_codec ${CMAKE_CURRENT_SOURCE_DIR}/rpc/frame_codec.rs)
set(SRPC_GOAL0_SOURCE_serializable ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable.rs)
set(SRPC_GOAL0_SOURCE_serializable_envelope ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable_envelope.rs)
set(SRPC_GOAL0_SOURCE_future ${CMAKE_CURRENT_SOURCE_DIR}/reactor/future.rs)
set(SRPC_GOAL0_SOURCE_logging ${CMAKE_CURRENT_SOURCE_DIR}/base/logging.rs)
set(SRPC_GOAL0_SOURCE_idempotency ${CMAKE_CURRENT_SOURCE_DIR}/rpc/idempotency.rs)
set(SRPC_GOAL0_SOURCE_fiber ${CMAKE_CURRENT_SOURCE_DIR}/reactor/fiber.rs)
set(SRPC_GOAL0_SOURCE_misc ${CMAKE_CURRENT_SOURCE_DIR}/base/misc.rs)
set(SRPC_GOAL0_SOURCE_channel ${CMAKE_CURRENT_SOURCE_DIR}/rpc/channel.rs)
set(SRPC_GOAL0_SOURCE_epoll_wrapper ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_wrapper.rs)
set(SRPC_GOAL0_SOURCE_pollable_proxy ${CMAKE_CURRENT_SOURCE_DIR}/rpc/pollable_proxy.rs)
set(SRPC_GOAL0_SOURCE_callbacks ${CMAKE_CURRENT_SOURCE_DIR}/rpc/callbacks.rs)
set(SRPC_GOAL0_SOURCE_inmemory_channel ${CMAKE_CURRENT_SOURCE_DIR}/rpc/inmemory_channel.rs)
set(SRPC_GOAL0_SOURCE_fiber_channel ${CMAKE_CURRENT_SOURCE_DIR}/rpc/fiber_channel.rs)
set(SRPC_GOAL0_SOURCE_threading ${CMAKE_CURRENT_SOURCE_DIR}/base/threading.rs)
set(SRPC_GOAL0_SOURCE_debugging ${CMAKE_CURRENT_SOURCE_DIR}/base/debugging.rs)
set(SRPC_GOAL0_SOURCE_any_message ${CMAKE_CURRENT_SOURCE_DIR}/misc/any_message.rs)
set(SRPC_GOAL0_SOURCE_tcp_channel ${CMAKE_CURRENT_SOURCE_DIR}/rpc/tcp_channel.rs)
set(SRPC_GOAL0_SOURCE_reactor ${CMAKE_CURRENT_SOURCE_DIR}/reactor/reactor.rs)
set(SRPC_GOAL0_SOURCE_server ${CMAKE_CURRENT_SOURCE_DIR}/rpc/server.rs)
set(SRPC_GOAL0_SOURCE_client ${CMAKE_CURRENT_SOURCE_DIR}/rpc/client.rs)
set(_SRPC_GOAL0_UNIQUE_MODULES ${SRPC_GOAL0_CANONICAL_MODULES})
list(REMOVE_DUPLICATES _SRPC_GOAL0_UNIQUE_MODULES)
list(LENGTH SRPC_GOAL0_CANONICAL_MODULES _SRPC_GOAL0_MODULE_COUNT)
list(LENGTH _SRPC_GOAL0_UNIQUE_MODULES _SRPC_GOAL0_UNIQUE_MODULE_COUNT)
if(_SRPC_GOAL0_MODULE_COUNT EQUAL 0 OR
   NOT _SRPC_GOAL0_MODULE_COUNT EQUAL _SRPC_GOAL0_UNIQUE_MODULE_COUNT)
    message(FATAL_ERROR
        "Goal-0 canonical module inventory must contain unique names")
endif()

set(SRPC_GOAL0_CRATE_CPP_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/goal0-crate-cpp
)
set(SRPC_GOAL0_CANONICAL_RUST_SRC)
set(SRPC_GOAL0_CRATE_CHILD_SRC)
foreach(_SRPC_GOAL0_MODULE IN LISTS SRPC_GOAL0_CANONICAL_MODULES)
    list(APPEND SRPC_GOAL0_CANONICAL_RUST_SRC
        ${SRPC_GOAL0_SOURCE_${_SRPC_GOAL0_MODULE}})
    list(APPEND SRPC_GOAL0_CRATE_CHILD_SRC
        ${SRPC_GOAL0_CRATE_CPP_DIR}/srpc.${_SRPC_GOAL0_MODULE}.cppm)
endforeach()

set(SRPC_GOAL0_RETIRED_CARRIER_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/base/basetypes.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/base/callback_wrapper.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/internal_protocol.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/stat.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/errors.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/connection_metrics.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/completion_tracker.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/rand.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_options.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/reconnect_policy.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/circuit_breaker.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/connection_state.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/heartbeat.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_queue.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/load_balancer.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/utils.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/frame_codec.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable_envelope.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/future.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/base/logging.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/idempotency.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/fiber.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/base/misc.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/channel.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_wrapper.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/pollable_proxy.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/callbacks.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/inmemory_channel.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/fiber_channel.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/base/threading.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/base/debugging.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/any_message.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/tcp_channel.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/reactor.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/server.rs
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/client.rs
)
set(_SRPC_GOAL0_RETIRED_SORTED ${SRPC_GOAL0_RETIRED_CARRIER_SRC})
set(_SRPC_GOAL0_CANONICAL_SORTED ${SRPC_GOAL0_CANONICAL_RUST_SRC})
set(_SRPC_GOAL0_UNIQUE_RETIRED ${SRPC_GOAL0_RETIRED_CARRIER_SRC})
list(REMOVE_DUPLICATES _SRPC_GOAL0_UNIQUE_RETIRED)
list(LENGTH SRPC_GOAL0_RETIRED_CARRIER_SRC _SRPC_GOAL0_RETIRED_COUNT)
list(LENGTH _SRPC_GOAL0_UNIQUE_RETIRED _SRPC_GOAL0_UNIQUE_RETIRED_COUNT)
list(SORT _SRPC_GOAL0_RETIRED_SORTED)
list(SORT _SRPC_GOAL0_CANONICAL_SORTED)
if(NOT _SRPC_GOAL0_RETIRED_COUNT EQUAL _SRPC_GOAL0_UNIQUE_RETIRED_COUNT OR
   NOT _SRPC_GOAL0_RETIRED_SORTED STREQUAL _SRPC_GOAL0_CANONICAL_SORTED)
    message(FATAL_ERROR
        "Goal-0 retired carriers must equal the unique canonical Rust "
        "source inventory exactly")
endif()
foreach(_SRPC_GOAL0_CANONICAL_SOURCE IN LISTS SRPC_GOAL0_CANONICAL_RUST_SRC)
    if(NOT EXISTS "${_SRPC_GOAL0_CANONICAL_SOURCE}")
        message(FATAL_ERROR
            "Goal-0 canonical Rust source is missing: "
            "${_SRPC_GOAL0_CANONICAL_SOURCE}")
    endif()
endforeach()
# Canonical Rust sources are transpiler inputs, not native C++. They held their
# historical .cpp/.cc paths only long enough for Git to record the C++→Rust
# rename lineage; they are named .rs now, so this removal is a no-op guard.
list(REMOVE_ITEM SRPC_BENCH_SRC ${SRPC_GOAL0_RETIRED_CARRIER_SRC})
set(SRPC_GOAL0_CRATE_ROOT_SRC
    ${SRPC_GOAL0_CRATE_CPP_DIR}/srpc.cppm
)
list(LENGTH SRPC_GOAL0_CANONICAL_RUST_SRC _SRPC_GOAL0_CANONICAL_COUNT)
list(LENGTH SRPC_GOAL0_CRATE_CHILD_SRC _SRPC_GOAL0_CHILD_COUNT)
set(_SRPC_GOAL0_UNIQUE_CANONICAL_RUST_SRC ${SRPC_GOAL0_CANONICAL_RUST_SRC})
set(_SRPC_GOAL0_UNIQUE_CRATE_CHILD_SRC ${SRPC_GOAL0_CRATE_CHILD_SRC})
list(REMOVE_DUPLICATES _SRPC_GOAL0_UNIQUE_CANONICAL_RUST_SRC)
list(REMOVE_DUPLICATES _SRPC_GOAL0_UNIQUE_CRATE_CHILD_SRC)
list(LENGTH _SRPC_GOAL0_UNIQUE_CANONICAL_RUST_SRC
    _SRPC_GOAL0_UNIQUE_CANONICAL_COUNT)
list(LENGTH _SRPC_GOAL0_UNIQUE_CRATE_CHILD_SRC _SRPC_GOAL0_UNIQUE_CHILD_COUNT)
if(NOT _SRPC_GOAL0_CANONICAL_COUNT EQUAL _SRPC_GOAL0_MODULE_COUNT OR
   NOT _SRPC_GOAL0_CHILD_COUNT EQUAL _SRPC_GOAL0_MODULE_COUNT OR
   NOT _SRPC_GOAL0_UNIQUE_CANONICAL_COUNT EQUAL _SRPC_GOAL0_MODULE_COUNT OR
   NOT _SRPC_GOAL0_UNIQUE_CHILD_COUNT EQUAL _SRPC_GOAL0_MODULE_COUNT)
    message(FATAL_ERROR
        "Goal-0 crate production must own one unique Rust/generated "
        "provider pair per canonical module")
endif()

# Production always combines the remaining inline carriers with the canonical
# generated providers. The partial crate root remains compile-only until all
# 37 retained named modules are canonical Rust (CPUInfo was removed entirely).
set(SRPC_MODULE_SRC ${SRPC_INLINE_MODULE_SRC} ${SRPC_GOAL0_CRATE_CHILD_SRC})
set(SRPC_MODULE_BASE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/base
    ${CMAKE_CURRENT_SOURCE_DIR}/misc
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc
    ${SRPC_GOAL0_CRATE_CPP_DIR}
)
set_source_files_properties(${SRPC_GOAL0_CRATE_CHILD_SRC}
    PROPERTIES GENERATED TRUE)

list(LENGTH SRPC_INLINE_MODULE_SRC _SRPC_INLINE_MODULE_COUNT)
list(LENGTH SRPC_MODULE_SRC _SRPC_PRODUCTION_MODULE_COUNT)
set(_SRPC_PRODUCTION_MODULE_NAMES
    ${SRPC_GOAL0_CANONICAL_MODULES}
    ${_SRPC_ACTUAL_INLINE_MODULES}
)
set(_SRPC_UNIQUE_PRODUCTION_MODULE_NAMES ${_SRPC_PRODUCTION_MODULE_NAMES})
list(REMOVE_DUPLICATES _SRPC_UNIQUE_PRODUCTION_MODULE_NAMES)
list(LENGTH _SRPC_UNIQUE_PRODUCTION_MODULE_NAMES
    _SRPC_UNIQUE_PRODUCTION_MODULE_NAME_COUNT)
math(EXPR _SRPC_EXPECTED_INLINE_COUNT
    "37 - ${_SRPC_GOAL0_MODULE_COUNT}")
if(NOT _SRPC_INLINE_MODULE_COUNT EQUAL _SRPC_EXPECTED_INLINE_COUNT OR
   NOT _SRPC_PRODUCTION_MODULE_COUNT EQUAL 37 OR
   NOT _SRPC_UNIQUE_PRODUCTION_MODULE_NAME_COUNT EQUAL 37)
    message(FATAL_ERROR
        "Goal-0 production must contain all 37 retained module providers")
endif()
list(FIND SRPC_MODULE_SRC "${SRPC_GOAL0_CRATE_ROOT_SRC}" _SRPC_GOAL0_ROOT_INDEX)
if(NOT _SRPC_GOAL0_ROOT_INDEX EQUAL -1)
    message(FATAL_ERROR
        "Partial Goal-0 crate root must not be a production module provider")
endif()
foreach(_SRPC_GOAL0_CHILD IN LISTS SRPC_GOAL0_CRATE_CHILD_SRC)
    list(FIND SRPC_MODULE_SRC "${_SRPC_GOAL0_CHILD}" _SRPC_GOAL0_CHILD_INDEX)
    if(_SRPC_GOAL0_CHILD_INDEX EQUAL -1)
        message(FATAL_ERROR
            "Goal-0 production is missing generated provider: ${_SRPC_GOAL0_CHILD}")
    endif()
endforeach()
# Platform implementation units for srpc.epoll_wrapper (Rust std's
# sys-module pattern): exactly one is compiled, selected below.
list(REMOVE_ITEM SRPC_BENCH_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_platform_linux.cc)

set(SRPC_CXXFLAGS ${BENCH_CXXFLAGS})

# The `rusty` umbrella module — historically defined here because the
# 29122d04-lineage rusty-cpp shipped rusty.cppm only as a header on the
# include path. The converged goal0-on-main rusty-cpp defines the `rusty`
# umbrella target itself (linking std_port instead of the deleted
# hashbrown_port, #177), so this block is now a fallback that only fires
# if upstream ever stops providing the target.
if(NOT TARGET rusty)
    add_library(rusty STATIC)
    target_sources(rusty PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${SRPC_RUSTY_CPP_DIR}/include
        FILES
        ${SRPC_RUSTY_CPP_DIR}/include/rusty/rusty.cppm
    )
    target_include_directories(rusty PUBLIC
        ${SRPC_RUSTY_CPP_DIR}/include
    )
    target_compile_features(rusty PUBLIC cxx_std_23)
    # Mako-default toolchain uses `-std=gnu++23` (CMAKE_CXX_EXTENSIONS=ON
    # at the top level — matches how CMake builds std.pcm). The rusty
    # umbrella has to match so consumers in srpc can `import rusty;`
    # without tripping the "GNU extensions was disabled in precompiled
    # file" PCM-config check.
    target_compile_options(rusty PRIVATE
        -O3 -DNDEBUG -march=native -std=gnu++23
    )
    target_link_libraries(rusty PUBLIC
        vec_port btree_port rc_port std_port rusty_async
        binary_heap_port vec_deque_port cell_port string_port arc_port
        linked_list_port
    )
endif()

# Rand, basetypes, and circuit breaker keep only thin calls into the plain-C
# seed and timing kernels. The generated-object and production importer lanes
# supply deterministic definitions for exact draw/destroy and clock behavior.
# This separate executable pulls the real C archive and smoke-tests both seams
# without introducing another generated provider into either comparison lane.
add_library(srpc_goal0_rand_c_kernel STATIC EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/srpc_rand.c
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/srpc_timing.c
)
set_target_properties(srpc_goal0_rand_c_kernel PROPERTIES CXX_MODULE_STD OFF)
target_include_directories(srpc_goal0_rand_c_kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/misc
)
target_link_libraries(srpc_goal0_rand_c_kernel PUBLIC pthread)
add_executable(srpc_goal0_rand_kernel_smoke EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/rand_kernel_smoke.c
)
set_target_properties(srpc_goal0_rand_kernel_smoke PROPERTIES CXX_MODULE_STD OFF)
target_include_directories(srpc_goal0_rand_kernel_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(srpc_goal0_rand_kernel_smoke PRIVATE
    srpc_goal0_rand_c_kernel
)

if(NOT TARGET srpc)
    add_library(srpc STATIC ${SRPC_BENCH_SRC})
    if(SRPC_MODULE_SRC)
        target_sources(srpc PUBLIC
            FILE_SET srpc_modules TYPE CXX_MODULES
            BASE_DIRS ${SRPC_MODULE_BASE_DIRS}
            FILES ${SRPC_MODULE_SRC}
        )
    endif()
    # Goal-0 C demotion: syscall ladders that will never be Rust live as
    # plain C with C signatures (the crate's own extern-C kernels mirror
    # them); the C++ modules keep thin type-converting shims.
    target_sources(srpc PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/srpc_connect.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/srpc_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/srpc_net.c
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/srpc_timing.c
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/srpc_rand.c
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/srpc_io.c
        ${CMAKE_CURRENT_SOURCE_DIR}/base/srpc_base.c
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/srpc_fiber.c
    )
    target_include_directories(srpc PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SRPC_RUSTY_CPP_DIR}/include
    )
    target_compile_options(srpc PUBLIC ${SRPC_CXXFLAGS})
    target_link_libraries(srpc pthread rusty)
    target_sources(srpc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_platform_linux.cc)

    # Remaining hand-authored SRPC C++ module units for borrow checking. The
    # canonical Rust modules are checked by rustc and are not duplicated
    # here as generated C++ providers.
    set(SRPC_BORROW_SRC ${SRPC_INLINE_MODULE_SRC})
    set(_SRPC_BORROW_SORTED ${SRPC_BORROW_SRC})
    set(_SRPC_INLINE_SORTED ${SRPC_INLINE_MODULE_SRC})
    list(SORT _SRPC_BORROW_SORTED)
    list(SORT _SRPC_INLINE_SORTED)
    # Quoted for the same reason as the inline-inventory guard above: both
    # lists are empty now that every carrier is canonical, and `set(X)` with
    # no value unsets X, so the unquoted form would compare variable names.
    if(NOT "${_SRPC_BORROW_SORTED}" STREQUAL "${_SRPC_INLINE_SORTED}")
        message(FATAL_ERROR
            "Borrow-check carriers must equal the remaining inline module "
            "inventory exactly")
    endif()
    # Parse-recovery: rpc/server.rs and rpc/fiber_channel.rs trip a
    # libclang-22 crash on the first parse attempt (clang++ CLI compiles them
    # normally). rusty-cpp retries without module arguments. The architecture
    # context-switch assembly remains deliberately outside this exact set.

    if(ENABLE_BORROW_CHECKING AND SRPC_BORROW_SRC)
        add_library(srpc_borrow OBJECT ${SRPC_BORROW_SRC})
        set_target_properties(srpc_borrow PROPERTIES EXCLUDE_FROM_ALL YES)
        target_include_directories(srpc_borrow PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${SRPC_RUSTY_CPP_DIR}/include
        )
        target_compile_options(srpc_borrow PUBLIC ${SRPC_CXXFLAGS})

        add_borrow_check_target(srpc_borrow)

        # The srpc target owns the FILE_SET CXX_MODULES and is what emits the
        # per-source `.o.modmap` response files (which list `-fmodule-file=`
        # for every dependent BMI) plus the BMIs themselves. If we run the
        # borrow checks before `srpc` has compiled, libclang sees neither —
        # parses proceed with "module 'std' not found" warnings and degraded
        # analysis. Force each individual check target to depend on the
        # `srpc` library target so ninja serializes them after BMI emission.
        # (Adding the dep only to `borrow_check_all_srpc_borrow` is not enough
        # — that's an aggregate, but ninja can still parallelize each per-file
        # check target without seeing the group-level dep.)
        foreach(_BORROW_SRC ${SRPC_BORROW_SRC})
            if(_BORROW_SRC MATCHES "\\.(cpp|cc|cxx)$")
                get_filename_component(_BORROW_FILE_NAME ${_BORROW_SRC} NAME_WE)
                set(_BORROW_CHECK_NAME "borrow_check_srpc_borrow_${_BORROW_FILE_NAME}")
                if(TARGET ${_BORROW_CHECK_NAME})
                    add_dependencies(${_BORROW_CHECK_NAME} srpc)
                endif()
            endif()
        endforeach()

        add_custom_target(borrow_check_srpc
            DEPENDS borrow_check_all_srpc_borrow
            COMMENT "Running borrow checker on SRPC library files"
        )
    elseif(ENABLE_BORROW_CHECKING)
        add_custom_target(borrow_check_srpc
            COMMAND ${CMAKE_COMMAND} -E echo "No SRPC files configured for borrow checking"
            COMMENT "No SRPC files configured for borrow checking"
        )
    endif()
endif()

# Goal 0 is guarded at both source and produced-archive boundaries. These are
# custom targets (rather than configure-time checks) so every production build
# observes changes made after CMake configuration.
if(NOT TARGET build_rusty_cpp_transpiler)
    message(FATAL_ERROR "srpc Goal-0 gates require build_rusty_cpp_transpiler")
endif()
find_program(SRPC_BASH_EXECUTABLE NAMES bash REQUIRED)
find_program(SRPC_CARGO_EXECUTABLE NAMES cargo REQUIRED)

add_custom_target(srpc_goal0_source_gate
    COMMAND
        "${SRPC_BASH_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/srpc_dsl_check.sh"
        "${RUSTYCPP_TRANSPILER}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_srpc_rust.py"
        --check
        --transpiler "${RUSTYCPP_TRANSPILER}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_goal0_standalone.py"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_goal0_contracts.py"
    COMMAND
        "${CMAKE_COMMAND}"
        -E env
        "RUSTFLAGS=-Dwarnings"
        "${SRPC_CARGO_EXECUTABLE}"
        test
        --locked
        --workspace
        --all-targets
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    COMMAND
        "${SRPC_CARGO_EXECUTABLE}"
        clippy
        --locked
        --workspace
        --all-targets
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
        --
        -D warnings
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking remaining inline Rust and canonical rustc crate sources"
    USES_TERMINAL
    VERBATIM
)
add_dependencies(srpc_goal0_source_gate build_rusty_cpp_transpiler)
add_dependencies(srpc srpc_goal0_source_gate)

# Cargo owns the transpiler executable, but build_rusty_cpp_transpiler is an
# always-run custom target and does not declare that executable as a CMake
# output. Depending on the target alone is only an order edge; depending on the
# executable directly would let Ninja scan its old timestamp before Cargo
# replaces it in the same build. Instead, fingerprint the executable after the
# Cargo edge and expose that fingerprint as a declared byproduct. The crate
# generation rule consumes the fingerprint as a normal file dependency.
set(SRPC_GOAL0_TRANSPILER_FINGERPRINT
    "${CMAKE_CURRENT_BINARY_DIR}/goal0-transpiler.fingerprint"
)
add_custom_target(srpc_goal0_transpiler_fingerprint
    COMMAND
        "${CMAKE_COMMAND}"
        "-DINPUT=${RUSTYCPP_TRANSPILER}"
        "-DOUTPUT=${SRPC_GOAL0_TRANSPILER_FINGERPRINT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/update_file_fingerprint.cmake"
    BYPRODUCTS "${SRPC_GOAL0_TRANSPILER_FINGERPRINT}"
    COMMENT "Fingerprinting the Goal-0 rusty-cpp emitter"
    VERBATIM
)
add_dependencies(
    srpc_goal0_transpiler_fingerprint
    build_rusty_cpp_transpiler
)

# CI's incremental regression reads these configure-time paths, touches a real
# emitter input, and verifies that one build both replaces the executable and
# reruns crate generation.
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/goal0-transpiler-executable.txt"
    CONTENT "${RUSTYCPP_TRANSPILER}\n"
)
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/goal0-transpiler-input.txt"
    CONTENT "${RUSTYCPP_DIR}/transpiler/src/main.rs\n"
)

# Crate mode emits all children and the partial root in one invocation. Both
# the production target and dual gate consume this one build-tree generation.
# Only child .cppm files enter SRPC_MODULE_SRC. The dual gate compiles
# srpc.cppm after its children in temporary storage, but production never does.
set(SRPC_GOAL0_CRATE_GENERATED_OUTPUTS
    ${SRPC_GOAL0_CRATE_CHILD_SRC}
    ${SRPC_GOAL0_CRATE_ROOT_SRC}
    ${SRPC_GOAL0_CRATE_CPP_DIR}/CMakeLists.txt
    ${SRPC_GOAL0_CRATE_CPP_DIR}/rusty_hand_slots.md
)
set(SRPC_GOAL0_MODULE_PREAMBLE
    ${CMAKE_CURRENT_SOURCE_DIR}/module-preambles.toml
)
set(SRPC_GOAL0_TYPE_MAP
    ${CMAKE_CURRENT_SOURCE_DIR}/rust-type-map.toml
)
set(SRPC_GOAL0_CPP_MODULE_INDEX
    ${CMAKE_CURRENT_SOURCE_DIR}/cpp-module-index.toml
)
# Keep the crate manifest absolute at the process boundary. The crate
# dependency validator deliberately resolves the exact local `rusty` facade;
# an absolute entry manifest makes that walk independent of WORKING_DIRECTORY.
get_filename_component(
    SRPC_GOAL0_CRATE_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    ABSOLUTE
)
add_custom_command(
    OUTPUT ${SRPC_GOAL0_CRATE_GENERATED_OUTPUTS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SRPC_GOAL0_CRATE_CPP_DIR}"
    COMMAND
        "${RUSTYCPP_TRANSPILER}"
        --crate "${SRPC_GOAL0_CRATE_MANIFEST}"
        --output-dir "${SRPC_GOAL0_CRATE_CPP_DIR}"
        --cxx-namespace srpc
        --flat-import-namespace srpc
        --module-preamble "${SRPC_GOAL0_MODULE_PREAMBLE}"
        --type-map "${SRPC_GOAL0_TYPE_MAP}"
        --cpp-module-index "${SRPC_GOAL0_CPP_MODULE_INDEX}"
    DEPENDS
        srpc_goal0_source_gate
        srpc_goal0_transpiler_fingerprint
        "${SRPC_GOAL0_TRANSPILER_FINGERPRINT}"
        "${SRPC_GOAL0_CRATE_MANIFEST}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
        "${CMAKE_CURRENT_SOURCE_DIR}/rusty-rustc/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/rusty-rustc/src/lib.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/rust-modules.toml"
        "${SRPC_GOAL0_MODULE_PREAMBLE}"
        "${SRPC_GOAL0_TYPE_MAP}"
        "${SRPC_GOAL0_CPP_MODULE_INDEX}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs"
        ${SRPC_GOAL0_CANONICAL_RUST_SRC}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating Goal-0 srpc crate C++ child modules"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(srpc_goal0_crate_codegen
    DEPENDS ${SRPC_GOAL0_CRATE_GENERATED_OUTPUTS}
)
add_dependencies(srpc srpc_goal0_crate_codegen)

# The Python gate links raw archives rather than a CMake target, so carry the
# complete static runtime closure explicitly. Each archive is placed in a
# linker rescan group by the gate; `$<TARGET_FILE:rusty>` alone would discard
# CMake's PUBLIC dependency graph and leave imported-module initializers
# unresolved.
set(SRPC_GOAL0_RUNTIME_TARGETS
    rusty
    rusty_async
    vec_port
    btree_port
    rc_port
    std_port
    std_port_hashbrown
    binary_heap_port
    vec_deque_port
    cell_port
    string_port
    arc_port
    linked_list_port
)
set(SRPC_GOAL0_RUNTIME_LIBRARY_ARGS)
foreach(_SRPC_GOAL0_RUNTIME_TARGET IN LISTS SRPC_GOAL0_RUNTIME_TARGETS)
    if(NOT TARGET ${_SRPC_GOAL0_RUNTIME_TARGET})
        message(FATAL_ERROR
            "Goal-0 crate gate runtime target is unavailable: "
            "${_SRPC_GOAL0_RUNTIME_TARGET}")
    endif()
    list(APPEND SRPC_GOAL0_RUNTIME_LIBRARY_ARGS
        --runtime-library "$<TARGET_FILE:${_SRPC_GOAL0_RUNTIME_TARGET}>")
endforeach()

# Match the configured Clang/libc++ ABI in the gate's direct compile and link
# commands. Also preserve the selected Homebrew libc++ search path/rpath when
# the toolchain setup above supplied one.
set(SRPC_GOAL0_DIRECT_TOOLCHAIN_ARGS)
# The configured `rusty` BMI is compiled with `-march=native` above.  Clang
# requires every importer of that BMI to use the same target-feature set, so
# carry the architecture selection into the gate's direct module commands.
list(APPEND SRPC_GOAL0_DIRECT_TOOLCHAIN_ARGS
    --cxx-flag=-march=native
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND SRPC_GOAL0_DIRECT_TOOLCHAIN_ARGS
        --cxx-flag=-stdlib=libc++
        --link-flag=-lc++abi
    )
    separate_arguments(_SRPC_GOAL0_BREW_LINK_FLAGS
        NATIVE_COMMAND "${SRPC_BREW_LIBCXX_FLAGS}")
    foreach(_SRPC_GOAL0_LINK_FLAG IN LISTS _SRPC_GOAL0_BREW_LINK_FLAGS)
        list(APPEND SRPC_GOAL0_DIRECT_TOOLCHAIN_ARGS
            "--link-flag=${_SRPC_GOAL0_LINK_FLAG}")
    endforeach()
endif()

add_custom_target(srpc_goal0_dual_compile ALL
    COMMAND "$<TARGET_FILE:srpc_goal0_rand_kernel_smoke>"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_srpc_crate_mode.py"
        --transpiler "${RUSTYCPP_TRANSPILER}"
        --generated-dir "${SRPC_GOAL0_CRATE_CPP_DIR}"
        --production-library "$<TARGET_FILE:srpc>"
        --runtime-module-root
            "${CMAKE_BINARY_DIR}/third-party/rusty-cpp/CMakeFiles"
        --dependency-module-root
            "${CMAKE_BINARY_DIR}/CMakeFiles"
        --configured-module-map-root
            "${CMAKE_BINARY_DIR}"
        ${SRPC_GOAL0_RUNTIME_LIBRARY_ARGS}
        ${SRPC_GOAL0_DIRECT_TOOLCHAIN_ARGS}
        --clang "${CMAKE_CXX_COMPILER}"
        --nm "${CMAKE_NM}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking whole srpc crate through rustc and rusty-cpp"
    USES_TERMINAL
    VERBATIM
)
add_dependencies(srpc_goal0_dual_compile
    srpc
    srpc_goal0_crate_codegen
    srpc_goal0_rand_kernel_smoke
    ${SRPC_GOAL0_RUNTIME_TARGETS}
)

# ---------------------------------------------------------------------------
# Goal-0 runtime test battery.
#
# reactor/CANONICAL_CHECKPOINT.md makes generated-C++ multithread/TLS runtime
# coverage a hard promotion gate and names the exact eight incumbent suites
# below.  All eight sources have always lived in tests/, but nothing in this
# build ever compiled or ran them, so that gate had nothing to run.
#
# These targets link the CURRENT production libsrpc.a — whose `srpc.reactor`
# provider is the canonical Rust source `reactor/reactor.rs` —
# which is precisely the incumbent baseline a promoted canonical-Rust Reactor
# has to be compared against.  They do not require the canonical Rust Reactor
# to compile.
# ---------------------------------------------------------------------------
set(SRPC_GTEST_ROOT
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/googletest/googletest)
set(SRPC_RUNTIME_BATTERY_AVAILABLE OFF)
if(EXISTS ${SRPC_GTEST_ROOT}/src/gtest-all.cc AND
   EXISTS ${SRPC_GTEST_ROOT}/include/gtest/gtest.h)
    set(SRPC_RUNTIME_BATTERY_AVAILABLE ON)
else()
    message(WARNING
        "Vendored GoogleTest not found at ${SRPC_GTEST_ROOT}; the Goal-0 "
        "runtime test battery will NOT be built. Run "
        "`git submodule update --init --recursive`.")
endif()

if(SRPC_RUNTIME_BATTERY_AVAILABLE)
    # GoogleTest is compiled from its own sources rather than through its
    # CMakeLists so it inherits this project's exact Clang/libc++ ABI.  A
    # distro libgtest.a is built against libstdc++ and will not link.
    # GoogleTest itself must not consume the std module: it is ordinary
    # header-including C++ and CXX_MODULE_STD would add a needless std BMI
    # dependency to every one of its TUs.
    if(NOT TARGET srpc_gtest)
        add_library(srpc_gtest STATIC ${SRPC_GTEST_ROOT}/src/gtest-all.cc)
        set_target_properties(srpc_gtest PROPERTIES CXX_MODULE_STD OFF)
        target_include_directories(srpc_gtest PUBLIC
            ${SRPC_GTEST_ROOT}/include
            ${SRPC_GTEST_ROOT}
        )
        target_compile_options(srpc_gtest PRIVATE ${BENCH_CXXFLAGS})
        target_compile_definitions(srpc_gtest PUBLIC GTEST_HAS_PTHREAD=1)
        target_link_libraries(srpc_gtest PUBLIC pthread)
    endif()
    if(NOT TARGET srpc_gtest_main)
        add_library(srpc_gtest_main STATIC ${SRPC_GTEST_ROOT}/src/gtest_main.cc)
        set_target_properties(srpc_gtest_main PROPERTIES CXX_MODULE_STD OFF)
        target_compile_options(srpc_gtest_main PRIVATE ${BENCH_CXXFLAGS})
        target_link_libraries(srpc_gtest_main PUBLIC srpc_gtest pthread)
    endif()

    # name -> source. Exactly the eight suites named by the checkpoint.
    set(SRPC_RUNTIME_BATTERY
        test_reactor
        test_reactor_extended
        test_reactor_minimal
        test_timeout_race
        test_and_event
        test_fiber
        test_fiber_runtime
        test_rpc_pollthread_proxy_storage
    )
    set(SRPC_BATTERY_SRC_test_reactor                    tests/test_reactor.cc)
    set(SRPC_BATTERY_SRC_test_reactor_extended           tests/test_reactor_extended.cc)
    set(SRPC_BATTERY_SRC_test_reactor_minimal            tests/test_reactor_minimal.cc)
    set(SRPC_BATTERY_SRC_test_timeout_race               tests/test_timeout_race.cc)
    set(SRPC_BATTERY_SRC_test_and_event                  tests/test_and_event.cc)
    set(SRPC_BATTERY_SRC_test_fiber                      tests/fiber_test.cc)
    set(SRPC_BATTERY_SRC_test_fiber_runtime              tests/fiber_runtime.cc)
    set(SRPC_BATTERY_SRC_test_rpc_pollthread_proxy_storage
        tests/rpc_pollthread_proxy_storage_test.cc)

    # Suites that define their own `int main`. Everything else links
    # gtest_main. test_reactor_minimal is a plain program with no gtest at all.
    set(SRPC_BATTERY_OWN_MAIN
        test_reactor
        test_reactor_extended
        test_reactor_minimal
        test_timeout_race
        test_and_event
        test_fiber
        test_fiber_runtime
    )

    # The battery programs are this build's first pure module *consumers*.
    # CMake 4.4 cannot collate their dyndep: their link closure contains both
    # `srpc` (whose module reference map resolves `std` to
    # @cmake_cxx_std@synth_0) and the plain rusty/port targets (which resolve
    # `std` to @synth_1, because they compile -O3 -march=native).  That is a
    # hard "Disagreement of the location of the 'std' module" error, and
    # neither side of the divergence may be flattened: the ports really are
    # built -march=native, and srpc's flags produce the gated production ABI.
    # So the battery opts out of the scanner and is handed the exact module
    # map srpc itself was built against; see scripts/emit_module_map.py.
    set(SRPC_BATTERY_MODMAP
        ${CMAKE_CURRENT_BINARY_DIR}/goal0-battery-modules.modmap)
    add_custom_command(
        OUTPUT ${SRPC_BATTERY_MODMAP}
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/emit_module_map.py"
            --build-dir "${CMAKE_BINARY_DIR}"
            --modules-json
                "${CMAKE_BINARY_DIR}/CMakeFiles/srpc.dir/CXXModules.json"
            --output "${SRPC_BATTERY_MODMAP}"
        DEPENDS
            srpc
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/emit_module_map.py"
        COMMENT "Collecting the srpc module map for the Goal-0 runtime battery"
        VERBATIM
    )
    add_custom_target(srpc_battery_modmap DEPENDS ${SRPC_BATTERY_MODMAP})

    foreach(_battery_target IN LISTS SRPC_RUNTIME_BATTERY)
        set(_battery_source
            ${CMAKE_CURRENT_SOURCE_DIR}/${SRPC_BATTERY_SRC_${_battery_target}})
        if(NOT EXISTS "${_battery_source}")
            message(FATAL_ERROR
                "Goal-0 runtime battery source is missing: ${_battery_source}")
        endif()
        add_executable(${_battery_target} ${_battery_source})
        # Scanning off: the modmap above already names every BMI, including
        # `std`, so CMake must not also attach one of its synthesised std
        # variants and re-derive a (conflicting) module graph.
        set_target_properties(${_battery_target} PROPERTIES
            CXX_SCAN_FOR_MODULES OFF
            CXX_MODULE_STD OFF)
        target_include_directories(${_battery_target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
        )
        # The response file is attached to the SOURCE, not the target.  A
        # target-level compile option would join that target's compile-settings
        # signature, and CMake then feeds that whole signature -- `@file` and
        # all -- into a synthesised `@cmake_cxx_std@synth_N` std module
        # compile, which runs before the file is generated and dies with
        # "no such file or directory: '@.../goal0-battery-modules.modmap'".
        set_source_files_properties(${_battery_source} PROPERTIES
            COMPILE_OPTIONS "@${SRPC_BATTERY_MODMAP}"
            OBJECT_DEPENDS "${SRPC_BATTERY_MODMAP}")
        target_link_libraries(${_battery_target} PRIVATE srpc pthread)
        add_dependencies(${_battery_target} srpc_battery_modmap)
        list(FIND SRPC_BATTERY_OWN_MAIN ${_battery_target} _battery_own_main)
        if(_battery_own_main EQUAL -1)
            target_link_libraries(${_battery_target} PRIVATE srpc_gtest_main)
        endif()
        if(NOT _battery_target STREQUAL "test_reactor_minimal")
            target_link_libraries(${_battery_target} PRIVATE srpc_gtest)
        endif()
    endforeach()

    add_custom_target(srpc_runtime_battery
        DEPENDS ${SRPC_RUNTIME_BATTERY}
        COMMENT "Goal-0 runtime test battery (incumbent reactor)"
    )
endif()

if(BUILD_TESTING)
    if(SRPC_RUNTIME_BATTERY_AVAILABLE)
        foreach(_battery_target IN LISTS SRPC_RUNTIME_BATTERY)
            add_test(NAME ${_battery_target}
                COMMAND "$<TARGET_FILE:${_battery_target}>")
            # These suites drive real epoll poll threads, fibers and timeouts.
            # Serialise them and give the slowest (ReactorLoadTest/StressTest)
            # room, but keep a hard cap so a livelock reports as a failure
            # rather than hanging the run.
            set_tests_properties(${_battery_target} PROPERTIES
                TIMEOUT 600
                RUN_SERIAL TRUE
                LABELS "runtime_battery")
        endforeach()
    endif()
    add_test(
        NAME srpc_goal0_standalone_structure
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_goal0_standalone.py"
    )
    set_tests_properties(srpc_goal0_standalone_structure PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME srpc_goal0_cargo
        COMMAND "${CMAKE_COMMAND}" -E env RUSTFLAGS=-Dwarnings
            "${CARGO_EXECUTABLE}" test --locked --workspace --all-targets
    )
    set_tests_properties(srpc_goal0_cargo PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME srpc_goal0_contracts
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_goal0_contracts.py"
    )
    set_tests_properties(srpc_goal0_contracts PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(
        NAME srpc_goal0_rand_kernel_smoke
        COMMAND "$<TARGET_FILE:srpc_goal0_rand_kernel_smoke>"
    )
endif()
