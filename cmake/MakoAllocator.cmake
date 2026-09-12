include_guard(GLOBAL)

function(_mako_allocator_custom_library_hints output_variable)
  set(_hints)
  if(DEFINED CUSTOM_LDPATH AND NOT "${CUSTOM_LDPATH}" STREQUAL "")
    string(JOIN " " _custom_ldpath ${CUSTOM_LDPATH})
    separate_arguments(_custom_link_args NATIVE_COMMAND "${_custom_ldpath}")
    set(_expect_directory OFF)
    foreach(_argument IN LISTS _custom_link_args)
      if(_expect_directory)
        list(APPEND _hints "${_argument}")
        set(_expect_directory OFF)
      elseif(_argument STREQUAL "-L")
        set(_expect_directory ON)
      elseif(_argument MATCHES "^-L(.+)$")
        list(APPEND _hints "${CMAKE_MATCH_1}")
      endif()
    endforeach()
    if(_expect_directory)
      message(FATAL_ERROR "CUSTOM_LDPATH ends with -L and no directory")
    endif()
  endif()
  set(${output_variable} "${_hints}" PARENT_SCOPE)
endfunction()

function(_mako_allocator_pkg_hints package output_variable)
  set(_hints)
  if(package STREQUAL "jemalloc")
    pkg_check_modules(_MAKO_ALLOCATOR_PKG QUIET jemalloc)
  elseif(package STREQUAL "tcmalloc")
    pkg_check_modules(_MAKO_ALLOCATOR_PKG QUIET libtcmalloc)
    if(NOT _MAKO_ALLOCATOR_PKG_FOUND)
      pkg_check_modules(_MAKO_ALLOCATOR_PKG QUIET tcmalloc)
    endif()
  elseif(package STREQUAL "flow")
    pkg_check_modules(_MAKO_ALLOCATOR_PKG QUIET flow)
  else()
    message(FATAL_ERROR "unsupported allocator package ${package}")
  endif()
  if(_MAKO_ALLOCATOR_PKG_FOUND)
    list(APPEND _hints ${_MAKO_ALLOCATOR_PKG_LIBRARY_DIRS})
  endif()
  set(${output_variable} "${_hints}" PARENT_SCOPE)
endfunction()

