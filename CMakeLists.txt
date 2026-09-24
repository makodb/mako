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

# `-march=native` is NOT a performance knob here: it is an ABI/PCM
# compatibility requirement.  Every rusty-cpp port target is compiled
# `-O3 -DNDEBUG -march=native` (third-party/rusty-cpp/CMakeLists.txt:90 for
# vec_port, :278 std_port, :550 rusty, and 16 more -- 19 targets in all),
# and Clang refuses to
# load a BMI whose target-feature set differs from the importing translation
# unit's:
#
#   error: precompiled file '.../vec_port.vec.pcm' was compiled with the
#          target feature '+64bit' but the current translation unit is not
#
# Without this flag every srpc module unit that imports a port BMI fails that
# check (7 targets / 133 errors, zero of them real source errors).  Two other
# places in this file already carry the same flag for exactly this reason --
# the fallback `rusty` umbrella below and `--cxx-flag=-march=native` in the
# Goal-0 gate's direct module commands, whose comment states the rule
# verbatim -- but the `srpc` target itself, which imports the very same BMIs,
# was never given it.  Downstream Mako compiles these same sources with
# `-march=native` applied globally (mako CMakeLists.txt:663), which is why the
# defect is invisible there and only bites the standalone build.
#
# Caveat, deliberately accepted: `-march=native` bakes in *this* machine's
# feature set, so a build tree (and the BMIs in it) is not portable to a host
# with a different CPU.  That is already true of this tree via the rusty-cpp
# ports, so the flag adds no new constraint; making it portable means giving
# rusty-cpp an explicit `-march=` baseline, which is a change in that repo.
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
    -march=native
)

# SRPC_NATIVE_MANIFEST_BEGIN
# Both build systems compile this same reviewed set of C and assembly leaves.
set(SRPC_NATIVE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/scripts/native-kernel-sources.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SRPC_NATIVE_MANIFEST}")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(_SRPC_NATIVE_ARCH x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(_SRPC_NATIVE_ARCH aarch64)
else()
    message(FATAL_ERROR "Unsupported SRPC native architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
file(STRINGS "${SRPC_NATIVE_MANIFEST}" _SRPC_NATIVE_RECORDS)
set(SRPC_NATIVE_SOURCES)
foreach(_SRPC_NATIVE_RECORD IN LISTS _SRPC_NATIVE_RECORDS)
    if(NOT _SRPC_NATIVE_RECORD MATCHES "^(all|x86_64|aarch64) ([a-zA-Z0-9_/.]+)$")
        message(FATAL_ERROR "Invalid native kernel manifest record: ${_SRPC_NATIVE_RECORD}")
    endif()
    set(_SRPC_NATIVE_SELECTOR "${CMAKE_MATCH_1}")
    set(_SRPC_NATIVE_PATH "${CMAKE_MATCH_2}")
    if(_SRPC_NATIVE_SELECTOR STREQUAL "all" OR _SRPC_NATIVE_SELECTOR STREQUAL _SRPC_NATIVE_ARCH)
        list(APPEND SRPC_NATIVE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${_SRPC_NATIVE_PATH}")
    endif()
endforeach()
set(SRPC_BENCH_SRC)
# SRPC_NATIVE_MANIFEST_END

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

# All 37 production providers come from canonical Rust. The empty inline
# inventory remains checked below. The partial crate umbrella is compile-only.
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
    # Both Rust and generated C++ call the same C platform kernels.
    # Retry, timeout, and scheduling policy lives in canonical Rust.
    target_sources(srpc PRIVATE ${SRPC_NATIVE_SOURCES})
    target_include_directories(srpc PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SRPC_RUSTY_CPP_DIR}/include
    )
    target_compile_options(srpc PUBLIC ${SRPC_CXXFLAGS})
    target_link_libraries(srpc pthread rusty)

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
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_test_inventory.py"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_test_inventory.py"
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
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_runtime_parity.py"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_rust_source_audit.py"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_native_kernels.py"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_rust_independence.py"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_native_kernels.py"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_native_abi_bindings.py"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_native_abi_bindings.py"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/rust_source_audit.py"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_rust_independence.py"
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
        test
        --locked
        --workspace
        --doc
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
    COMMENT "Checking canonical Rust, adapter ownership, and native kernel contracts"
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
# Keep the crate manifest absolute so Cargo metadata and module discovery
# remain independent of WORKING_DIRECTORY.
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

# Keep an actual scanned consumer of the runtime umbrella. Canonical providers
# now import individual std ports, so their dependency graph alone does not
# build the umbrella BMI that C++ consumers still import. Match srpc's flags
# and link only the runtime here to avoid the mixed std-module closure below.
add_library(srpc_runtime_imports OBJECT EXCLUDE_FROM_ALL
    tests/runtime_imports.cc
)
set_target_properties(srpc_runtime_imports PROPERTIES CXX_SCAN_FOR_MODULES ON)
target_include_directories(srpc_runtime_imports PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SRPC_RUSTY_CPP_DIR}/include
)
target_compile_options(srpc_runtime_imports PRIVATE ${SRPC_CXXFLAGS})
target_link_libraries(srpc_runtime_imports PRIVATE rusty)

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
    srpc_runtime_imports
    ${SRPC_GOAL0_RUNTIME_TARGETS}
)

