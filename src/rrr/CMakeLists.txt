# rrr: STATIC library compiled from .cpp/.cc implementation units.
# Files migrated to C++23 named modules keep their .cpp/.cc extension
# when they are interfaces and are wired in via FILE_SET CXX_MODULES below
# (RRR_MODULE_SRC). Primary module implementation units stay ordinary scanned
# target sources. Migrated legacy paths are explicitly removed from the regular
# source glob to avoid double-compile. Migration is incremental and
# measurement-gated — see docs/dev/srpc_module_migration_plan.md.
file(GLOB RRR_BENCH_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/base/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/misc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/reactor/*.cc"
    # fiber context-switch trampolines, real assembly (see top-level
    # CMakeLists enable_language(ASM))
    "${CMAKE_CURRENT_SOURCE_DIR}/reactor/*.S"
)

# C++23 named-module interface units. Most entries are legacy .cpp/.cc files;
# the srpc pilot entries are checked-in .cppm artifacts generated from Rust.
# Each contains `export module rrr.<name>;`. Matching .hpp/.h files remain as
# forward-declaration shims for non-importing consumers during the transition.
set(SRPC_GENERATED_MODULE_DIR
    ${CMAKE_SOURCE_DIR}/crates/srpc/cpp/generated)
set(SRPC_CPP_CONSUMER_MANIFEST
    ${CMAKE_SOURCE_DIR}/crates/srpc/cpp/mako-consumer.toml)
set(SRPC_CPP_GENERATOR
    ${CMAKE_SOURCE_DIR}/scripts/generate_srpc_cpp.py)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${SRPC_CPP_CONSUMER_MANIFEST}
    ${SRPC_CPP_GENERATOR}
)

# Ordinary C++ builds consume checked-in modules and do not need rustc or the
# transpiler. Fail closed at configure time if their offline ownership/source/
# integrity stamps drift from the Rust sources or consumer profile.
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
execute_process(
    COMMAND
        "${Python3_EXECUTABLE}"
        "${SRPC_CPP_GENERATOR}"
        --check-stamps
        --manifest "${SRPC_CPP_CONSUMER_MANIFEST}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE SRPC_CPP_STAMP_CHECK_RESULT
    OUTPUT_VARIABLE SRPC_CPP_STAMP_CHECK_STDOUT
    ERROR_VARIABLE SRPC_CPP_STAMP_CHECK_STDERR
)
if(NOT SRPC_CPP_STAMP_CHECK_RESULT STREQUAL "0")
    string(STRIP
        "${SRPC_CPP_STAMP_CHECK_STDOUT}\n${SRPC_CPP_STAMP_CHECK_STDERR}"
        SRPC_CPP_STAMP_CHECK_DIAGNOSTIC)
    message(FATAL_ERROR
        "srpc generated C++ module integrity check failed:\n"
        "${SRPC_CPP_STAMP_CHECK_DIAGNOSTIC}\n"
        "Regenerate with scripts/generate_srpc_cpp.py --write")
endif()
message(STATUS "Verified srpc generated C++ module integrity stamps")

set(SRPC_CPP_CMAKE_MANIFEST
    ${CMAKE_CURRENT_BINARY_DIR}/srpc-generated-modules.cmake)
execute_process(
    COMMAND
        "${Python3_EXECUTABLE}"
        "${SRPC_CPP_GENERATOR}"
        --emit-cmake
        --manifest "${SRPC_CPP_CONSUMER_MANIFEST}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE SRPC_CPP_CMAKE_RESULT
    OUTPUT_FILE "${SRPC_CPP_CMAKE_MANIFEST}"
    ERROR_VARIABLE SRPC_CPP_CMAKE_STDERR
)
if(NOT SRPC_CPP_CMAKE_RESULT STREQUAL "0")
    string(STRIP "${SRPC_CPP_CMAKE_STDERR}" SRPC_CPP_CMAKE_DIAGNOSTIC)
    message(FATAL_ERROR
        "srpc generated module manifest export failed:\n"
        "${SRPC_CPP_CMAKE_DIAGNOSTIC}")
endif()
include("${SRPC_CPP_CMAKE_MANIFEST}")

set(RRR_MODULE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/base/basetypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/callback_wrapper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/debugging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/logging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/misc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/threading.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/any_message.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/cpuinfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/rand.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable_envelope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_wrapper.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/fiber.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/future.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/reactor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/callbacks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/circuit_breaker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/completion_tracker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/connection_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/errors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/fiber_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/frame_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/heartbeat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/idempotency.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/inmemory_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/load_balancer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/pollable_proxy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/reconnect_policy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_options.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_queue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/tcp_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/utils.cpp
    ${SRPC_GENERATED_MODULE_INTERFACE_FILES}
)
# A manifest entry atomically replaces its legacy module. This removal also
# makes adding future generated units a profile-only operation rather than a
# second hand-maintained CMake list.
list(REMOVE_ITEM RRR_MODULE_SRC
    ${SRPC_RETIRED_LEGACY_MODULE_FILES}
)
# Remove retired paths from the ordinary source glob so each generated named
# module has exactly one compilation unit. Keeping the paths explicit also
# makes stale files in a no-delete build mirror harmless.
list(REMOVE_ITEM RRR_BENCH_SRC
    ${RRR_MODULE_SRC}
    ${SRPC_RETIRED_LEGACY_MODULE_FILES}
)
# Legacy platform implementation units for rrr.epoll_wrapper (Rust std's
# sys-module pattern): exactly one is compiled until the corresponding
# manifest unit retires it.  Keep this as a list so generated implementation
# units can atomically replace the selected legacy source through the same
# SRPC_RETIRED_LEGACY_MODULE_FILES ownership set as interfaces.
set(RRR_PLATFORM_MODULE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_platform_linux.cc)
list(REMOVE_ITEM RRR_BENCH_SRC
    ${RRR_PLATFORM_MODULE_SRC})
list(REMOVE_ITEM RRR_PLATFORM_MODULE_SRC
    ${SRPC_RETIRED_LEGACY_MODULE_FILES})

set(RRR_CXXFLAGS ${BENCH_CXXFLAGS})

# The `rusty` umbrella module — defined here (not upstream in rusty-cpp)
# because rusty-cpp ships rusty.cppm only as a header on the include
# path, leaving consumers responsible for compiling it as a named
# C++20 module unit. We re-export the transpiled port libraries
# (vec_port, btree_port, rc_port, hashbrown_port, rusty_async) so that
# `import rusty;` in rrr sources resolves rusty::Vec / Rc / BTreeMap
# / HashMap / etc. via the umbrella's `using` aliases.
if(NOT TARGET rusty)
    add_library(rusty STATIC)
    target_sources(rusty PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_SOURCE_DIR}/third-party/rusty-cpp/include
        FILES
        ${CMAKE_SOURCE_DIR}/third-party/rusty-cpp/include/rusty/rusty.cppm
    )
    target_include_directories(rusty PUBLIC
        ${CMAKE_SOURCE_DIR}/third-party/rusty-cpp/include
    )
    target_compile_features(rusty PUBLIC cxx_std_23)
    # Mako-default toolchain uses `-std=gnu++23` (CMAKE_CXX_EXTENSIONS=ON
    # at the top level — matches how CMake builds std.pcm). The rusty
    # umbrella has to match so consumers in rrr can `import rusty;`
    # without tripping the "GNU extensions was disabled in precompiled
    # file" PCM-config check.
    target_compile_options(rusty PRIVATE
        -O3 -DNDEBUG -march=native -std=gnu++23
    )
    target_link_libraries(rusty PUBLIC
        vec_port btree_port rc_port hashbrown_port rusty_async
        binary_heap_port vec_deque_port cell_port string_port arc_port
        linked_list_port
    )
endif()

if(NOT TARGET rrr)
    add_library(rrr STATIC ${RRR_BENCH_SRC})
    if(RRR_MODULE_SRC)
        target_sources(rrr PUBLIC
            FILE_SET rrr_modules TYPE CXX_MODULES
            BASE_DIRS
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${SRPC_GENERATED_MODULE_DIR}
            FILES ${RRR_MODULE_SRC}
        )
    endif()
    # Primary module implementation units are not valid members of a
    # CXX_MODULES file set. Add generated implementations as ordinary private
    # sources after their interfaces have been registered, and force module
    # scanning so CMake's dyndep graph orders each unit after its interface.
    if(SRPC_GENERATED_MODULE_IMPLEMENTATION_FILES)
        set_source_files_properties(
            ${SRPC_GENERATED_MODULE_IMPLEMENTATION_FILES}
            PROPERTIES CXX_SCAN_FOR_MODULES ON
        )
        target_sources(rrr PRIVATE
            ${SRPC_GENERATED_MODULE_IMPLEMENTATION_FILES}
        )
    endif()
    # Goal-0 C demotion: syscall ladders that will never be Rust live as
    # plain C with C signatures (the crate's own extern-C kernels mirror
    # them); the C++ modules keep thin type-converting shims.
    target_sources(rrr PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/srpc_connect.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/srpc_net.c
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/srpc_timing.c
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/srpc_rand.c
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/srpc_io.c
        ${CMAKE_CURRENT_SOURCE_DIR}/base/srpc_base.c
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/srpc_fiber.c
    )
    # `src/compat` ships shims that fix third-party header gotchas
    # without modifying the submodule (e.g.
    # `compat/rusty/function.hpp` adds `#include <cstdlib>` before
    # delegating to the upstream `rusty/function.hpp` via
    # `#include_next`). It MUST come BEFORE
    # `third-party/rusty-cpp/include` on the include path so the
    # shim wins for the names it overrides — use the BEFORE keyword
    # since target_include_directories(... PUBLIC ...) otherwise
    # appends (root-level include_directories() entries already place
    # third-party before src/compat).
    target_include_directories(rrr BEFORE PUBLIC
        ${CMAKE_SOURCE_DIR}/src/compat
    )
    target_include_directories(rrr PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/rrr
        ${CMAKE_SOURCE_DIR}/third-party/rusty-cpp/include
    )
    target_compile_options(rrr PUBLIC ${RRR_CXXFLAGS})
    target_link_libraries(rrr pthread rusty)
    if(RRR_PLATFORM_MODULE_SRC)
        target_sources(rrr PRIVATE ${RRR_PLATFORM_MODULE_SRC})
    endif()

    # RRR files for borrow checking — every non-test, hand-maintained C++
    # module unit in rrr except for the two genuine exclusions called out at
    # the bottom. Rust-owned generated units are documented inline.
    set(RRR_BORROW_SRC
        # base/
        ${CMAKE_CURRENT_SOURCE_DIR}/base/basetypes.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/callback_wrapper.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/debugging.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/logging.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/misc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/threading.cpp
            # misc/
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/any_message.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/cpuinfo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/rand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable_envelope.cpp
        # Manifest-owned generated modules are emitted from crates/srpc Rust
        # sources. rustc owns their source-level safety checks; checked-in C++
        # artifacts are removed from this list below rather than borrow-checked.
        # reactor/ — (fiber_impl.cc, event.cc, quorum_event.cc, reactor.cc were
        # merged into rrr.reactor / reactor.cpp during modularization.)
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_wrapper.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/fiber.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/future.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/reactor.cpp
        # rpc/
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/callbacks.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/circuit_breaker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/client.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/completion_tracker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/connection_state.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/errors.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/fiber_channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/frame_codec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/heartbeat.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/idempotency.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/inmemory_channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/load_balancer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/pollable_proxy.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/reconnect_policy.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_options.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_queue.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/server.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/tcp_channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/utils.cpp
        # Notes on parse-recovery:
        # - rpc/server.cpp trips a libclang-22 crash on the first parse attempt
        #   (clang++ CLI compiles it fine — this is a libclang-specific bug).
        #   rusty-cpp's parser detects the crash
        #   and retries with module args dropped (keeps `import std;` only),
        #   producing a partial AST. Findings against this file are
        #   missing cross-module callee-name resolution, so they show up as
        #   `Calling non-safe function 'unknown'` and may include false
        #   positives. Once upstream libclang is fixed the recovery will
        #   become a no-op and findings will gain full precision.
        # Exclusions (each with a reason):
        # - reactor/fiber_context_{aarch64,x86_64}.cc: arch-specific inline
        #   `__asm__ volatile(...)` context-switch trampolines (~30 lines
        #   each). C++ semantics don't reach them.
    )
    list(REMOVE_ITEM RRR_BORROW_SRC
        ${SRPC_RETIRED_LEGACY_MODULE_FILES}
    )

    if(ENABLE_BORROW_CHECKING AND RRR_BORROW_SRC)
        add_library(rrr_borrow OBJECT ${RRR_BORROW_SRC})
        set_target_properties(rrr_borrow PROPERTIES EXCLUDE_FROM_ALL YES)
        target_include_directories(rrr_borrow PUBLIC
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src/rrr
            ${CMAKE_SOURCE_DIR}/third-party/rusty-cpp/include
        )
        target_compile_options(rrr_borrow PUBLIC ${RRR_CXXFLAGS})

        add_borrow_check_target(rrr_borrow)

        # The rrr target owns the FILE_SET CXX_MODULES and is what emits the
        # per-source `.o.modmap` response files (which list `-fmodule-file=`
        # for every dependent BMI) plus the BMIs themselves. If we run the
        # borrow checks before `rrr` has compiled, libclang sees neither —
        # parses proceed with "module 'std' not found" warnings and degraded
        # analysis. Force each individual check target to depend on the
        # `rrr` library target so ninja serializes them after BMI emission.
        # (Adding the dep only to `borrow_check_all_rrr_borrow` is not enough
        # — that's an aggregate, but ninja can still parallelize each per-file
        # check target without seeing the group-level dep.)
        foreach(_BORROW_SRC ${RRR_BORROW_SRC})
            if(_BORROW_SRC MATCHES "\\.(cpp|cc|cxx)$")
                get_filename_component(_BORROW_FILE_NAME ${_BORROW_SRC} NAME_WE)
                set(_BORROW_CHECK_NAME "borrow_check_rrr_borrow_${_BORROW_FILE_NAME}")
                if(TARGET ${_BORROW_CHECK_NAME})
                    add_dependencies(${_BORROW_CHECK_NAME} rrr)
                endif()
            endif()
        endforeach()

        add_custom_target(borrow_check_rrr
            DEPENDS borrow_check_all_rrr_borrow
            COMMENT "Running borrow checker on RRR library files"
        )
    elseif(ENABLE_BORROW_CHECKING)
        add_custom_target(borrow_check_rrr
            COMMAND ${CMAKE_COMMAND} -E echo "No RRR files configured for borrow checking"
            COMMENT "No RRR files configured for borrow checking"
        )
    endif()
endif()