function(mako_configure_allocator)
  if(TARGET mako_allocator)
    message(FATAL_ERROR "mako allocator target was configured more than once")
  endif()
  if(NOT "${USE_MALLOC_MODE}" MATCHES "^[0-3]$")
    message(FATAL_ERROR
      "USE_MALLOC_MODE must be exactly 0, 1, 2, or 3; got '${USE_MALLOC_MODE}'")
  endif()

  add_library(mako_allocator INTERFACE)

  set(_kind system)
  set(_linkage none)
  set(_link_name "")
  set(_library_path "")
  set(_soname "")
  set(_sha256 "")

  if(USE_MALLOC_MODE EQUAL 1)
    set(_kind jemalloc)
    set(_link_name jemalloc)
  elseif(USE_MALLOC_MODE EQUAL 2)
    set(_kind tcmalloc)
    set(_link_name tcmalloc)
  elseif(USE_MALLOC_MODE EQUAL 3)
    set(_kind flow)
    set(_link_name flow)
  endif()

  if(NOT USE_MALLOC_MODE EQUAL 0)
    _mako_allocator_custom_library_hints(_custom_hints)
    _mako_allocator_pkg_hints("${_kind}" _pkg_hints)
    set(_library_hints ${_custom_hints} ${_pkg_hints})
    list(REMOVE_DUPLICATES _library_hints)

    find_library(_resolved_library
      NAMES "${_link_name}"
      HINTS ${_library_hints}
      NO_CACHE
      REQUIRED)
    file(REAL_PATH "${_resolved_library}" _library_path)
    if(NOT EXISTS "${_library_path}" OR IS_DIRECTORY "${_library_path}")
      message(FATAL_ERROR
        "resolved ${_kind} allocator is not a library file: ${_library_path}")
    endif()
    get_filename_component(_library_filename "${_library_path}" NAME)
    if(NOT _library_filename MATCHES "\\.so(\\..+)?$")
      message(FATAL_ERROR
        "USE_MALLOC_MODE=${USE_MALLOC_MODE} requires a shared ELF allocator, "
        "but ${_resolved_library} resolves to ${_library_path}")
    endif()
    if(NOT CMAKE_READELF)
      find_program(CMAKE_READELF NAMES llvm-readelf readelf REQUIRED)
    endif()
    execute_process(
      COMMAND "${CMAKE_READELF}" -d "${_library_path}"
      RESULT_VARIABLE _readelf_status
      OUTPUT_VARIABLE _readelf_output
      ERROR_VARIABLE _readelf_error)
    if(NOT _readelf_status EQUAL 0)
      message(FATAL_ERROR
        "cannot inspect ${_kind} allocator SONAME at ${_library_path}: "
        "${_readelf_error}")
    endif()
    string(REGEX MATCH "SONAME[^[]*\\[([^]]+)\\]" _soname_match
      "${_readelf_output}")
    set(_soname "${CMAKE_MATCH_1}")
    if(_soname STREQUAL "" OR _soname MATCHES "[/\\\\\r\n]")
      message(FATAL_ERROR
        "resolved ${_kind} allocator has no safe ELF SONAME: ${_library_path}")
    endif()
    get_filename_component(_library_directory "${_library_path}" DIRECTORY)
    set(_runtime_spelling "${_library_directory}/${_soname}")
    if(NOT EXISTS "${_runtime_spelling}" OR IS_DIRECTORY "${_runtime_spelling}")
      message(FATAL_ERROR
        "resolved ${_kind} allocator SONAME ${_soname} is unavailable beside "
        "${_library_path}")
    endif()
    file(REAL_PATH "${_runtime_spelling}" _runtime_target)
    if(NOT "${_runtime_target}" STREQUAL "${_library_path}")
      message(FATAL_ERROR
        "resolved ${_kind} allocator SONAME ${_runtime_spelling} reaches "
        "${_runtime_target}, not ${_library_path}")
    endif()
    file(SHA256 "${_library_path}" _sha256)
    set(_linkage shared)
    target_link_libraries(mako_allocator INTERFACE "${_library_path}")
  endif()

  foreach(_field IN ITEMS
      "${_kind}" "${_linkage}" "${_link_name}" "${_library_path}"
      "${_soname}" "${_sha256}")
    if(_field MATCHES "[\r\n]")
      message(FATAL_ERROR "allocator metadata contains a newline")
    endif()
  endforeach()

  set(_metadata
    "${CMAKE_BINARY_DIR}/generated/mako_allocator_contract.txt")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
  string(CONCAT _contract
    "schema=1\n"
    "mode=${USE_MALLOC_MODE}\n"
    "kind=${_kind}\n"
    "linkage=${_linkage}\n"
    "link_name=${_link_name}\n"
    "library_path=${_library_path}\n"
    "soname=${_soname}\n"
    "sha256=${_sha256}\n")
  file(GENERATE OUTPUT "${_metadata}" CONTENT "${_contract}")

  set_property(TARGET mako_allocator PROPERTY
    MAKO_ALLOCATOR_MODE "${USE_MALLOC_MODE}")
  set_property(TARGET mako_allocator PROPERTY
    MAKO_ALLOCATOR_KIND "${_kind}")
  set_property(TARGET mako_allocator PROPERTY
    MAKO_ALLOCATOR_LIBRARY "${_library_path}")

  set(MAKO_ALLOCATOR_METADATA "${_metadata}" PARENT_SCOPE)
  set(MAKO_ALLOCATOR_LIBRARY "${_library_path}" PARENT_SCOPE)
  set(MAKO_ALLOCATOR_KIND "${_kind}" PARENT_SCOPE)
  set(MAKO_ALLOCATOR_LINK_NAME "${_link_name}" PARENT_SCOPE)
  set(MAKO_ALLOCATOR_SONAME "${_soname}" PARENT_SCOPE)
  message(STATUS
    "Mako allocator: mode=${USE_MALLOC_MODE}, kind=${_kind}, "
    "library=${_library_path}")
endfunction()
