# rrr: STATIC library compiled from .cpp/.cc implementation units.
# Files migrated to C++23 named modules keep their .cpp/.cc extension
# but are wired in via FILE_SET CXX_MODULES below (RRR_MODULE_SRC).
# They are explicitly removed from the regular source glob to avoid
# double-compile. Migration is incremental and measurement-gated — see
# docs/dev/srpc_module_migration_plan.md.
file(GLOB RRR_BENCH_SRC
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
set(RRR_INLINE_MODULE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/base/basetypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/debugging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/logging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/misc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/threading.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/any_message.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/cpuinfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable_envelope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_wrapper.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/fiber.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/future.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/reactor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/callbacks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/fiber_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/frame_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/idempotency.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/inmemory_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/load_balancer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/pollable_proxy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_queue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/tcp_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/utils.cpp
)
list(REMOVE_ITEM RRR_BENCH_SRC ${RRR_INLINE_MODULE_SRC})

# These twelve modules are owned by canonical Rust sources. rusty-cpp generates
# their complete C++ module interfaces; no hand-authored carrier remains.
set(RRR_GOAL0_CANONICAL_MODULES
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
)
set(_RRR_GOAL0_UNIQUE_MODULES ${RRR_GOAL0_CANONICAL_MODULES})
list(REMOVE_DUPLICATES _RRR_GOAL0_UNIQUE_MODULES)
list(LENGTH RRR_GOAL0_CANONICAL_MODULES _RRR_GOAL0_MODULE_COUNT)
list(LENGTH _RRR_GOAL0_UNIQUE_MODULES _RRR_GOAL0_UNIQUE_MODULE_COUNT)
if(NOT _RRR_GOAL0_MODULE_COUNT EQUAL 12 OR
   NOT _RRR_GOAL0_UNIQUE_MODULE_COUNT EQUAL 12)
    message(FATAL_ERROR
        "Goal-0 canonical module inventory must contain twelve unique names")
endif()

set(RRR_GOAL0_CRATE_CPP_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/goal0-crate-cpp
)
set(RRR_GOAL0_CANONICAL_RUST_SRC)
set(RRR_GOAL0_CRATE_CHILD_SRC)
foreach(_RRR_GOAL0_MODULE IN LISTS RRR_GOAL0_CANONICAL_MODULES)
    list(APPEND RRR_GOAL0_CANONICAL_RUST_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/${_RRR_GOAL0_MODULE}.rs)
    list(APPEND RRR_GOAL0_CRATE_CHILD_SRC
        ${RRR_GOAL0_CRATE_CPP_DIR}/rrr.${_RRR_GOAL0_MODULE}.cppm)
endforeach()

set(RRR_GOAL0_RETIRED_CARRIER_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/base/callback_wrapper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/internal_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/stat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/errors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/connection_metrics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/completion_tracker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/rand.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_options.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/reconnect_policy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/circuit_breaker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/connection_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/heartbeat.cpp
)
foreach(_RRR_GOAL0_CANONICAL_SOURCE IN LISTS RRR_GOAL0_CANONICAL_RUST_SRC)
    if(NOT EXISTS "${_RRR_GOAL0_CANONICAL_SOURCE}")
        message(FATAL_ERROR
            "Goal-0 canonical Rust source is missing: "
            "${_RRR_GOAL0_CANONICAL_SOURCE}")
    endif()
endforeach()
foreach(_RRR_GOAL0_RETIRED_CARRIER IN LISTS RRR_GOAL0_RETIRED_CARRIER_SRC)
    if(EXISTS "${_RRR_GOAL0_RETIRED_CARRIER}")
        message(FATAL_ERROR
            "Goal-0 retired inline carrier was reintroduced: "
            "${_RRR_GOAL0_RETIRED_CARRIER}")
    endif()
endforeach()
set(RRR_GOAL0_CRATE_ROOT_SRC
    ${RRR_GOAL0_CRATE_CPP_DIR}/rrr.cppm
)
list(LENGTH RRR_GOAL0_CANONICAL_RUST_SRC _RRR_GOAL0_CANONICAL_COUNT)
list(LENGTH RRR_GOAL0_CRATE_CHILD_SRC _RRR_GOAL0_CHILD_COUNT)
set(_RRR_GOAL0_UNIQUE_CANONICAL_RUST_SRC ${RRR_GOAL0_CANONICAL_RUST_SRC})
set(_RRR_GOAL0_UNIQUE_CRATE_CHILD_SRC ${RRR_GOAL0_CRATE_CHILD_SRC})
list(REMOVE_DUPLICATES _RRR_GOAL0_UNIQUE_CANONICAL_RUST_SRC)
list(REMOVE_DUPLICATES _RRR_GOAL0_UNIQUE_CRATE_CHILD_SRC)
list(LENGTH _RRR_GOAL0_UNIQUE_CANONICAL_RUST_SRC
    _RRR_GOAL0_UNIQUE_CANONICAL_COUNT)
list(LENGTH _RRR_GOAL0_UNIQUE_CRATE_CHILD_SRC _RRR_GOAL0_UNIQUE_CHILD_COUNT)
if(NOT _RRR_GOAL0_CANONICAL_COUNT EQUAL 12 OR
   NOT _RRR_GOAL0_CHILD_COUNT EQUAL 12 OR
   NOT _RRR_GOAL0_UNIQUE_CANONICAL_COUNT EQUAL 12 OR
   NOT _RRR_GOAL0_UNIQUE_CHILD_COUNT EQUAL 12)
    message(FATAL_ERROR
        "Goal-0 crate production must own exactly twelve unique "
        "Rust/generated providers")
endif()

# Production always combines the remaining inline carriers with the twelve
# generated providers. The partial crate root remains compile-only until all
# 38 named modules are canonical Rust.
set(RRR_MODULE_SRC ${RRR_INLINE_MODULE_SRC} ${RRR_GOAL0_CRATE_CHILD_SRC})
set(RRR_MODULE_BASE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RRR_GOAL0_CRATE_CPP_DIR}
)
set_source_files_properties(${RRR_GOAL0_CRATE_CHILD_SRC}
    PROPERTIES GENERATED TRUE)