# ---------------------------------------------------------------------------
# Goal-0 runtime test battery.
#
# These targets link the production libsrpc.a built from canonical Rust
# providers and the shared native kernels. The suites cover threads, fibers,
# transports, ownership, serialization, and paired Rust/C++ execution.
# ---------------------------------------------------------------------------
set(SRPC_GTEST_ROOT
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party/googletest/googletest)
set(SRPC_RUNTIME_BATTERY_AVAILABLE OFF)
if(EXISTS ${SRPC_GTEST_ROOT}/src/gtest-all.cc AND
   EXISTS ${SRPC_GTEST_ROOT}/include/gtest/gtest.h)
    set(SRPC_RUNTIME_BATTERY_AVAILABLE ON)
elseif(BUILD_TESTING)
    message(FATAL_ERROR
        "BUILD_TESTING requires vendored GoogleTest. Run "
        "`git submodule update --init --recursive`, or explicitly configure "
        "with -DBUILD_TESTING=OFF.")
else()
    message(WARNING
        "Vendored GoogleTest not found at ${SRPC_GTEST_ROOT}; the Goal-0 "
        "runtime test battery will NOT be built. Run "
        "`git submodule update --init --recursive`.")
endif()

# The battery programs and `rpcbench` are this build's pure module *consumers*.
# CMake 4.4 cannot collate their dyndep: their link closure contains both
# `srpc` (whose module reference map resolves `std` to
# @cmake_cxx_std@synth_0) and the plain rusty/port targets (which resolve
# `std` to @synth_1).  That is a hard "Disagreement of the location of the
# 'std' module" error, and neither side of the divergence may be
# flattened: srpc's flags produce the gated production ABI, and the ports'
# flags are rusty-cpp's, in a different repository.  So the consumers opt
# out of the scanner and are handed the exact module map srpc itself was
# built against; see scripts/emit_module_map.py.
#
# NOTE (still true after BENCH_CXXFLAGS gained -march=native): the split is
# NOT caused by -march.  Both sides carry it now, and CMake 4.4.0 still
# synthesises two std variants, because the rest of the settings signature
# differs.  Measured from the generated build.ninja:
#     synth_0 (srpc):  ... -w -Wreturn-type -MD -MP -DRUSTYCPP_DISABLE_ARC_LOG
#                          -DREUSE_FIBER -O2 -g -fno-omit-frame-pointer -march=native
#     synth_1 (ports): ... -O3 -DNDEBUG -march=native
# srpc -> @cmake_cxx_std@synth_0 and rusty/vec_port -> @synth_1 both before
# and after the flag change, so this workaround stays necessary.  (Under
# the pinned CMake 4.3.4 there is a single __cmake_cxx_std_23 target and no
# synth split at all; the modmap is then simply redundant, not wrong.)
#
# This sits OUTSIDE the GoogleTest guard below: it depends on `srpc`, the
# scanned runtime consumer and the emitter; `rpcbench` uses it without gtest.
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
        srpc_runtime_imports
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/emit_module_map.py"
    COMMENT "Collecting the srpc module map for the Goal-0 module consumers"
    VERBATIM
)
add_custom_target(srpc_battery_modmap DEPENDS ${SRPC_BATTERY_MODMAP})

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

    # Runtime suites, including retained-waker teardown and transport coverage.
    set(SRPC_RUNTIME_BATTERY
        test_reactor
        test_reactor_extended
        test_reactor_minimal
        test_timeout_race
        test_and_event
        test_fiber
        test_fiber_runtime
        test_rpc_pollthread_proxy_storage
        test_rpc_transport_matrix
        reactor_stackless_battery
        test_rpc_tcp_channel
        test_rpc_metrics
        test_client_replay
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
    set(SRPC_BATTERY_SRC_test_rpc_transport_matrix
        tests/rpc_transport_matrix_test.cc)
    set(SRPC_BATTERY_SRC_reactor_stackless_battery
        tests/reactor_stackless_battery.cc)
    set(SRPC_BATTERY_SRC_test_rpc_tcp_channel
        tests/rpc_tcp_channel_test.cc)

    set(SRPC_BATTERY_SRC_test_rpc_metrics tests/rpc_metrics_test.cc)
    set(SRPC_BATTERY_SRC_test_client_replay tests/client_replay_test.cc)

    # Suites that define their own `int main`. Everything else links
    # gtest_main. test_reactor_minimal is a plain program with no gtest at all.
    set(SRPC_BATTERY_OWN_MAIN
        test_rpc_metrics
        test_reactor
        test_reactor_extended
        test_reactor_minimal
        test_timeout_race
        test_and_event
        test_fiber
        test_fiber_runtime
    )

    # Documentation suites. They are built through the battery loop below
    # because they need exactly the same plumbing (`import std;` => the
    # modmap response file, the scanner opt-out, gtest_main), but they are
    # deliberately NOT members of SRPC_RUNTIME_BATTERY: that list is the
    # runtime suites, and it also drives the
    # `runtime_battery` ctest label and the RUN_SERIAL / TIMEOUT 600 policy,
    # which exist for suites that drive real epoll threads and fibers.  A
    # docs test only reads a file and searches strings, so it neither belongs
    # under that label nor needs to be serialised against them.
    list(APPEND SRPC_RUNTIME_BATTERY test_native_policy)
    list(APPEND SRPC_BATTERY_OWN_MAIN test_native_policy)
    set(SRPC_BATTERY_SRC_test_native_policy tests/native_policy_test.cc)

    list(APPEND SRPC_RUNTIME_BATTERY test_runtime_parity)
    list(APPEND SRPC_BATTERY_OWN_MAIN test_runtime_parity)
    set(SRPC_BATTERY_SRC_test_runtime_parity tests/runtime_parity_test.cc)

    list(APPEND SRPC_RUNTIME_BATTERY test_serialization_parity)
    set(SRPC_BATTERY_SRC_test_serialization_parity tests/serialization_parity_test.cc)

    set(SRPC_DOCS_BATTERY
        test_rpc_docs_symbols
    )
    set(SRPC_BATTERY_SRC_test_rpc_docs_symbols  tests/rpc_docs_symbols_test.cc)

    # Retained C++ consumer contracts. Rust behavioral counterparts and the
    # disposition of every other test source are recorded in test-inventory.json.
    set(SRPC_CPP_CONTRACT_SOURCES
        rpc_errors_test
        rpc_circuit_breaker_test
        rpc_connection_state_test
        rpc_frame_codec_test
        rpc_heartbeat_test
        rpc_reconnect_policy_test
        rpc_request_queue_test
        rpc_pollable_proxy_facade_test
        rpc_service_proxy_facade_test
        rpc_graceful_shutdown_test
        rpc_restart_detection_test
        rpc_callbacks_test
        rpc_any_message_test
        test_completion_tracker
        test_idempotency
        test_marshal
        test_timer
    )
    foreach(_contract IN LISTS SRPC_CPP_CONTRACT_SOURCES)
        list(APPEND SRPC_RUNTIME_BATTERY ${_contract})
        set(SRPC_BATTERY_SRC_${_contract} tests/${_contract}.cc)
    endforeach()
    list(APPEND SRPC_BATTERY_OWN_MAIN
        rpc_request_queue_test rpc_callbacks_test
        test_completion_tracker test_idempotency test_marshal)

    foreach(_battery_target IN LISTS SRPC_RUNTIME_BATTERY SRPC_DOCS_BATTERY)
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

    # A test-only reactor implementation unit can exercise private canonical
    # worker operations without adding production exports or duplicating them.
    set(_fd_reuse_source ${CMAKE_CURRENT_SOURCE_DIR}/tests/pollworker_fd_reuse_test.cc)
    target_sources(test_reactor_extended PRIVATE ${_fd_reuse_source})
    set_source_files_properties(${_fd_reuse_source} PROPERTIES
        CXX_SCAN_FOR_MODULES OFF
        COMPILE_OPTIONS "@${SRPC_BATTERY_MODMAP}"
        OBJECT_DEPENDS "${SRPC_BATTERY_MODMAP}")

    # The documentation guard checks both the Rust guide and C++ companion.
    # Absolute source paths also work when CTest runs from the build tree.
    target_compile_definitions(test_rpc_docs_symbols PRIVATE
        "SRPC_BOOK_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/docs/srpc-book.md\""
        "SRPC_CPP_BOOK_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/docs/srpc-cpp-book.md\"")

    add_custom_target(srpc_runtime_battery
        DEPENDS ${SRPC_RUNTIME_BATTERY}
        COMMENT "Canonical runtime and Rust/C++ parity test battery"
    )
endif()

# ---------------------------------------------------------------------------
# rpcbench -- the throughput benchmark.
#
# The sources have been tracked since the `rrr` era, but nothing built them:
# the recipe lived outside the repository, so every measured qps number was
# unreproducible by anyone but its author.  This target is that recipe.
#
# EXCLUDE_FROM_ALL on purpose.  A benchmark is not a correctness gate, and the
# C++ lane is already minutes cold; `ALL` stays the two gate targets.  Build it
# explicitly:
#     cmake --build build --target rpcbench
#     scripts/run_rpcbench.sh build/rpcbench <tag>
#
# It is deliberately NOT registered with ctest: throughput is not pass/fail,
# and it binds a real TCP port.
# ---------------------------------------------------------------------------
set(SRPC_RPCBENCH_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/rpcbench.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_service.cc)

# tests/benchmark_service.h is GENERATED by pylib/simplerpcgen and opens with
# the monorepo-relative `#include "srpc/srpc.hpp"`.  That include cannot be
# edited here -- regenerating the header would silently restore it, and
# regenerating is already hazardous (RPC ids are stabilised by scraping the
# previous header).  So give it a directory in which `srpc/` *is* this source
# tree, and leave the generated file untouched.
set(SRPC_BENCH_INCLUDE_SHIM ${CMAKE_CURRENT_BINARY_DIR}/bench-include)
file(MAKE_DIRECTORY ${SRPC_BENCH_INCLUDE_SHIM})
if(NOT EXISTS ${SRPC_BENCH_INCLUDE_SHIM}/srpc)
    file(CREATE_LINK
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SRPC_BENCH_INCLUDE_SHIM}/srpc
        SYMBOLIC)
