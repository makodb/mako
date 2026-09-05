# Mako's build integration for the vendored srpc tree at `src/srpc`.
#
# This file deliberately lives OUTSIDE `src/srpc`. That directory is a verbatim
# copy of the standalone srpc repository (https://github.com/stonysystems/srpc),
# which ships its own top-level `CMakeLists.txt` describing the standalone
# project. Keeping mako's glue here means `src/srpc` holds only srpc's content,
# so the tree can be resynced -- or swapped for a submodule -- without a merge.
#
# The root `CMakeLists.txt` adds this directory with an explicit binary
# directory (`add_subdirectory(src/srpc-cmake src/srpc)`), so every generated
# artifact still lands under `<build>/src/srpc/...` exactly as before; only the
# *source* directory moved. Because of that, `CMAKE_CURRENT_SOURCE_DIR` no
# longer names the srpc sources: use `SRPC_SOURCE_DIR` for anything that lives in
# the vendored tree, and `CMAKE_CURRENT_BINARY_DIR` (unchanged) for build
# outputs.
set(SRPC_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/srpc")

# srpc: STATIC library compiled from .cpp/.cc implementation units.
# Files migrated to C++23 named modules keep their .cpp/.cc extension
# but are wired in via FILE_SET CXX_MODULES below (SRPC_MODULE_SRC).
# They are explicitly removed from the regular source glob to avoid
# double-compile. Migration is incremental and measurement-gated — see
# docs/dev/srpc_module_migration_plan.md.
file(GLOB SRPC_BENCH_SRC
    "${SRPC_SOURCE_DIR}/base/*.cpp"
    "${SRPC_SOURCE_DIR}/misc/*.cpp"
    "${SRPC_SOURCE_DIR}/rpc/*.cpp"
    "${SRPC_SOURCE_DIR}/reactor/*.cc"
    # fiber context-switch trampolines, real assembly (see top-level
    # CMakeLists enable_language(ASM))
    "${SRPC_SOURCE_DIR}/reactor/*.S"
)

# Remaining C++23 named-module interface units whose Rust is still embedded in
# an inline carrier. Keep this census complete so none is also compiled through
# the regular source glob.
# Goal 0 complete: NO hand-authored module interface unit remains under
# src/srpc. Every one of the thirty-seven named modules is generated from a
# canonical Rust source by rusty-cpp crate mode, so this census is empty and
# nothing has to be removed from the regular source glob.
set(SRPC_INLINE_MODULE_SRC)

# These thirty-seven modules are owned by canonical Rust sources. rusty-cpp generates
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
set(_SRPC_GOAL0_UNIQUE_MODULES ${SRPC_GOAL0_CANONICAL_MODULES})
list(REMOVE_DUPLICATES _SRPC_GOAL0_UNIQUE_MODULES)
list(LENGTH SRPC_GOAL0_CANONICAL_MODULES _SRPC_GOAL0_MODULE_COUNT)
list(LENGTH _SRPC_GOAL0_UNIQUE_MODULES _SRPC_GOAL0_UNIQUE_MODULE_COUNT)
if(NOT _SRPC_GOAL0_MODULE_COUNT EQUAL 37 OR
   NOT _SRPC_GOAL0_UNIQUE_MODULE_COUNT EQUAL 37)
    message(FATAL_ERROR
        "Goal-0 canonical module inventory must contain thirty-seven unique names")
endif()

set(SRPC_GOAL0_CRATE_CPP_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/goal0-crate-cpp
)
# Canonical Rust lives beside the C++ it replaced -- base/, misc/, rpc/,
# reactor/ -- so `src/srpc` mirrors srpc's tree exactly and a future sync is a
# copy rather than a merge. The manifest owns the mapping; this list is its
# CMake mirror and the census check below keeps the two in step.
set(SRPC_GOAL0_CANONICAL_SOURCE_RELPATH
    base/basetypes.rs
    base/callback_wrapper.rs
    rpc/internal_protocol.rs
    misc/stat.rs
    rpc/errors.rs
    rpc/connection_metrics.rs
    rpc/completion_tracker.rs
    misc/rand.rs
    rpc/request_options.rs
    rpc/reconnect_policy.rs
    rpc/circuit_breaker.rs
    rpc/connection_state.rs
    rpc/heartbeat.rs
    rpc/request_queue.rs
    rpc/load_balancer.rs
    rpc/utils.rs
    rpc/frame_codec.rs
    misc/serializable.rs
    misc/serializable_envelope.rs
    reactor/future.rs
    base/logging.rs
    rpc/idempotency.rs
    reactor/fiber.rs
    base/misc.rs
    rpc/channel.rs
    reactor/epoll_wrapper.rs
    rpc/pollable_proxy.rs
    rpc/callbacks.rs
    rpc/inmemory_channel.rs
    rpc/fiber_channel.rs
    base/threading.rs
    base/debugging.rs
    misc/any_message.rs
    rpc/tcp_channel.rs
    reactor/reactor.rs
    rpc/server.rs
    rpc/client.rs
)
set(SRPC_GOAL0_CANONICAL_RUST_SRC)
set(SRPC_GOAL0_CRATE_CHILD_SRC)
foreach(_SRPC_GOAL0_MODULE IN LISTS SRPC_GOAL0_CANONICAL_MODULES)
    list(APPEND SRPC_GOAL0_CRATE_CHILD_SRC
        ${SRPC_GOAL0_CRATE_CPP_DIR}/srpc.${_SRPC_GOAL0_MODULE}.cppm)
endforeach()
foreach(_SRPC_GOAL0_RELPATH IN LISTS SRPC_GOAL0_CANONICAL_SOURCE_RELPATH)
    list(APPEND SRPC_GOAL0_CANONICAL_RUST_SRC
        ${SRPC_SOURCE_DIR}/${_SRPC_GOAL0_RELPATH})
endforeach()

set(SRPC_GOAL0_RETIRED_CARRIER_SRC
    ${SRPC_SOURCE_DIR}/base/basetypes.cpp
    ${SRPC_SOURCE_DIR}/base/callback_wrapper.cpp
    ${SRPC_SOURCE_DIR}/rpc/internal_protocol.cpp
    ${SRPC_SOURCE_DIR}/misc/stat.cpp
    ${SRPC_SOURCE_DIR}/rpc/errors.cpp
    ${SRPC_SOURCE_DIR}/rpc/connection_metrics.cpp
    ${SRPC_SOURCE_DIR}/rpc/completion_tracker.cpp
    ${SRPC_SOURCE_DIR}/misc/rand.cpp
    ${SRPC_SOURCE_DIR}/rpc/request_options.cpp
    ${SRPC_SOURCE_DIR}/rpc/reconnect_policy.cpp
    ${SRPC_SOURCE_DIR}/rpc/circuit_breaker.cpp
    ${SRPC_SOURCE_DIR}/rpc/connection_state.cpp
    ${SRPC_SOURCE_DIR}/rpc/heartbeat.cpp
    ${SRPC_SOURCE_DIR}/rpc/request_queue.cpp
    ${SRPC_SOURCE_DIR}/rpc/load_balancer.cpp
    ${SRPC_SOURCE_DIR}/rpc/utils.cpp
    ${SRPC_SOURCE_DIR}/rpc/frame_codec.cpp
    ${SRPC_SOURCE_DIR}/base/debugging.cpp
    ${SRPC_SOURCE_DIR}/base/logging.cpp
    ${SRPC_SOURCE_DIR}/base/misc.cpp
    ${SRPC_SOURCE_DIR}/base/threading.cpp
    ${SRPC_SOURCE_DIR}/misc/any_message.cpp
    ${SRPC_SOURCE_DIR}/misc/serializable.cpp
    ${SRPC_SOURCE_DIR}/misc/serializable_envelope.cpp
    ${SRPC_SOURCE_DIR}/reactor/epoll_wrapper.cc
    ${SRPC_SOURCE_DIR}/reactor/fiber.cpp
    ${SRPC_SOURCE_DIR}/reactor/future.cpp
    ${SRPC_SOURCE_DIR}/reactor/reactor.cpp
    ${SRPC_SOURCE_DIR}/rpc/callbacks.cpp
    ${SRPC_SOURCE_DIR}/rpc/channel.cpp
    ${SRPC_SOURCE_DIR}/rpc/client.cpp
    ${SRPC_SOURCE_DIR}/rpc/fiber_channel.cpp
    ${SRPC_SOURCE_DIR}/rpc/idempotency.cpp
    ${SRPC_SOURCE_DIR}/rpc/inmemory_channel.cpp
    ${SRPC_SOURCE_DIR}/rpc/pollable_proxy.cpp
    ${SRPC_SOURCE_DIR}/rpc/server.cpp
    ${SRPC_SOURCE_DIR}/rpc/tcp_channel.cpp
)
foreach(_SRPC_GOAL0_CANONICAL_SOURCE IN LISTS SRPC_GOAL0_CANONICAL_RUST_SRC)
    if(NOT EXISTS "${_SRPC_GOAL0_CANONICAL_SOURCE}")
        message(FATAL_ERROR
            "Goal-0 canonical Rust source is missing: "
            "${_SRPC_GOAL0_CANONICAL_SOURCE}")
    endif()
endforeach()
foreach(_SRPC_GOAL0_RETIRED_CARRIER IN LISTS SRPC_GOAL0_RETIRED_CARRIER_SRC)
    if(EXISTS "${_SRPC_GOAL0_RETIRED_CARRIER}")
        message(FATAL_ERROR
            "Goal-0 retired inline carrier was reintroduced: "
            "${_SRPC_GOAL0_RETIRED_CARRIER}")
    endif()
endforeach()
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
if(NOT _SRPC_GOAL0_CANONICAL_COUNT EQUAL 37 OR
   NOT _SRPC_GOAL0_CHILD_COUNT EQUAL 37 OR
   NOT _SRPC_GOAL0_UNIQUE_CANONICAL_COUNT EQUAL 37 OR
   NOT _SRPC_GOAL0_UNIQUE_CHILD_COUNT EQUAL 37)
    message(FATAL_ERROR
        "Goal-0 crate production must own exactly thirty-seven unique "
        "Rust/generated providers")
endif()

# Production is now exactly the thirty-seven generated providers; the inline
# census is empty. The crate root stays compile-only: it is the crate's
# umbrella, not a production module provider.
set(SRPC_MODULE_SRC ${SRPC_INLINE_MODULE_SRC} ${SRPC_GOAL0_CRATE_CHILD_SRC})
set(SRPC_MODULE_BASE_DIRS
    ${SRPC_SOURCE_DIR}
    ${SRPC_GOAL0_CRATE_CPP_DIR}
)
set_source_files_properties(${SRPC_GOAL0_CRATE_CHILD_SRC}
    PROPERTIES GENERATED TRUE)

list(LENGTH SRPC_INLINE_MODULE_SRC _SRPC_INLINE_MODULE_COUNT)
list(LENGTH SRPC_MODULE_SRC _SRPC_PRODUCTION_MODULE_COUNT)
if(NOT _SRPC_INLINE_MODULE_COUNT EQUAL 0 OR
   NOT _SRPC_PRODUCTION_MODULE_COUNT EQUAL 37)
    message(FATAL_ERROR
        "Goal-0 production must contain 0 inline and thirty-seven generated modules")
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
    ${SRPC_SOURCE_DIR}/reactor/epoll_platform_linux.cc)