list(LENGTH RRR_INLINE_MODULE_SRC _RRR_INLINE_MODULE_COUNT)
list(LENGTH RRR_MODULE_SRC _RRR_PRODUCTION_MODULE_COUNT)
if(NOT _RRR_INLINE_MODULE_COUNT EQUAL 26 OR
   NOT _RRR_PRODUCTION_MODULE_COUNT EQUAL 38)
    message(FATAL_ERROR
        "Goal-0 production must contain 26 inline and twelve generated modules")
endif()
list(FIND RRR_MODULE_SRC "${RRR_GOAL0_CRATE_ROOT_SRC}" _RRR_GOAL0_ROOT_INDEX)
if(NOT _RRR_GOAL0_ROOT_INDEX EQUAL -1)
    message(FATAL_ERROR
        "Partial Goal-0 crate root must not be a production module provider")
endif()
foreach(_RRR_GOAL0_CHILD IN LISTS RRR_GOAL0_CRATE_CHILD_SRC)
    list(FIND RRR_MODULE_SRC "${_RRR_GOAL0_CHILD}" _RRR_GOAL0_CHILD_INDEX)
    if(_RRR_GOAL0_CHILD_INDEX EQUAL -1)
        message(FATAL_ERROR
            "Goal-0 production is missing generated provider: ${_RRR_GOAL0_CHILD}")
    endif()
endforeach()
# Platform implementation units for rrr.epoll_wrapper (Rust std's
# sys-module pattern): exactly one is compiled, selected below.
list(REMOVE_ITEM RRR_BENCH_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_platform_linux.cc)

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

