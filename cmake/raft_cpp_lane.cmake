# Transpiled C++ lane of the Raft core (MAKO_RAFT_LANE=cpp, plan phases L2-L4).
#
# The core crate at src/deptran/raft is staged (scripts/raft_cpp_stage.py
# resolves its `raft_test` gates, which crate mode cannot) and transpiled in
# crate mode; the resulting C++20 modules compile into raft_cpp_core. Nothing
# generated is checked in. The output is keyed on RAFT_TEST because the lab
# modules exist only under the raft_test feature, exactly as for cargo.
set(RAFT_CPP_CRATE_DIR "${CMAKE_SOURCE_DIR}/src/deptran/raft")
if(RAFT_TEST)
    set(RAFT_CPP_VARIANT lab)
    set(RAFT_CPP_FEATURES raft_test)
else()
    set(RAFT_CPP_VARIANT prod)
    set(RAFT_CPP_FEATURES "")
endif()
set(RAFT_CPP_ROOT "${CMAKE_BINARY_DIR}/raft-cpp-lane/${RAFT_CPP_VARIANT}")
set(RAFT_CPP_STAGE "${RAFT_CPP_ROOT}/crate")
set(RAFT_CPP_DIR "${RAFT_CPP_ROOT}/cpp")
set(RAFT_CPP_STAGE_ARGS
    "${CMAKE_SOURCE_DIR}/scripts/raft_cpp_stage.py"
    --crate "${RAFT_CPP_CRATE_DIR}"
    --rusty-facade "${CMAKE_SOURCE_DIR}/src/rusty-rustc"
    "--features=${RAFT_CPP_FEATURES}"
)

# The module set is a configure-time fact: stage once now to learn it, and
# reconfigure when lib.rs (which names the modules) changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${RAFT_CPP_CRATE_DIR}/src/lib.rs"
    "${CMAKE_SOURCE_DIR}/scripts/raft_cpp_stage.py")
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${RAFT_CPP_STAGE_ARGS} --out "${RAFT_CPP_STAGE}"
    RESULT_VARIABLE _raft_cpp_stage_rc
    OUTPUT_QUIET)
if(NOT _raft_cpp_stage_rc EQUAL 0)
    message(FATAL_ERROR "scripts/raft_cpp_stage.py failed (${_raft_cpp_stage_rc})")
endif()
file(STRINGS "${RAFT_CPP_STAGE}/modules.txt" RAFT_CPP_MODULE_NAMES)

set(RAFT_CPP_MODULES "${RAFT_CPP_DIR}/raft.cppm")
foreach(_m IN LISTS RAFT_CPP_MODULE_NAMES)
    list(APPEND RAFT_CPP_MODULES "${RAFT_CPP_DIR}/raft.${_m}.cppm")
endforeach()

# Every module sees, in its global module fragment, the lane's `rusty` facade
# and the global declaration of every kernel (raft_cpp_lane_kernels.h, which
# includes the facade; generated after transpilation, see raft_cpp_stage.py
# --kernels). The preamble is written from the same module list.
set(RAFT_CPP_KERNELS_H "${RAFT_CPP_ROOT}/include/raft_cpp_lane_kernels.h")
set(RAFT_CPP_PREAMBLE "${RAFT_CPP_ROOT}/module-preambles.toml")
set(_raft_cpp_preamble "version = 1\n")
foreach(_name IN ITEMS raft LISTS RAFT_CPP_MODULE_NAMES)
    if(NOT _name STREQUAL "raft")
        set(_name "raft.${_name}")
    endif()
    string(APPEND _raft_cpp_preamble
        "\n[[module]]\nname = \"${_name}\"\nincludes = [\n    { path = \"raft_cpp_lane_kernels.h\", form = \"quote\" },\n]\n")
endforeach()
file(CONFIGURE OUTPUT "${RAFT_CPP_PREAMBLE}" CONTENT "${_raft_cpp_preamble}")

file(GLOB RAFT_CPP_CANONICAL_RS CONFIGURE_DEPENDS "${RAFT_CPP_CRATE_DIR}/src/*.rs")
add_custom_command(
    OUTPUT ${RAFT_CPP_MODULES} "${RAFT_CPP_DIR}/rusty_hand_slots.md" "${RAFT_CPP_KERNELS_H}"
    COMMAND ${Python3_EXECUTABLE} ${RAFT_CPP_STAGE_ARGS} --out "${RAFT_CPP_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${RAFT_CPP_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RAFT_CPP_DIR}"
    COMMAND "${CMAKE_SOURCE_DIR}/third-party/rusty-cpp/target/release/rusty-cpp-transpiler"
        --crate "${RAFT_CPP_STAGE}/Cargo.toml"
        --output-dir "${RAFT_CPP_DIR}"
        --auto-namespace
        --module-preamble "${RAFT_CPP_PREAMBLE}"
        --type-map "${RAFT_CPP_CRATE_DIR}/cpp-lane-type-map.toml"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/scripts/raft_cpp_stage.py"
        --kernels "${RAFT_CPP_DIR}" "${RAFT_CPP_KERNELS_H}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/scripts/raft_cpp_stage.py"
        --gate "${RAFT_CPP_DIR}"
    DEPENDS
        ${RAFT_CPP_CANONICAL_RS}
        "${CMAKE_SOURCE_DIR}/scripts/raft_cpp_stage.py"
        "${CMAKE_SOURCE_DIR}/src/rusty-rustc/src/lib.rs"
        "${RAFT_CPP_PREAMBLE}"
        "${RAFT_CPP_CRATE_DIR}/cpp-lane-type-map.toml"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Transpiling the Raft core into the C++ lane (${RAFT_CPP_VARIANT})"
    VERBATIM
)

add_library(raft_cpp_core STATIC EXCLUDE_FROM_ALL)
target_sources(raft_cpp_core PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${RAFT_CPP_DIR}"
    FILES ${RAFT_CPP_MODULES}
)
target_compile_options(raft_cpp_core PRIVATE
    -O3 -DNDEBUG -march=native -std=gnu++23 -ferror-limit=0)
target_include_directories(raft_cpp_core PRIVATE
    "${RAFT_CPP_CRATE_DIR}" "${RAFT_CPP_ROOT}/include")
target_link_libraries(raft_cpp_core PUBLIC rusty)
