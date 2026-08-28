cmake_minimum_required(VERSION 3.30)

if(NOT DEFINED MAKO_NM OR "${MAKO_NM}" STREQUAL "")
    message(FATAL_ERROR "CheckMtreeAbiExports: MAKO_NM is required")
endif()
if(NOT DEFINED MAKO_ABI_ARTIFACT OR NOT EXISTS "${MAKO_ABI_ARTIFACT}")
    message(FATAL_ERROR
        "CheckMtreeAbiExports: ABI artifact does not exist: ${MAKO_ABI_ARTIFACT}")
endif()

# `-P` selects POSIX output on GNU nm and llvm-nm. Each symbol is therefore
# the first whitespace-delimited field; Mach-O's conventional leading
# underscore is normalized below.
execute_process(
    COMMAND "${MAKO_NM}" -g -P "${MAKO_ABI_ARTIFACT}"
    OUTPUT_VARIABLE _nm_output
    ERROR_VARIABLE _nm_error
    RESULT_VARIABLE _nm_result
)
if(NOT _nm_result EQUAL 0)
    message(FATAL_ERROR
        "nm failed for ${MAKO_ABI_ARTIFACT} (${_nm_result}): ${_nm_error}")
endif()

string(REPLACE "\r\n" "\n" _nm_output "${_nm_output}")
string(REPLACE "\n" ";" _nm_lines "${_nm_output}")
set(_actual)
foreach(_line IN LISTS _nm_lines)
    string(STRIP "${_line}" _line)
    string(REGEX MATCH
        "(^|.*:)[ \t]*(_?mt_[A-Za-z0-9_]+)[ \t]+([A-Za-z?])([ \t]|$)"
        _match "${_line}")
    if(_match AND NOT CMAKE_MATCH_3 STREQUAL "U")
        set(_symbol "${CMAKE_MATCH_2}")
        string(REGEX REPLACE "^_" "" _symbol "${_symbol}")
        list(APPEND _actual "${_symbol}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _actual)
list(SORT _actual)

set(_expected
    mt_abi_version
    mt_build_id_alignment
    mt_build_id_size
    mt_endianness
    mt_exported_symbols_fingerprint
    mt_feature_bits
    mt_get
    mt_get_build_fingerprint
    mt_get_or_insert
    mt_get_or_insert_result_alignment
    mt_get_or_insert_result_size
    mt_get_strided
    mt_max_key_length
    mt_max_threads
    mt_pointer_width
    mt_record_id_limit
    mt_read_scope_alignment
    mt_read_scope_begin
    mt_read_scope_end
    mt_read_scope_get
    mt_read_scope_get_strided
    mt_read_scope_size
    mt_runtime_acquire
    mt_runtime_config_alignment
    mt_runtime_config_init
    mt_runtime_config_size
    mt_runtime_health
    mt_runtime_max_key_length
    mt_runtime_max_threads
    mt_runtime_shutdown
    mt_scan
    mt_scan_bound_alignment
    mt_scan_bound_size
    mt_scan_entry_alignment
    mt_scan_entry_size
    mt_scan_result_alignment
    mt_scan_result_size
    mt_thread_attach
    mt_thread_quiesce
    mt_tree_create
    mt_tree_release
)
list(SORT _expected)

set(_missing ${_expected})
foreach(_symbol IN LISTS _actual)
    list(REMOVE_ITEM _missing "${_symbol}")
endforeach()
set(_unexpected ${_actual})
foreach(_symbol IN LISTS _expected)
    list(REMOVE_ITEM _unexpected "${_symbol}")
endforeach()

if(_missing OR _unexpected)
    list(JOIN _missing ", " _missing_text)
    list(JOIN _unexpected ", " _unexpected_text)
    message(FATAL_ERROR
        "Masstree ABI exported-symbol mismatch\n"
        "  missing: ${_missing_text}\n"
        "  unexpected: ${_unexpected_text}")
endif()

list(LENGTH _actual _actual_count)
if(NOT _actual_count EQUAL 41)
    message(FATAL_ERROR
        "Masstree ABI must export exactly 41 mt_* symbols, found ${_actual_count}")
endif()
message(STATUS
    "Masstree ABI exports exactly the 41 allowlisted mt_* symbols")
