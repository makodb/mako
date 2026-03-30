#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "hdr_histogram::hdr_histogram" for configuration "RelWithDebInfo"
set_property(TARGET hdr_histogram::hdr_histogram APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(hdr_histogram::hdr_histogram PROPERTIES
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/libhdr_histogram.so.6.1.1"
  IMPORTED_SONAME_RELWITHDEBINFO "libhdr_histogram.so.6"
  )

list(APPEND _IMPORT_CHECK_TARGETS hdr_histogram::hdr_histogram )
list(APPEND _IMPORT_CHECK_FILES_FOR_hdr_histogram::hdr_histogram "${_IMPORT_PREFIX}/lib/libhdr_histogram.so.6.1.1" )

# Import target "hdr_histogram::hdr_histogram_static" for configuration "RelWithDebInfo"
set_property(TARGET hdr_histogram::hdr_histogram_static APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(hdr_histogram::hdr_histogram_static PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELWITHDEBINFO "C"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/libhdr_histogram_static.a"
  )

list(APPEND _IMPORT_CHECK_TARGETS hdr_histogram::hdr_histogram_static )
list(APPEND _IMPORT_CHECK_FILES_FOR_hdr_histogram::hdr_histogram_static "${_IMPORT_PREFIX}/lib/libhdr_histogram_static.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
