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
)

# Files that have been converted to C++23 named-module interface units.
# Each entry is a regular .cpp/.cc file but contains `export module
# rrr.<name>;` at the top. The matching .hpp/.h remains as a forward-
# declaration shim for non-importing consumers; the linker resolves
# their calls to the module-emitted .o.
set(RRR_MODULE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/base/basetypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/callback_wrapper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/debugging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/logging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/misc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/strop.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/threading.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/unittest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/alarm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/any_message.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/cpuinfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/dball.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/marshal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/netinfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/rand.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/serializable_envelope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/stat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_wrapper.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/fiber.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/future.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/reactor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/callbacks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/circuit_breaker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/completion_tracker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/connection_metrics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/connection_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/errors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/frame_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/heartbeat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/idempotency.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/inmemory_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/internal_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/load_balancer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/pollable_proxy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/reconnect_policy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_options.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_queue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/utils.cpp
)
list(REMOVE_ITEM RRR_BENCH_SRC ${RRR_MODULE_SRC})

set(RRR_CXXFLAGS ${BENCH_CXXFLAGS})

if(NOT TARGET rrr)
    add_library(rrr STATIC ${RRR_BENCH_SRC})
    if(RRR_MODULE_SRC)
        target_sources(rrr PUBLIC
            FILE_SET rrr_modules TYPE CXX_MODULES FILES ${RRR_MODULE_SRC}
        )
    endif()
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
    target_link_libraries(rrr pthread)

    # RRR files for borrow checking (explicit list to control what gets checked)
    set(RRR_BORROW_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/fiber_impl.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/event.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/quorum_event.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_wrapper.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/reactor/reactor.cc
        # base/logging.cpp omitted — module unit.
        # base/misc.cpp omitted — module unit.
        # base/basetypes.cpp omitted — module unit (see RRR_MODULE_SRC).
        # base/debugging.cpp omitted — module unit.
        ${CMAKE_CURRENT_SOURCE_DIR}/base/threading.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/alock.cpp
        # misc/any_message.cpp omitted — module unit.
        # misc/marshal.cpp omitted — module unit.
        # base/strop.cpp omitted — converted to a C++23 module interface
        # unit (see RRR_MODULE_SRC). rusty-cpp cannot resolve module
        # imports yet; revisit when the checker handles BMI paths.
        # base/unittest.cpp omitted — module unit.
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/rand.cpp
        # removed misc/recorder.cpp — class
        # deleted; was unused after Phase 4e-35.
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/utils.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/client.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc/server.cpp
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
