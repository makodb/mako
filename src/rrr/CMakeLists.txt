# rrr's C++20 named module interfaces and implementation partitions.
set(RRR_MODULE_INTERFACE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/base/debugging.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/strop.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/unittest.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/netinfo.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/rand.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/stat.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/fiber_impl.h
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/circuit_breaker.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/connection_metrics.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/connection_state.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/errors.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/heartbeat.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/channel.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/internal_protocol.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/frame_codec.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/reconnect_policy.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_options.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/utils.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/basetypes.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/dball.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/load_balancer.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/callbacks.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/misc.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/threading.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/logging.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/all.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/cpuinfo.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/marshal.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_wrapper.h
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/event.h
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/marshallable_proxy.h
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/recorder.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/idempotency.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/request_queue.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/pollable_proxy.h
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/quorum_event.h
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/completion_tracker.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/reactor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/alarm.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/future.h
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/client.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/server.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/alock.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/fiber.h
    ${CMAKE_CURRENT_SOURCE_DIR}/rrr.hpp
)

set(RRR_MODULE_IMPL_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/base/basetypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/debugging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/logging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/misc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/strop.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/threading.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/unittest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/alock.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/marshal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/rand.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/recorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_wrapper.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/event.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/fiber_context_runtime.cc
    # fiber_context_x86_64.cc is a plain TU (top-level asm).
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/fiber_impl.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/quorum_event.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor/reactor.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/frame_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/utils.cpp
)

# Export module file lists to parent CMakeLists.txt for txlog_core_obj wiring.
set(RRR_MODULE_INTERFACE_FILES ${RRR_MODULE_INTERFACE_FILES} PARENT_SCOPE)
set(RRR_MODULE_IMPL_FILES ${RRR_MODULE_IMPL_FILES} PARENT_SCOPE)

set_source_files_properties(${RRR_MODULE_INTERFACE_FILES} PROPERTIES
    LANGUAGE CXX
    CXX_SCAN_FOR_MODULES ON
)

file(GLOB RRR_BENCH_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/base/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/misc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/reactor/*.cc"
)

# Module implementation partitions are registered via FILE_SET on target `rrr`.
foreach(_RRR_MOD_IMPL IN LISTS RRR_MODULE_IMPL_FILES)
    list(REMOVE_ITEM RRR_BENCH_SRC "${_RRR_MOD_IMPL}")
endforeach()

# RRR module implementation units cannot be compiled with the global forced
# include (`-include .../masstree/config.h`) because it injects tokens before
# the module declaration.
set(RRR_CXXFLAGS ${BENCH_CXXFLAGS})
list(FIND RRR_CXXFLAGS "-include" RRR_INCLUDE_FLAG_INDEX)
if(NOT RRR_INCLUDE_FLAG_INDEX EQUAL -1)
    math(EXPR RRR_INCLUDE_PATH_INDEX "${RRR_INCLUDE_FLAG_INDEX}+1")
    list(REMOVE_AT RRR_CXXFLAGS ${RRR_INCLUDE_PATH_INDEX})
    list(REMOVE_AT RRR_CXXFLAGS ${RRR_INCLUDE_FLAG_INDEX})
endif()

if(NOT TARGET rrr)
    add_library(rrr STATIC ${RRR_BENCH_SRC})
    target_sources(rrr PUBLIC
        FILE_SET rrr_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_SOURCE_DIR}
        FILES ${RRR_MODULE_INTERFACE_FILES} ${RRR_MODULE_IMPL_FILES}
    )
    target_include_directories(rrr PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/rrr
        ${CMAKE_SOURCE_DIR}/third-party/rusty-cpp/include
        ${PROXY_INCLUDE_DIR}
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
        ${CMAKE_CURRENT_SOURCE_DIR}/base/logging.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/misc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/basetypes.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/debugging.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/threading.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/alock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/marshal.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/strop.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/base/unittest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/rand.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/recorder.cpp
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
            ${PROXY_INCLUDE_DIR}
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
