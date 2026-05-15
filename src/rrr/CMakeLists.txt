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
    ${CMAKE_CURRENT_SOURCE_DIR}/base/debugging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/logging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/base/strop.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/base/misc.cpp
        # base/basetypes.cpp omitted — module unit (see RRR_MODULE_SRC).
        # base/debugging.cpp omitted — module unit.
        ${CMAKE_CURRENT_SOURCE_DIR}/base/threading.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/alock.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/misc/marshal.cpp
        # base/strop.cpp omitted — converted to a C++23 module interface
        # unit (see RRR_MODULE_SRC). rusty-cpp cannot resolve module
        # imports yet; revisit when the checker handles BMI paths.
        ${CMAKE_CURRENT_SOURCE_DIR}/base/unittest.cpp
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