endif()

add_executable(rpcbench EXCLUDE_FROM_ALL ${SRPC_RPCBENCH_SRC})
# Same reasoning as the battery: the modmap already names every BMI including
# `std`, so CMake must not synthesise a second, conflicting one.
set_target_properties(rpcbench PROPERTIES
    CXX_SCAN_FOR_MODULES OFF
    CXX_MODULE_STD OFF)
target_include_directories(rpcbench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${SRPC_BENCH_INCLUDE_SHIM}
)
# Source scope, not target scope -- see the battery's note above: a
# target-level `@response` joins the compile-settings signature and CMake
# then feeds it into a synthesised std compile that runs before the file
# exists.
set_source_files_properties(${SRPC_RPCBENCH_SRC} PROPERTIES
    COMPILE_OPTIONS "@${SRPC_BATTERY_MODMAP}"
    OBJECT_DEPENDS "${SRPC_BATTERY_MODMAP}")
target_link_libraries(rpcbench PRIVATE srpc pthread)
add_dependencies(rpcbench srpc_battery_modmap)

# Every test registered below carries the label `srpc`, because
# add_subdirectory(third-party/rusty-cpp) also registers ~69 tests of its own
# whose executables are NOT in ALL.  A bare `ctest` therefore reports them as
# "Not Run" and exits non-zero; `ctest -L srpc` selects exactly this project's.
if(BUILD_TESTING)
    add_test(NAME srpc_test_inventory
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_test_inventory.py"
            --configured "${CMAKE_CURRENT_BINARY_DIR}/srpc-test-targets.txt")
    set_tests_properties(srpc_test_inventory PROPERTIES LABELS "srpc")
    add_test(NAME srpc_test_inventory_controls
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_test_inventory.py")
    set_tests_properties(srpc_test_inventory_controls PROPERTIES LABELS "srpc")
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
                LABELS "runtime_battery;srpc")
        endforeach()
        add_test(NAME srpc_runtime_parity
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_runtime_parity.py"
                --repo "${CMAKE_CURRENT_SOURCE_DIR}"
                --cargo "${CARGO_EXECUTABLE}"
                --cpp-exe "$<TARGET_FILE:test_runtime_parity>")
        set_tests_properties(srpc_runtime_parity PROPERTIES
            TIMEOUT 300
            RUN_SERIAL TRUE
            LABELS "runtime_battery;srpc")
        add_test(NAME srpc_runtime_parity_negative_controls
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_runtime_parity.py")
        set_tests_properties(srpc_runtime_parity_negative_controls PROPERTIES
            TIMEOUT 30
            LABELS "srpc")
        # Pure file I/O and string search: no reactor, no serialisation, and
        # a short timeout.  Not labelled `runtime_battery`.
        add_test(NAME test_rpc_docs_symbols
            COMMAND "$<TARGET_FILE:test_rpc_docs_symbols>")
        set_tests_properties(test_rpc_docs_symbols PROPERTIES
            TIMEOUT 120
            LABELS "docs;srpc")
    endif()
    add_test(
        NAME srpc_goal0_standalone_structure
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_goal0_standalone.py"
    )
    set_tests_properties(srpc_goal0_standalone_structure PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS "srpc")
    add_test(
        NAME srpc_goal0_cargo
        COMMAND "${CMAKE_COMMAND}" -E env RUSTFLAGS=-Dwarnings
            "${CARGO_EXECUTABLE}" test --locked --workspace --all-targets
    )
    set_tests_properties(srpc_goal0_cargo PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS "srpc")
    add_test(
        NAME srpc_goal0_contracts
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_goal0_contracts.py"
    )
    set_tests_properties(srpc_goal0_contracts PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS "srpc")
    add_test(
        NAME srpc_native_abi_bindings
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_native_abi_bindings.py"
    )
    set_tests_properties(srpc_native_abi_bindings PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS "srpc")
    add_test(
        NAME srpc_native_abi_binding_controls
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_native_abi_bindings.py"
    )
    set_tests_properties(srpc_native_abi_binding_controls PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS "srpc")
    # The canonical body audit builds its AST scanner independently of the transpiler.
    add_test(
        NAME srpc_canonical_runtime_controls
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_rust_source_audit.py"
    )
    set_tests_properties(srpc_canonical_runtime_controls PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS "srpc")
    add_test(
        NAME srpc_native_kernel_controls
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_native_kernels.py"
    )
    set_tests_properties(srpc_native_kernel_controls PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS "srpc")
    add_test(
        NAME srpc_rust_independence_controls
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_rust_independence.py"
    )
    set_tests_properties(srpc_rust_independence_controls PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS "srpc")
    add_test(
        NAME srpc_canonical_runtime
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/rust_source_audit.py"
    )
    set_tests_properties(srpc_canonical_runtime PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS "srpc")
    # Build a copied Cargo tree containing no facade, C++ runtime or compiler.
    add_test(
        NAME srpc_rust_independence
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_rust_independence.py"
    )
    set_tests_properties(srpc_rust_independence PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS "srpc")
    add_test(
        NAME srpc_goal0_rand_kernel_smoke
        COMMAND "$<TARGET_FILE:srpc_goal0_rand_kernel_smoke>"
    )
    set_tests_properties(srpc_goal0_rand_kernel_smoke PROPERTIES
        LABELS "srpc")
    # Lint half only: it checks that every ```cpp fence in the book carries
    # exactly one known tag.  That needs no compiler, no build tree and no
    # submodule, so it runs everywhere.  The compile half of the same script
    # is deliberately NOT wired up: it needs a configured build tree so it can
    # pick up goal0-battery-modules.modmap.
    add_test(
        NAME srpc_docs_snippet_lint
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/rpc_docs_snippet_compile_test.py"
            --lint-only
            --book "${CMAKE_CURRENT_SOURCE_DIR}/docs/srpc-cpp-book.md"
            --repo "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    set_tests_properties(srpc_docs_snippet_lint PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS "srpc")
endif()

# A glob is only the census. Targets stay explicit, and every exclusion names
# its reason and canonical behavioral coverage in the reviewed inventory.
file(GLOB_RECURSE _SRPC_TEST_CENSUS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-inventory.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_test_inventory.py")
get_property(_SRPC_CONFIGURED_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
get_property(_SRPC_CONFIGURED_TESTS DIRECTORY PROPERTY TESTS)
set(_SRPC_TEST_TARGET_RECORDS "")
foreach(_target IN LISTS _SRPC_CONFIGURED_TARGETS)
    get_target_property(_sources ${_target} SOURCES)
    get_target_property(_excluded_from_all ${_target} EXCLUDE_FROM_ALL)
    set(_default_build yes)
    if(_excluded_from_all)
        set(_default_build no)
    endif()
    foreach(_source IN LISTS _sources)
        get_filename_component(_absolute "${_source}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        file(RELATIVE_PATH _relative "${CMAKE_CURRENT_SOURCE_DIR}" "${_absolute}")
        if(_relative MATCHES "^tests/.*\\.cc$")
            set(_registered no)
            if(_target IN_LIST _SRPC_CONFIGURED_TESTS)
                set(_registered yes)
            endif()
            string(APPEND _SRPC_TEST_TARGET_RECORDS
                "${_relative}|${_target}|${_registered}|${_default_build}\n")
        endif()
    endforeach()
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/srpc-test-targets.txt" "${_SRPC_TEST_TARGET_RECORDS}")
set(_SRPC_TEST_INVENTORY_ARGS)
if(NOT BUILD_TESTING)
    list(APPEND _SRPC_TEST_INVENTORY_ARGS --no-testing)
endif()
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_test_inventory.py"
        --configured "${CMAKE_CURRENT_BINARY_DIR}/srpc-test-targets.txt"
        ${_SRPC_TEST_INVENTORY_ARGS}
    RESULT_VARIABLE _SRPC_TEST_INVENTORY_RESULT
    OUTPUT_VARIABLE _SRPC_TEST_INVENTORY_OUTPUT
    ERROR_VARIABLE _SRPC_TEST_INVENTORY_ERROR)
if(NOT _SRPC_TEST_INVENTORY_RESULT EQUAL 0)
    message(FATAL_ERROR "${_SRPC_TEST_INVENTORY_OUTPUT}${_SRPC_TEST_INVENTORY_ERROR}")
endif()
message(STATUS "${_SRPC_TEST_INVENTORY_OUTPUT}")
