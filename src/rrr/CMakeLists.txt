# De-modularized rrr: plain STATIC library compiled from .cpp/.cc
# implementation units. Header-only files (former module interface
# units) are #included by the impl units and by external consumers.
file(GLOB RRR_BENCH_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/base/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/misc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/reactor/*.cc"
)

set(RRR_CXXFLAGS ${BENCH_CXXFLAGS})

if(NOT TARGET rrr)
    add_library(rrr STATIC ${RRR_BENCH_SRC})
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
        # Workstream N Phase 4e-36: removed misc/recorder.cpp — class
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