set(SRPC_CXXFLAGS ${BENCH_CXXFLAGS})

# The `rusty` umbrella module — defined here (not upstream in rusty-cpp)
# because rusty-cpp ships rusty.cppm only as a header on the include
# path, leaving consumers responsible for compiling it as a named
# C++20 module unit. We re-export the transpiled port libraries
# (vec_port, btree_port, rc_port, std_port, rusty_async) so that
# `import rusty;` in srpc sources resolves rusty::Vec / Rc / BTreeMap
# / HashMap / etc. via the umbrella's `using` aliases. (HashMap/HashSet
# moved from the deleted hashbrown_port to std_port — rusty-cpp #177.)
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
    ${SRPC_SOURCE_DIR}/misc/srpc_rand.c
    ${SRPC_SOURCE_DIR}/misc/srpc_timing.c
)
target_include_directories(srpc_goal0_rand_c_kernel PRIVATE
    ${SRPC_SOURCE_DIR}/misc
)
target_link_libraries(srpc_goal0_rand_c_kernel PUBLIC pthread)
add_executable(srpc_goal0_rand_kernel_smoke EXCLUDE_FROM_ALL
    ${SRPC_SOURCE_DIR}/tests/rand_kernel_smoke.c
)
target_include_directories(srpc_goal0_rand_kernel_smoke PRIVATE
    ${SRPC_SOURCE_DIR}
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
        ${SRPC_SOURCE_DIR}/rpc/srpc_connect.c
        ${SRPC_SOURCE_DIR}/rpc/srpc_server.c
        ${SRPC_SOURCE_DIR}/rpc/srpc_net.c
        ${SRPC_SOURCE_DIR}/misc/srpc_timing.c
        ${SRPC_SOURCE_DIR}/misc/srpc_rand.c
        ${SRPC_SOURCE_DIR}/misc/srpc_io.c
        ${SRPC_SOURCE_DIR}/base/srpc_base.c
        ${SRPC_SOURCE_DIR}/reactor/srpc_fiber.c
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
    target_include_directories(srpc BEFORE PUBLIC
        ${CMAKE_SOURCE_DIR}/src/compat
    )
    target_include_directories(srpc PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/srpc
        ${CMAKE_SOURCE_DIR}/third-party/rusty-cpp/include
    )
    target_compile_options(srpc PUBLIC ${SRPC_CXXFLAGS})
    target_link_libraries(srpc pthread rusty)
    target_sources(srpc PRIVATE ${SRPC_SOURCE_DIR}/reactor/epoll_platform_linux.cc)

    # Remaining hand-authored SRPC C++ module units for borrow checking. The
    # eighteen canonical Rust modules are checked by rustc and are not duplicated
    # here as generated C++ providers.
    # Goal 0 complete: every src/srpc module is generated from canonical Rust
    # and checked by rustc, so no hand-authored C++ module unit remains to
    # borrow-check. The arch-specific fiber context-switch trampolines
    # (reactor/fiber_context_{aarch64,x86_64}.cc) stay excluded: they are
    # inline `__asm__ volatile(...)` that C++ semantics do not reach.
    set(SRPC_BORROW_SRC)

    if(ENABLE_BORROW_CHECKING AND SRPC_BORROW_SRC)
        add_library(srpc_borrow OBJECT ${SRPC_BORROW_SRC})
        set_target_properties(srpc_borrow PROPERTIES EXCLUDE_FROM_ALL YES)
        target_include_directories(srpc_borrow PUBLIC
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src/srpc
            ${CMAKE_SOURCE_DIR}/third-party/rusty-cpp/include
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
        "${CMAKE_SOURCE_DIR}/scripts/srpc_dsl_check.sh"
        "${RUSTYCPP_TRANSPILER}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/extract_srpc_rust.py"
        --check
        --transpiler "${RUSTYCPP_TRANSPILER}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/tests/test_extract_srpc_rust.py"
    COMMAND
        "${CMAKE_COMMAND}"
        -E env
        "RUSTFLAGS=-Dwarnings"
        "${SRPC_CARGO_EXECUTABLE}"
        test
        --locked
        --workspace
        --all-targets
        --manifest-path "${SRPC_SOURCE_DIR}/Cargo.toml"
    COMMAND
        "${SRPC_CARGO_EXECUTABLE}"
        clippy
        --locked
        --workspace
        --all-targets
        --manifest-path "${SRPC_SOURCE_DIR}/Cargo.toml"
        --
        -D warnings
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
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
        -P "${CMAKE_SOURCE_DIR}/scripts/update_file_fingerprint.cmake"
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
# Only the seventeen child .cppm files enter SRPC_MODULE_SRC. The dual gate compiles
# srpc.cppm after its children in temporary storage, but production never does.
set(SRPC_GOAL0_CRATE_GENERATED_OUTPUTS
    ${SRPC_GOAL0_CRATE_CHILD_SRC}
    ${SRPC_GOAL0_CRATE_ROOT_SRC}
    ${SRPC_GOAL0_CRATE_CPP_DIR}/CMakeLists.txt
    ${SRPC_GOAL0_CRATE_CPP_DIR}/rusty_hand_slots.md
)
set(SRPC_GOAL0_MODULE_PREAMBLE
    ${SRPC_SOURCE_DIR}/module-preambles.toml
)
set(SRPC_GOAL0_TYPE_MAP
    ${SRPC_SOURCE_DIR}/rust-type-map.toml
)
set(SRPC_GOAL0_CPP_MODULE_INDEX
    ${SRPC_SOURCE_DIR}/cpp-module-index.toml
)
# Keep the crate manifest absolute at the process boundary. The crate
# dependency validator deliberately resolves the exact local `rusty` facade;
# an absolute entry manifest makes that walk independent of WORKING_DIRECTORY.
get_filename_component(
    SRPC_GOAL0_CRATE_MANIFEST
    "${SRPC_SOURCE_DIR}/Cargo.toml"
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
        "${SRPC_SOURCE_DIR}/Cargo.lock"
        "${SRPC_SOURCE_DIR}/rusty-rustc/Cargo.toml"
        "${SRPC_SOURCE_DIR}/rusty-rustc/src/lib.rs"
        "${SRPC_SOURCE_DIR}/rust-modules.toml"
        "${SRPC_GOAL0_MODULE_PREAMBLE}"
        "${SRPC_GOAL0_TYPE_MAP}"
        "${SRPC_GOAL0_CPP_MODULE_INDEX}"
        "${SRPC_SOURCE_DIR}/src/lib.rs"
        ${SRPC_GOAL0_CANONICAL_RUST_SRC}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
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
    # hashbrown_port was deleted upstream (rusty-cpp #177). std_port and
    # std_port_hashbrown are two SEPARATE C++20 modules / archives, so both
    # have to be named here: the gate links raw archives and does not walk
    # std_port's PUBLIC dependency on std_port_hashbrown.
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
        NATIVE_COMMAND "${MAKO_BREW_LIBCXX_FLAGS}")
    foreach(_SRPC_GOAL0_LINK_FLAG IN LISTS _SRPC_GOAL0_BREW_LINK_FLAGS)
        list(APPEND SRPC_GOAL0_DIRECT_TOOLCHAIN_ARGS
            "--link-flag=${_SRPC_GOAL0_LINK_FLAG}")
    endforeach()
endif()

add_custom_target(srpc_goal0_dual_compile ALL
    COMMAND "$<TARGET_FILE:srpc_goal0_rand_kernel_smoke>"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/check_srpc_crate_mode.py"
        --transpiler "${RUSTYCPP_TRANSPILER}"
        --generated-dir "${SRPC_GOAL0_CRATE_CPP_DIR}"
        --production-library "$<TARGET_FILE:srpc>"
        --runtime-module-root
            "${CMAKE_BINARY_DIR}/third-party/rusty-cpp/CMakeFiles"
        # Canonical srpc.logging / srpc.serializable / srpc.reactor / srpc.future
        # `import std;`, so the gate's standalone module compiles need CMake's
        # own std BMI, not just the rusty runtime ones.
        --runtime-module-root
            "${CMAKE_BINARY_DIR}/CMakeFiles/__cmake_cxx_std_23.dir"
        ${SRPC_GOAL0_RUNTIME_LIBRARY_ARGS}
        ${SRPC_GOAL0_DIRECT_TOOLCHAIN_ARGS}
        --clang "${CMAKE_CXX_COMPILER}"
        --nm "${CMAKE_NM}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
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
