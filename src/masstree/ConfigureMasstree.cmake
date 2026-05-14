# CMake-native Masstree configuration.
#
# This replaces the historical autoconf/configure step by probing the
# toolchain/platform with CMake checks and generating config.h in the build
# tree.

include_guard(GLOBAL)

include(CheckIncludeFile)
include(CheckIncludeFileCXX)
include(CheckLibraryExists)
include(CheckSymbolExists)
include(CheckTypeSize)
include(CheckCXXSourceCompiles)
include(TestBigEndian)

set(MASSTREE_CONFIG_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/masstree")
set(MASSTREE_GENERATED_CONFIG_H "${MASSTREE_CONFIG_INCLUDE_DIR}/config.h")
file(MAKE_DIRECTORY "${MASSTREE_CONFIG_INCLUDE_DIR}")

function(_mako_set_01 OUT INVAR)
    if(${INVAR})
        set(${OUT} 1 PARENT_SCOPE)
    else()
        set(${OUT} 0 PARENT_SCOPE)
    endif()
endfunction()

function(_mako_check_cxx OUT SOURCE)
    check_cxx_source_compiles("${SOURCE}" _${OUT})
    if(_${OUT})
        set(${OUT} 1 PARENT_SCOPE)
    else()
        set(${OUT} 0 PARENT_SCOPE)
    endif()
endfunction()

function(_mako_check_same_type OUT TYPE_A TYPE_B INCLUDES)
    check_cxx_source_compiles("${INCLUDES}
#include <type_traits>
int main() {
    static_assert(std::is_same<${TYPE_A}, ${TYPE_B}>::value, \"types differ\");
    return 0;
}" _${OUT})
    if(_${OUT})
        set(${OUT} 1 PARENT_SCOPE)
    else()
        set(${OUT} 0 PARENT_SCOPE)
    endif()
endfunction()

# Header checks.
check_include_file("sys/epoll.h" _MASSTREE_HAVE_SYS_EPOLL_H)
_mako_set_01(HAVE_SYS_EPOLL_H _MASSTREE_HAVE_SYS_EPOLL_H)

check_include_file("numa.h" _MASSTREE_HAVE_NUMA_H)
_mako_set_01(HAVE_NUMA_H _MASSTREE_HAVE_NUMA_H)

check_include_file("inttypes.h" _MASSTREE_HAVE_INTTYPES_H)
_mako_set_01(HAVE_INTTYPES_H _MASSTREE_HAVE_INTTYPES_H)

check_include_file("stdint.h" _MASSTREE_HAVE_STDINT_H)
_mako_set_01(HAVE_STDINT_H _MASSTREE_HAVE_STDINT_H)

check_include_file("stdio.h" _MASSTREE_HAVE_STDIO_H)
_mako_set_01(HAVE_STDIO_H _MASSTREE_HAVE_STDIO_H)

check_include_file("stdlib.h" _MASSTREE_HAVE_STDLIB_H)
_mako_set_01(HAVE_STDLIB_H _MASSTREE_HAVE_STDLIB_H)

check_include_file("strings.h" _MASSTREE_HAVE_STRINGS_H)
_mako_set_01(HAVE_STRINGS_H _MASSTREE_HAVE_STRINGS_H)

check_include_file("string.h" _MASSTREE_HAVE_STRING_H)
_mako_set_01(HAVE_STRING_H _MASSTREE_HAVE_STRING_H)

check_include_file("sys/stat.h" _MASSTREE_HAVE_SYS_STAT_H)
_mako_set_01(HAVE_SYS_STAT_H _MASSTREE_HAVE_SYS_STAT_H)

check_include_file("sys/types.h" _MASSTREE_HAVE_SYS_TYPES_H)
_mako_set_01(HAVE_SYS_TYPES_H _MASSTREE_HAVE_SYS_TYPES_H)

check_include_file("time.h" _MASSTREE_HAVE_TIME_H)
_mako_set_01(HAVE_TIME_H _MASSTREE_HAVE_TIME_H)

check_include_file("execinfo.h" _MASSTREE_HAVE_EXECINFO_H)
_mako_set_01(HAVE_EXECINFO_H _MASSTREE_HAVE_EXECINFO_H)

check_include_file("unistd.h" _MASSTREE_HAVE_UNISTD_H)
_mako_set_01(HAVE_UNISTD_H _MASSTREE_HAVE_UNISTD_H)

check_include_file_cxx("type_traits" _MASSTREE_HAVE_TYPE_TRAITS)
_mako_set_01(HAVE_TYPE_TRAITS _MASSTREE_HAVE_TYPE_TRAITS)

# Symbol / function checks.
check_symbol_exists(getline "stdio.h" _MASSTREE_HAVE_DECL_GETLINE)
_mako_set_01(HAVE_DECL_GETLINE _MASSTREE_HAVE_DECL_GETLINE)

check_symbol_exists(clock_gettime "time.h" _MASSTREE_HAVE_DECL_CLOCK_GETTIME)
_mako_set_01(HAVE_DECL_CLOCK_GETTIME _MASSTREE_HAVE_DECL_CLOCK_GETTIME)

set(_MASSTREE_CLOCK_GETTIME_SOURCE
"#include <time.h>
int main() {
    struct timespec ts;
    return clock_gettime(CLOCK_REALTIME, &ts);
}")
check_cxx_source_compiles("${_MASSTREE_CLOCK_GETTIME_SOURCE}" _MASSTREE_HAVE_CLOCK_GETTIME)
if(NOT _MASSTREE_HAVE_CLOCK_GETTIME)
    set(_MASSTREE_REQUIRED_LIBRARIES_SAVE "${CMAKE_REQUIRED_LIBRARIES}")
    set(CMAKE_REQUIRED_LIBRARIES rt)
    check_cxx_source_compiles("${_MASSTREE_CLOCK_GETTIME_SOURCE}" _MASSTREE_HAVE_CLOCK_GETTIME_WITH_RT)
    set(CMAKE_REQUIRED_LIBRARIES "${_MASSTREE_REQUIRED_LIBRARIES_SAVE}")
    if(_MASSTREE_HAVE_CLOCK_GETTIME_WITH_RT)
        set(_MASSTREE_HAVE_CLOCK_GETTIME TRUE)
    endif()
endif()
_mako_set_01(HAVE_CLOCK_GETTIME _MASSTREE_HAVE_CLOCK_GETTIME)

check_library_exists(numa numa_available "" _MASSTREE_HAVE_LIBNUMA)
_mako_set_01(HAVE_LIBNUMA _MASSTREE_HAVE_LIBNUMA)

# C++ language/library feature checks.
_mako_check_cxx(HAVE_CXX_CONSTEXPR [[
constexpr int f(int x) { return x + 1; }
int main() { return f(2); }
]])

_mako_check_cxx(HAVE_CXX_STATIC_ASSERT [[
const int f = 2;
int main() {
    static_assert(f == 2, "f should be 2");
    return 0;
}
]])

_mako_check_cxx(HAVE_CXX_RVALUE_REFERENCES [[
int f(int&) { return 1; }
int f(int&&) { return 0; }
int main() { return f(int()); }
]])

_mako_check_cxx(HAVE_CXX_TEMPLATE_ALIAS [[
template <typename T> struct X { typedef T type; };
template <typename T> using Y = X<T>;
int f(int x) { return x; }
int main() { return f(Y<int>::type()); }
]])

_mako_check_cxx(HAVE_STD_HASH [[
#include <functional>
#include <cstddef>
int main() {
    std::hash<int> h;
    std::size_t x = h(1);
    return static_cast<int>(x == 0);
}
]])

_mako_check_cxx(HAVE_STD_IS_TRIVIALLY_COPYABLE [[
#include <type_traits>
int main() { return std::is_trivially_copyable<int>::value ? 0 : 1; }
]])

_mako_check_cxx(HAVE_STD_IS_TRIVIALLY_DESTRUCTIBLE [[
#include <type_traits>
int main() { return std::is_trivially_destructible<int>::value ? 0 : 1; }
]])

_mako_check_cxx(HAVE_STD_IS_RVALUE_REFERENCE [[
#include <type_traits>
int main() { return std::is_rvalue_reference<int>::value ? 0 : 1; }
]])

_mako_check_cxx(HAVE___HAS_TRIVIAL_COPY [[
int main() { return __has_trivial_copy(long) ? 0 : 1; }
]])

_mako_check_cxx(HAVE___HAS_TRIVIAL_DESTRUCTOR [[
int main() { return __has_trivial_destructor(long) ? 0 : 1; }
]])

# Builtin checks.
_mako_check_cxx(HAVE___BUILTIN_CLZ [[
int main() { return __builtin_clz(1U); }
]])

_mako_check_cxx(HAVE___BUILTIN_CLZL [[
int main() { return static_cast<int>(__builtin_clzl(1UL)); }
]])

_mako_check_cxx(HAVE___BUILTIN_CLZLL [[
int main() { return static_cast<int>(__builtin_clzll(1ULL)); }
]])

_mako_check_cxx(HAVE___BUILTIN_CTZ [[
int main() { return __builtin_ctz(1U); }
]])

_mako_check_cxx(HAVE___BUILTIN_CTZL [[
int main() { return static_cast<int>(__builtin_ctzl(1UL)); }
]])

_mako_check_cxx(HAVE___BUILTIN_CTZLL [[
int main() { return static_cast<int>(__builtin_ctzll(1ULL)); }
]])

_mako_check_cxx(HAVE___SYNC_SYNCHRONIZE [[
int main() { __sync_synchronize(); return 0; }
]])

_mako_check_cxx(HAVE___SYNC_FETCH_AND_ADD [[
int main() {
    long x = 0;
    return static_cast<int>(__sync_fetch_and_add(&x, 2L));
}
]])

_mako_check_cxx(HAVE___SYNC_ADD_AND_FETCH [[
int main() {
    long x = 0;
    return static_cast<int>(__sync_add_and_fetch(&x, 2L));
}
]])

_mako_check_cxx(HAVE___SYNC_FETCH_AND_ADD_8 [[
#include <cstdint>
int main() {
    int64_t x = 0;
    return static_cast<int>(__sync_fetch_and_add(&x, static_cast<int64_t>(2)));
}
]])

_mako_check_cxx(HAVE___SYNC_ADD_AND_FETCH_8 [[
#include <cstdint>
int main() {
    int64_t x = 0;
    return static_cast<int>(__sync_add_and_fetch(&x, static_cast<int64_t>(2)));
}
]])

_mako_check_cxx(HAVE___SYNC_FETCH_AND_OR [[
int main() {
    long x = 0;
    return static_cast<int>(__sync_fetch_and_or(&x, 2L));
}
]])

_mako_check_cxx(HAVE___SYNC_OR_AND_FETCH [[
int main() {
    long x = 0;
    return static_cast<int>(__sync_or_and_fetch(&x, 2L));
}
]])

_mako_check_cxx(HAVE___SYNC_FETCH_AND_OR_8 [[
#include <cstdint>
int main() {
    int64_t x = 0;
    return static_cast<int>(__sync_fetch_and_or(&x, static_cast<int64_t>(2)));
}
]])

_mako_check_cxx(HAVE___SYNC_OR_AND_FETCH_8 [[
#include <cstdint>
int main() {
    int64_t x = 0;
    return static_cast<int>(__sync_or_and_fetch(&x, static_cast<int64_t>(2)));
}
]])

_mako_check_cxx(HAVE___SYNC_BOOL_COMPARE_AND_SWAP [[
int main() {
    long x = 0;
    return __sync_bool_compare_and_swap(&x, 0L, 1L) ? 0 : 1;
}
]])

_mako_check_cxx(HAVE___SYNC_BOOL_COMPARE_AND_SWAP_8 [[
#include <cstdint>
int main() {
    int64_t x = 0;
    return __sync_bool_compare_and_swap(&x, static_cast<int64_t>(0), static_cast<int64_t>(1)) ? 0 : 1;
}
]])

_mako_check_cxx(HAVE___SYNC_VAL_COMPARE_AND_SWAP [[
int main() {
    long x = 0;
    return static_cast<int>(__sync_val_compare_and_swap(&x, 0L, 1L));
}
]])

_mako_check_cxx(HAVE___SYNC_VAL_COMPARE_AND_SWAP_8 [[
#include <cstdint>
int main() {
    int64_t x = 0;
    return static_cast<int>(__sync_val_compare_and_swap(&x, static_cast<int64_t>(0), static_cast<int64_t>(1)));
}
]])

_mako_check_cxx(HAVE___SYNC_LOCK_TEST_AND_SET [[
int main() {
    long x = 0;
    return static_cast<int>(__sync_lock_test_and_set(&x, 1L));
}
]])

_mako_check_cxx(HAVE___SYNC_LOCK_TEST_AND_SET_VAL [[
int main() {
    long x = 0;
    long y = 2;
    return static_cast<int>(__sync_lock_test_and_set(&x, y));
}
]])

_mako_check_cxx(HAVE___SYNC_LOCK_RELEASE_SET [[
int main() {
    long x = 0;
    __sync_lock_release(&x);
    return 0;
}
]])

# Same-type checks.
_mako_check_same_type(HAVE_OFF_T_IS_LONG "off_t" "long" [[#include <sys/types.h>]])
_mako_check_same_type(HAVE_OFF_T_IS_LONG_LONG "off_t" "long long" [[#include <sys/types.h>]])
_mako_check_same_type(HAVE_INT64_T_IS_LONG "int64_t" "long" [[#include <cstdint>]])
_mako_check_same_type(HAVE_INT64_T_IS_LONG_LONG "int64_t" "long long" [[#include <cstdint>]])
_mako_check_same_type(HAVE_SIZE_T_IS_UNSIGNED "size_t" "unsigned" [[#include <cstddef>]])
_mako_check_same_type(HAVE_SIZE_T_IS_UNSIGNED_LONG "size_t" "unsigned long" [[#include <cstddef>]])
_mako_check_same_type(HAVE_SIZE_T_IS_UNSIGNED_LONG_LONG "size_t" "unsigned long long" [[#include <cstddef>]])

# Type size checks.
check_type_size("short" SIZEOF_SHORT)
check_type_size("int" SIZEOF_INT)
check_type_size("long" SIZEOF_LONG)
check_type_size("long long" SIZEOF_LONG_LONG)
check_type_size("void *" SIZEOF_VOID_P)

if(NOT SIZEOF_SHORT)
    set(SIZEOF_SHORT 2)
endif()
if(NOT SIZEOF_INT)
    set(SIZEOF_INT 4)
endif()
if(NOT SIZEOF_LONG)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(SIZEOF_LONG 8)
    else()
        set(SIZEOF_LONG 4)
    endif()
endif()
if(NOT SIZEOF_LONG_LONG)
    set(SIZEOF_LONG_LONG 8)
endif()
if(NOT SIZEOF_VOID_P)
    set(SIZEOF_VOID_P "${CMAKE_SIZEOF_VOID_P}")
endif()

if(DEFINED SIZEOF_LONG_LONG AND NOT "${SIZEOF_LONG_LONG}" STREQUAL "" AND NOT "${SIZEOF_LONG_LONG}" STREQUAL "0")
    set(HAVE_LONG_LONG 1)
else()
    set(HAVE_LONG_LONG 0)
endif()

# Platform checks.
_mako_check_cxx(HAVE_MADV_HUGEPAGE [[
#include <sys/mman.h>
#ifndef MADV_HUGEPAGE
#error "MADV_HUGEPAGE not available"
#endif
int main() { return MADV_HUGEPAGE; }
]])

_mako_check_cxx(HAVE_MAP_HUGETLB [[
#include <sys/mman.h>
#ifndef MAP_HUGETLB
#error "MAP_HUGETLB not available"
#endif
int main() { return MAP_HUGETLB; }
]])

test_big_endian(_MASSTREE_WORDS_BIGENDIAN)
if(_MASSTREE_WORDS_BIGENDIAN)
    set(WORDS_BIGENDIAN 1)
else()
    set(WORDS_BIGENDIAN 0)
endif()
set(WORDS_BIGENDIAN_SET 1)

if(MASSTREE_ENABLE_SUPERPAGE AND (HAVE_MADV_HUGEPAGE OR HAVE_MAP_HUGETLB))
    set(HAVE_SUPERPAGE 1)
else()
    set(HAVE_SUPERPAGE 0)
endif()

# Keep historical defaults.
set(CACHE_LINE_SIZE 64)
set(HAVE_UNALIGNED_ACCESS 1)
set(HAVE_MEMDEBUG 0)

if(DEBUG)
    set(ENABLE_ASSERTIONS 1)
else()
    set(ENABLE_ASSERTIONS 0)
endif()
if(CHECK_INVARIANTS)
    set(ENABLE_INVARIANTS 1)
    set(ENABLE_PRECONDITIONS 1)
else()
    set(ENABLE_INVARIANTS 0)
    set(ENABLE_PRECONDITIONS 0)
endif()

set(MASSTREE_MAXKEYLEN "${MASSTREE_MAX_KEY_LEN}")

set(MASSTREE_ROW_TYPE_ARRAY 0)
set(MASSTREE_ROW_TYPE_ARRAY_VER 0)
set(MASSTREE_ROW_TYPE_BAG 0)
set(MASSTREE_ROW_TYPE_STR 0)
string(TOLOWER "${MASSTREE_ROW_TYPE}" _MASSTREE_ROW_TYPE_LC)
set(_MASSTREE_ROW_TYPE_VALUES bag array array_ver str)
list(FIND _MASSTREE_ROW_TYPE_VALUES "${_MASSTREE_ROW_TYPE_LC}" _MASSTREE_ROW_TYPE_INDEX)
if(_MASSTREE_ROW_TYPE_INDEX EQUAL -1)
    message(FATAL_ERROR "Invalid MASSTREE_ROW_TYPE='${MASSTREE_ROW_TYPE}'. Expected one of: bag, array, array_ver, str")
endif()
if(_MASSTREE_ROW_TYPE_LC STREQUAL "array")
    set(MASSTREE_ROW_TYPE_ARRAY 1)
elseif(_MASSTREE_ROW_TYPE_LC STREQUAL "array_ver")
    set(MASSTREE_ROW_TYPE_ARRAY_VER 1)
elseif(_MASSTREE_ROW_TYPE_LC STREQUAL "str")
    set(MASSTREE_ROW_TYPE_STR 1)
else()
    set(MASSTREE_ROW_TYPE_BAG 1)
endif()

set(HAVE_FLOW_MALLOC 0)
set(HAVE_HOARD_MALLOC 0)
set(HAVE_JEMALLOC 0)
set(HAVE_TCMALLOC 0)
if(USE_MALLOC_MODE EQUAL 1)
    set(HAVE_JEMALLOC 1)
elseif(USE_MALLOC_MODE EQUAL 2)
    set(HAVE_TCMALLOC 1)
elseif(USE_MALLOC_MODE EQUAL 3)
    set(HAVE_FLOW_MALLOC 1)
endif()

if(HAVE_STDIO_H AND HAVE_STDLIB_H AND HAVE_STRING_H AND HAVE_INTTYPES_H
   AND HAVE_STDINT_H AND HAVE_SYS_TYPES_H AND HAVE_SYS_STAT_H AND HAVE_UNISTD_H)
    set(STDC_HEADERS 1)
else()
    set(STDC_HEADERS 0)
endif()

set(MASSTREE_PACKAGE_BUGREPORT "")
set(MASSTREE_PACKAGE_NAME "masstree-beta")
set(MASSTREE_PACKAGE_STRING "masstree-beta 0.1")
set(MASSTREE_PACKAGE_TARNAME "masstree-beta")
set(MASSTREE_PACKAGE_URL "")
set(MASSTREE_PACKAGE_VERSION "0.1")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/masstree/config-cmake.h.in"
    "${MASSTREE_GENERATED_CONFIG_H}"
    @ONLY
)
message(STATUS "Generated Masstree config.h via CMake: ${MASSTREE_GENERATED_CONFIG_H}")
