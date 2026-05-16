# Masstree static library.
#
# The seven .cc files below have no `mako/` includes — they only
# reference other masstree headers and the STL. We lift them into a
# standalone static library so:
#
#   * libmako can depend on it (PUBLIC) without owning the source
#     files, and a touch in src/deptran/ or src/mako/lib/ no longer
#     forces masstree sources to recompile;
#   * downstream consumers can in principle link `masstree` alone
#     for the small subset of Masstree functionality that doesn't
#     require mako's RCU wrapper (in practice the `mbtree<P>` cursor
#     wrapper in `mako/masstree_btree.h` still drags in mako/rcu.h,
#     so most callers will keep linking mako too — the win is build
#     incrementality, not link-size).
#
# The root CMakeLists keeps `add_borrow_check(src/masstree/...cc)`
# pointing at these source paths; borrow checking is per-file, not
# per-target, so it doesn't care which library owns them.
#
# Test target wiring lives in src/masstree/tests/CMakeLists.txt and
# is pulled in by the root CMakeLists inside `if(BUILD_TESTS)`.

set(MASSTREE_LIB_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/compiler.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/str.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/string.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/straccum.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/json.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/kvthread.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/masstree_context.cc"
)

add_library(masstree STATIC ${MASSTREE_LIB_SOURCES})
target_include_directories(masstree PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${MASSTREE_CONFIG_INCLUDE_DIR}
)
# Inherit the project's standard compile options plus the forced
# include of the generated masstree/config.h. The flag is set in the
# root CMakeLists; if it's empty here (older builds) the option list
# just becomes whatever was already added globally.
if(DEFINED MASSTREE_FORCE_INCLUDE_FLAG)
    target_compile_options(masstree PUBLIC ${MASSTREE_FORCE_INCLUDE_FLAG})
endif()
