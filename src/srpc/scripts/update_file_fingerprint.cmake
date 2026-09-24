cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED INPUT OR INPUT STREQUAL "")
    message(FATAL_ERROR "update_file_fingerprint.cmake requires -DINPUT=<file>")
endif()
if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
    message(FATAL_ERROR "update_file_fingerprint.cmake requires -DOUTPUT=<file>")
endif()
if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "fingerprint input does not exist: ${INPUT}")
endif()

# Include both content and build identity. A reproducible emitter rebuild can
# produce identical bytes, while its new timestamp still represents a build
# edge that downstream generated sources must observe.
file(SHA256 "${INPUT}" _fingerprint_sha256)
file(SIZE "${INPUT}" _fingerprint_size)
file(TIMESTAMP "${INPUT}" _fingerprint_timestamp "%s.%f" UTC)
set(_fingerprint
    "sha256=${_fingerprint_sha256}\nsize=${_fingerprint_size}\nmtime=${_fingerprint_timestamp}\n")

set(_old_fingerprint "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" _old_fingerprint)
endif()
if(NOT _old_fingerprint STREQUAL _fingerprint)
    get_filename_component(_output_directory "${OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_output_directory}")
    file(WRITE "${OUTPUT}" "${_fingerprint}")
endif()