# Rand and circuit breaker keep only thin calls into the plain-C seed and
# monotonic-clock kernels. The generated-object and production importer lanes
# supply deterministic definitions for exact draw/destroy and clock behavior.
# This separate executable pulls the real C archive and smoke-tests both seams
# without introducing another generated provider into either comparison lane.
add_library(rrr_goal0_rand_c_kernel STATIC EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/srpc_rand.c
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/srpc_timing.c
)
target_include_directories(rrr_goal0_rand_c_kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/misc
)
target_link_libraries(rrr_goal0_rand_c_kernel PUBLIC pthread)
add_executable(rrr_goal0_rand_kernel_smoke EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/rand_kernel_smoke.c
)
target_include_directories(rrr_goal0_rand_kernel_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(rrr_goal0_rand_kernel_smoke PRIVATE
    rrr_goal0_rand_c_kernel
)

if(NOT TARGET rrr)
    add_library(rrr STATIC ${RRR_BENCH_SRC})
    if(RRR_MODULE_SRC)
        target_sources(rrr PUBLIC
            FILE_SET rrr_modules TYPE CXX_MODULES
            BASE_DIRS ${RRR_MODULE_BASE_DIRS}
            FILES ${RRR_MODULE_SRC}
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
    target_sources(rrr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_platform_linux.cc)

    # Remaining hand-authored RRR C++ module units for borrow checking. The
    # twelve canonical Rust modules are checked by rustc and are not duplicated
    # here as generated C++ providers.
    set(RRR_BORROW_SRC
        # base/
        ${CMAKE_CURRENT_SOURCE_DIR}/base/basetypes.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/debugging.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/logging.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/misc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/threading.cpp
            # misc/
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/any_message.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/cpuinfo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable_envelope.cpp
        # reactor/ — (fiber_impl.cc, event.cc, quorum_event.cc, reactor.cc were
        # merged into rrr.reactor / reactor.cpp during modularization.)
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_wrapper.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/fiber.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/future.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/reactor.cpp
        # rpc/
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/callbacks.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/client.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/fiber_channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/frame_codec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/idempotency.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/inmemory_channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/load_balancer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/pollable_proxy.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_queue.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/server.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/tcp_channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/utils.cpp
        # Notes on parse-recovery:
        # - rpc/server.cpp and rpc/fiber_channel.cpp trip a libclang-22 crash
        #   on the first parse attempt (clang++ CLI compiles them fine — it's
        #   a libclang-specific bug). rusty-cpp's parser detects the crash
        #   and retries with module args dropped (keeps `import std;` only),
        #   producing a partial AST. Findings against these two files are
        #   missing cross-module callee-name resolution, so they show up as
        #   `Calling non-safe function 'unknown'` and may include false
        #   positives. Once upstream libclang is fixed the recovery will
        #   become a no-op and findings will gain full precision.
        # Exclusions (each with a reason):
        # - reactor/fiber_context_{aarch64,x86_64}.cc: arch-specific inline
        #   `__asm__ volatile(...)` context-switch trampolines (~30 lines
        #   each). C++ semantics don't reach them.
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

# Goal 0 is guarded at both source and produced-archive boundaries. These are
# custom targets (rather than configure-time checks) so every production build
# observes changes made after CMake configuration.
if(NOT TARGET build_rusty_cpp_transpiler)
    message(FATAL_ERROR "rrr Goal-0 gates require build_rusty_cpp_transpiler")
endif()
find_program(RRR_BASH_EXECUTABLE NAMES bash REQUIRED)
find_program(RRR_CARGO_EXECUTABLE NAMES cargo REQUIRED)

add_custom_target(rrr_goal0_source_gate
    COMMAND
        "${RRR_BASH_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/rrr_dsl_check.sh"
        "${RUSTYCPP_TRANSPILER}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/extract_rrr_rust.py"
        --check
        --transpiler "${RUSTYCPP_TRANSPILER}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/tests/test_extract_rrr_rust.py"
    COMMAND
        "${CMAKE_COMMAND}"
        -E env
        "RUSTFLAGS=-Dwarnings"
        "${RRR_CARGO_EXECUTABLE}"
        test
        --locked
        --workspace
        --all-targets
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    COMMAND
        "${RRR_CARGO_EXECUTABLE}"
        clippy
        --locked
        --workspace
        --all-targets
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
        --
        -D warnings
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Checking remaining inline Rust and canonical rustc crate sources"
    USES_TERMINAL
    VERBATIM
)
add_dependencies(rrr_goal0_source_gate build_rusty_cpp_transpiler)
add_dependencies(rrr rrr_goal0_source_gate)

# Cargo owns the transpiler executable, but build_rusty_cpp_transpiler is an
# always-run custom target and does not declare that executable as a CMake
# output. Depending on the target alone is only an order edge; depending on the
# executable directly would let Ninja scan its old timestamp before Cargo
# replaces it in the same build. Instead, fingerprint the executable after the
# Cargo edge and expose that fingerprint as a declared byproduct. The crate
# generation rule consumes the fingerprint as a normal file dependency.
set(RRR_GOAL0_TRANSPILER_FINGERPRINT
    "${CMAKE_CURRENT_BINARY_DIR}/goal0-transpiler.fingerprint"
)
add_custom_target(rrr_goal0_transpiler_fingerprint
    COMMAND
        "${CMAKE_COMMAND}"
        "-DINPUT=${RUSTYCPP_TRANSPILER}"
        "-DOUTPUT=${RRR_GOAL0_TRANSPILER_FINGERPRINT}"
        -P "${CMAKE_SOURCE_DIR}/scripts/update_file_fingerprint.cmake"
    BYPRODUCTS "${RRR_GOAL0_TRANSPILER_FINGERPRINT}"
    COMMENT "Fingerprinting the Goal-0 rusty-cpp emitter"
    VERBATIM
)
add_dependencies(
    rrr_goal0_transpiler_fingerprint
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
# Only the twelve child .cppm files enter RRR_MODULE_SRC. The dual gate compiles
# rrr.cppm after its children in temporary storage, but production never does.
set(RRR_GOAL0_CRATE_GENERATED_OUTPUTS
    ${RRR_GOAL0_CRATE_CHILD_SRC}
    ${RRR_GOAL0_CRATE_ROOT_SRC}
    ${RRR_GOAL0_CRATE_CPP_DIR}/CMakeLists.txt
    ${RRR_GOAL0_CRATE_CPP_DIR}/rusty_hand_slots.md
)
set(RRR_GOAL0_MODULE_PREAMBLE
    ${CMAKE_CURRENT_SOURCE_DIR}/module-preambles.toml
)
# Keep the crate manifest absolute at the process boundary.  The bcc8 crate
# dependency validator deliberately resolves the exact local `rusty` facade;
# an absolute entry manifest makes that walk independent of WORKING_DIRECTORY.
get_filename_component(
    RRR_GOAL0_CRATE_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    ABSOLUTE
)
add_custom_command(
    OUTPUT ${RRR_GOAL0_CRATE_GENERATED_OUTPUTS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RRR_GOAL0_CRATE_CPP_DIR}"
    COMMAND
        "${RUSTYCPP_TRANSPILER}"
        --crate "${RRR_GOAL0_CRATE_MANIFEST}"
        --output-dir "${RRR_GOAL0_CRATE_CPP_DIR}"
        --cxx-namespace rrr
        --module-preamble "${RRR_GOAL0_MODULE_PREAMBLE}"
    DEPENDS
        rrr_goal0_source_gate
        rrr_goal0_transpiler_fingerprint
        "${RRR_GOAL0_TRANSPILER_FINGERPRINT}"
        "${RRR_GOAL0_CRATE_MANIFEST}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
        "${CMAKE_CURRENT_SOURCE_DIR}/rusty-rustc/Cargo.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/rusty-rustc/src/lib.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/rust-modules.toml"
        "${RRR_GOAL0_MODULE_PREAMBLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs"
        ${RRR_GOAL0_CANONICAL_RUST_SRC}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Generating Goal-0 rrr crate C++ child modules"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(rrr_goal0_crate_codegen
    DEPENDS ${RRR_GOAL0_CRATE_GENERATED_OUTPUTS}
)
add_dependencies(rrr rrr_goal0_crate_codegen)

# The Python gate links raw archives rather than a CMake target, so carry the
# complete static runtime closure explicitly. Each archive is placed in a
# linker rescan group by the gate; `$<TARGET_FILE:rusty>` alone would discard
# CMake's PUBLIC dependency graph and leave imported-module initializers
# unresolved.
set(RRR_GOAL0_RUNTIME_TARGETS
    rusty
    rusty_async
    vec_port
    btree_port
    rc_port
    hashbrown_port
    binary_heap_port
    vec_deque_port
    cell_port
    string_port
    arc_port
    linked_list_port
)
set(RRR_GOAL0_RUNTIME_LIBRARY_ARGS)
foreach(_RRR_GOAL0_RUNTIME_TARGET IN LISTS RRR_GOAL0_RUNTIME_TARGETS)
    if(NOT TARGET ${_RRR_GOAL0_RUNTIME_TARGET})
        message(FATAL_ERROR
            "Goal-0 crate gate runtime target is unavailable: "
            "${_RRR_GOAL0_RUNTIME_TARGET}")
    endif()
    list(APPEND RRR_GOAL0_RUNTIME_LIBRARY_ARGS
        --runtime-library "$<TARGET_FILE:${_RRR_GOAL0_RUNTIME_TARGET}>")
endforeach()

# Match the configured Clang/libc++ ABI in the gate's direct compile and link
# commands. Also preserve the selected Homebrew libc++ search path/rpath when
# the toolchain setup above supplied one.
set(RRR_GOAL0_DIRECT_TOOLCHAIN_ARGS)
# The configured `rusty` BMI is compiled with `-march=native` above.  Clang
# requires every importer of that BMI to use the same target-feature set, so
# carry the architecture selection into the gate's direct module commands.
list(APPEND RRR_GOAL0_DIRECT_TOOLCHAIN_ARGS
    --cxx-flag=-march=native
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND RRR_GOAL0_DIRECT_TOOLCHAIN_ARGS
        --cxx-flag=-stdlib=libc++
        --link-flag=-lc++abi
    )
    separate_arguments(_RRR_GOAL0_BREW_LINK_FLAGS
        NATIVE_COMMAND "${MAKO_BREW_LIBCXX_FLAGS}")
    foreach(_RRR_GOAL0_LINK_FLAG IN LISTS _RRR_GOAL0_BREW_LINK_FLAGS)
        list(APPEND RRR_GOAL0_DIRECT_TOOLCHAIN_ARGS
            "--link-flag=${_RRR_GOAL0_LINK_FLAG}")
    endforeach()
endif()

add_custom_target(rrr_goal0_dual_compile ALL
    COMMAND "$<TARGET_FILE:rrr_goal0_rand_kernel_smoke>"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/check_rrr_crate_mode.py"
        --transpiler "${RUSTYCPP_TRANSPILER}"
        --generated-dir "${RRR_GOAL0_CRATE_CPP_DIR}"
        --production-library "$<TARGET_FILE:rrr>"
        --runtime-module-root
            "${CMAKE_BINARY_DIR}/third-party/rusty-cpp/CMakeFiles"
        ${RRR_GOAL0_RUNTIME_LIBRARY_ARGS}
        ${RRR_GOAL0_DIRECT_TOOLCHAIN_ARGS}
        --clang "${CMAKE_CXX_COMPILER}"
        --nm "${CMAKE_NM}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Checking whole rrr crate through rustc and rusty-cpp"
    USES_TERMINAL
    VERBATIM
)
add_dependencies(rrr_goal0_dual_compile
    rrr
    rrr_goal0_crate_codegen
    rrr_goal0_rand_kernel_smoke
    ${RRR_GOAL0_RUNTIME_TARGETS}
)
